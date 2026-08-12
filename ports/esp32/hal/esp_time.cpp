/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * HAL backend: time, ESP32 implementation.
 *
 * kf_time_mono_us() is real: esp_timer_get_time(), the same monotonic
 * microsecond clock esp_timer's own docs point to, ticking from a hardware
 * timer that survives light sleep (irrelevant here yet -- kf/hal/power.h's
 * only sleep call is deep sleep, which resets the chip and this clock
 * along with it, exactly as kf/hal/time.h's own header comment warns).
 *
 * kf_time_wall() IS now backed by a real DS3231 (ADR 0026) -- this comment
 * used to say it wasn't; closing that gap is what this slice does.
 * kf_time_init() below tries once, at boot, to read the DS3231 over I2C:
 * same bus config, same pins (kf_esp_pins.h), same register map as
 * ports/esp32-bringup/main/bringup_main.cpp's stage_i2c_and_rtc(), the only
 * code in this repo that has actually talked to this exact chip -- this
 * driver's register map was checked directly against that file's source,
 * not against memory of it. Every failure path (bus init fails, nothing
 * answers at 0x68, the OSF flag says the oscillator has stopped since it
 * was last set) leaves the wall clock exactly as unset as it is today, on
 * a device with no RTC wired at all -- never a crash, never a hang, never
 * a silently wrong pet age. This driver deliberately does NOT auto-seed a
 * fake time the way the bring-up diagnostic does on OSF: that is a
 * human-supervised, one-time bring-up action, not something that should
 * happen quietly on every boot with a dead coin cell.
 *
 * kf_time_set_wall() now writes through to the DS3231 as well as the
 * in-RAM clock, when a chip answered at boot -- matching this header's own
 * documented contract ("Used by whatever configures the RTC: a settings
 * screen, an NTP sync, a companion app"). A write that only updated RAM
 * would silently undo the entire point of a battery-backed chip: the next
 * genuine power-off would forget the correction. This DOES have a
 * production caller now -- the Lua Settings screen, via
 * kf_lua_port_apply_clock() (sdk/lua/kf_lua_port.cpp). An earlier version
 * of this comment said nothing outside tests called it; that was true when
 * written and stopped being true when the Settings screen landed.
 *
 * RUN AGAINST REAL HARDWARE AND CONFIRMED, 2026-08-11: a board with a
 * DS3231 on GPIO13/14 was set from the Settings screen, unplugged from USB
 * for roughly a minute, and re-read at the next boot. The clock had
 * advanced 117 seconds across the power cut with OSF still clear -- the
 * coin cell carried it. Three consecutive boot reads were monotonic and
 * tracked real elapsed time. See
 * docs/architecture/adr-0026-ds3231-rtc-driver.md for the full result and
 * for what remains unverified (the OSF and wrong-chip failure paths have
 * still never been exercised on a real board).
 */

#include "kf/hal/time.h"

#include "kf/hal/log.h"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/i2c_master.h"
#include "kf_esp_pins.h"
#include "kf_esp_time_debug.h"

#include <cstring>
#include <sys/time.h>

namespace {

constexpr const char *TAG = "time";

bool g_wall_valid = false;
i2c_master_bus_handle_t g_i2c_bus = nullptr;
i2c_master_dev_handle_t g_rtc_dev = nullptr; /* null until a DS3231 answers */

/* DS3231 register map, verified directly against
 * ports/esp32-bringup/main/bringup_main.cpp's stage_i2c_and_rtc() -- the
 * only code in this repo that has actually talked to this chip:
 *   0x00 seconds  0x01 minutes  0x02 hours  0x03 day-of-week
 *   0x04 date     0x05 month    0x06 year   0x0F status (bit 7 = OSF)
 * Hours register bit 6 selects 12h/24h mode; this driver always writes
 * with that bit clear (24h mode), matching the bring-up seed's own "hours,
 * 24h mode (bit 6 clear)" comment, so reads only ever need to mask it off,
 * never branch on it. Month register bit 7 is the century bit, likewise
 * always written clear -- this driver only ever runs in the 2000-2099 span
 * the DS3231 (and this project) will exist for. Register 0x11 (temperature)
 * is not part of the time-keeping registers at all -- see
 * try_init_ds3231()'s use of it below. */
constexpr uint8_t kRegTemp = 0x11;
constexpr uint8_t kRegTime = 0x00;
constexpr uint8_t kRegStatus = 0x0F;
constexpr uint8_t kStatusOsfBit = 0x80;

uint8_t bcd_to_bin(uint8_t v) {
    return static_cast<uint8_t>((v >> 4) * 10 + (v & 0x0F));
}

uint8_t bin_to_bcd(uint8_t v) {
    return static_cast<uint8_t>(((v / 10) << 4) | (v % 10));
}

bool ds3231_read(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *out,
                  size_t len) {
    return i2c_master_transmit_receive(dev, &reg, 1, out, len, 200) == ESP_OK;
}

bool ds3231_write(i2c_master_dev_handle_t dev, uint8_t reg,
                   const uint8_t *data, size_t len) {
    uint8_t buf[8];
    if (len + 1 > sizeof(buf)) {
        return false;
    }
    buf[0] = reg;
    memcpy(&buf[1], data, len);
    return i2c_master_transmit(dev, buf, len + 1, 200) == ESP_OK;
}

/* Days since 1970-01-01 for a proleptic-Gregorian civil date, and its
 * inverse. Howard Hinnant's well-known days_from_civil/civil_from_days
 * algorithm -- used instead of mktime()/timegm() because ESP-IDF's default
 * newlib-nano does not reliably carry timegm(), and this needs no libc
 * calendar support at all: pure integer math that host-compiles and
 * behaves identically to however it runs on-device.
 * https://howardhinnant.github.io/date_algorithms.html */
int64_t days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy =
        (153u * static_cast<unsigned>(m + (m > 2 ? -3 : 9)) + 2u) / 5u +
        static_cast<unsigned>(d) - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

void civil_from_days(int64_t z, int *y, int *m, int *d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t yr = static_cast<int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    const unsigned mp = (5u * doy + 2u) / 153u;
    *d = static_cast<int>(doy - (153u * mp + 2u) / 5u + 1u);
    *m = static_cast<int>(mp + (mp < 10 ? 3 : -9));
    *y = static_cast<int>(yr + (*m <= 2 ? 1 : 0));
}

int64_t ds3231_regs_to_epoch(const uint8_t regs[7]) {
    const int seconds = bcd_to_bin(static_cast<uint8_t>(regs[0] & 0x7F));
    const int minutes = bcd_to_bin(regs[1]);
    const int hours = bcd_to_bin(static_cast<uint8_t>(regs[2] & 0x3F));
    const int date = bcd_to_bin(regs[4]);
    const int month = bcd_to_bin(static_cast<uint8_t>(regs[5] & 0x1F));
    const int year = 2000 + bcd_to_bin(regs[6]);

    const int64_t days = days_from_civil(year, month, date);
    return days * 86400 + hours * 3600 + minutes * 60 + seconds;
}

/* Inverse of ds3231_regs_to_epoch(). Day-of-week (register 0x03) is written
 * as a fixed, valid-but-meaningless value -- nothing in this codebase reads
 * it back, same as the bring-up seed's own placeholder for that field. */
void epoch_to_ds3231_regs(int64_t epoch_seconds, uint8_t regs[7]) {
    int64_t days = epoch_seconds >= 0 ? epoch_seconds / 86400
                                       : -((-epoch_seconds + 86399) / 86400);
    int64_t remainder = epoch_seconds - days * 86400;
    if (remainder < 0) {
        remainder += 86400;
        days -= 1;
    }
    const int hours = static_cast<int>(remainder / 3600);
    const int minutes = static_cast<int>((remainder / 60) % 60);
    const int seconds = static_cast<int>(remainder % 60);

    int year = 0;
    int month = 0;
    int date = 0;
    civil_from_days(days, &year, &month, &date);

    regs[0] = bin_to_bcd(static_cast<uint8_t>(seconds));
    regs[1] = bin_to_bcd(static_cast<uint8_t>(minutes));
    regs[2] = bin_to_bcd(static_cast<uint8_t>(hours)); /* bit 6 clear: 24h */
    regs[3] = bin_to_bcd(1);                           /* day-of-week, unused */
    regs[4] = bin_to_bcd(static_cast<uint8_t>(date));
    regs[5] = bin_to_bcd(static_cast<uint8_t>(month)); /* bit 7 clear: century */
    regs[6] = bin_to_bcd(static_cast<uint8_t>(year - 2000));
}

/* Best-effort DS3231 read at boot. See this file's header comment for the
 * failure-path contract: every branch below just returns, leaving
 * g_wall_valid false. g_rtc_dev is set as soon as the chip answers at all,
 * even if the OSF check below decides not to trust today's registers --
 * a chip that lost time can still be told the correct time later via
 * kf_time_set_wall(). */
void try_init_ds3231() {
    i2c_master_bus_config_t bus_cfg{};
    bus_cfg.i2c_port = I2C_NUM_0;
    bus_cfg.sda_io_num = KF_ESP_PIN_I2C_SDA;
    bus_cfg.scl_io_num = KF_ESP_PIN_I2C_SCL;
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = true;

    if (i2c_new_master_bus(&bus_cfg, &g_i2c_bus) != ESP_OK) {
        KF_LOGW(TAG, "DS3231: I2C bus init failed -- wall clock stays unset");
        return;
    }

    i2c_device_config_t dev_cfg{};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = KF_ESP_I2C_ADDR_DS3231;
    dev_cfg.scl_speed_hz = 100000;

    i2c_master_dev_handle_t dev = nullptr;
    if (i2c_master_bus_add_device(g_i2c_bus, &dev_cfg, &dev) != ESP_OK) {
        KF_LOGW(TAG, "DS3231: device add failed -- wall clock stays unset");
        return;
    }

    /* KF_ESP_I2C_ADDR_DS3231 (0x68) is also where an MPU-6050 answers (see
     * kf_esp_pins.h's own comment on the collision) -- so "something ACKed"
     * is not proof it is the RTC. The temperature register is what the
     * bring-up diagnostic uses to tell them apart (stage_i2c_and_rtc()),
     * and this driver does the same check for the same reason, just with a
     * bail-out instead of a printed diagnosis: trusting an MPU-6050's
     * registers as if they were a clock would not fail loudly, it would
     * silently feed kf_pet_session a wrong epoch and a wrong offline-aging
     * calculation with it. */
    uint8_t temp_raw[2] = {0, 0};
    if (!ds3231_read(dev, kRegTemp, temp_raw, sizeof(temp_raw))) {
        KF_LOGW(TAG,
                "DS3231: nothing answered at 0x%02X (not wired yet, or this "
                "is an MPU-6050 at the shared address) -- wall clock stays "
                "unset",
                KF_ESP_I2C_ADDR_DS3231);
        return;
    }
    const float celsius = static_cast<float>(static_cast<int8_t>(temp_raw[0])) +
                           static_cast<float>(temp_raw[1] >> 6) * 0.25f;
    if (celsius < -10.0f || celsius > 60.0f) {
        KF_LOGW(TAG,
                "DS3231: device at 0x%02X answered but reports %.2f C, not a "
                "plausible room temperature -- probably an MPU-6050 at the "
                "shared address, not the RTC -- wall clock stays unset",
                KF_ESP_I2C_ADDR_DS3231, static_cast<double>(celsius));
        return;
    }

    uint8_t status = 0;
    if (!ds3231_read(dev, kRegStatus, &status, 1)) {
        KF_LOGW(TAG,
                "DS3231: temperature read ok but status register did not -- "
                "wall clock stays unset");
        return;
    }

    g_rtc_dev = dev;

    if ((status & kStatusOsfBit) != 0) {
        /* Oscillator-stop flag: the chip is telling us it does not trust
         * its own registers (dead/missing backup cell, or never seeded).
         * Reporting this as a valid wall time would be worse than having
         * none -- kf_pet_session's offline fast-forward would silently
         * trust a wrong number. See this file's header comment for why
         * this driver never auto-seeds on OSF the way the bring-up
         * diagnostic does. */
        KF_LOGW(TAG,
                "DS3231: OSF set (oscillator stopped since it was last set) "
                "-- wall clock stays unset until it is reseeded");
        return;
    }

    uint8_t regs[7] = {};
    if (!ds3231_read(dev, kRegTime, regs, sizeof(regs))) {
        KF_LOGW(TAG,
                "DS3231: status register read ok but time registers did not "
                "-- wall clock stays unset");
        return;
    }

    const int64_t epoch_seconds = ds3231_regs_to_epoch(regs);
    struct timeval tv;
    tv.tv_sec = static_cast<time_t>(epoch_seconds);
    tv.tv_usec = 0;
    if (settimeofday(&tv, nullptr) != 0) {
        KF_LOGW(TAG,
                "DS3231: read a time but settimeofday() failed -- wall "
                "clock stays unset");
        return;
    }
    g_wall_valid = true;
    KF_LOGI(TAG, "DS3231: wall clock set from RTC (epoch %lld)",
            static_cast<long long>(epoch_seconds));
}

} // namespace

kf_result kf_time_init(void) {
    try_init_ds3231();
    return KF_OK;
}

uint64_t kf_time_mono_us(void) {
    return static_cast<uint64_t>(esp_timer_get_time());
}

kf_wall_time kf_time_wall(void) {
    kf_wall_time out{};
    out.valid = g_wall_valid;
    if (!g_wall_valid) {
        out.epoch_seconds = 0;
        return out;
    }
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    out.epoch_seconds = static_cast<int64_t>(tv.tv_sec);
    return out;
}

kf_result kf_time_set_wall(int64_t epoch_seconds) {
    struct timeval tv;
    tv.tv_sec = static_cast<time_t>(epoch_seconds);
    tv.tv_usec = 0;
    if (settimeofday(&tv, nullptr) != 0) {
        return KF_ERR_IO;
    }
    g_wall_valid = true;

    /* Write through to the physical clock too, when one answered at boot --
     * see this file's header comment for why. Best-effort: a write failure
     * here does not undo the RAM clock update above (still correct for the
     * rest of this power cycle either way), it just means the correction
     * will not survive the next power-off -- the same class of failure
     * this file's header comment already documents for a dying coin cell,
     * not a new one. */
    if (g_rtc_dev != nullptr) {
        uint8_t regs[7] = {};
        epoch_to_ds3231_regs(epoch_seconds, regs);
        if (ds3231_write(g_rtc_dev, kRegTime, regs, sizeof(regs))) {
            uint8_t status = 0;
            if (ds3231_read(g_rtc_dev, kRegStatus, &status, 1)) {
                const uint8_t cleared =
                    static_cast<uint8_t>(status & static_cast<uint8_t>(~kStatusOsfBit));
                ds3231_write(g_rtc_dev, kRegStatus, &cleared, 1);
            }
        } else {
            KF_LOGW(TAG, "DS3231: write-through failed -- RAM clock is "
                          "updated but the RTC itself was not");
        }
    }

    return KF_OK;
}

/* kf_esp_time_debug.h's one accessor -- see that header for the full
 * contract. Reads g_rtc_dev's registers directly, live, rather than
 * reporting anything cached: kf_time_wall() above never touches the bus
 * after boot (it reads gettimeofday(), the in-RAM clock), which is exactly
 * why a second, bus-reading function has to exist for `KFDBG RTC` to be
 * able to prove the two haven't drifted apart. */
bool kf_esp_time_debug_read_rtc(int64_t *epoch_seconds, bool *osf) {
    if (g_rtc_dev == nullptr) {
        return false;
    }

    uint8_t status = 0;
    if (!ds3231_read(g_rtc_dev, kRegStatus, &status, 1)) {
        return false;
    }

    uint8_t regs[7] = {};
    if (!ds3231_read(g_rtc_dev, kRegTime, regs, sizeof(regs))) {
        return false;
    }

    *osf = (status & kStatusOsfBit) != 0;
    *epoch_seconds = ds3231_regs_to_epoch(regs);
    return true;
}

void kf_time_delay_us(uint32_t microseconds) {
    if (microseconds == 0u) {
        return;
    }
    /* vTaskDelay works in ticks, not microseconds -- rounding up rather
     * than down, since "give the rest of the system a chance to run for
     * roughly this long" (kf/hal/time.h) is a lower bound, not a ceiling.
     * A caller wanting fine-grained sub-tick timing wants esp_rom_delay_us()
     * instead, which this HAL does not expose because nothing in core
     * calls kf_time_delay_us() for anything that short today. */
    const TickType_t ticks = pdMS_TO_TICKS((microseconds + 999u) / 1000u);
    vTaskDelay(ticks > 0 ? ticks : 1);
}
