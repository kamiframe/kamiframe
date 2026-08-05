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
 * kf_time_wall() is NOT backed by a DS3231 yet, and that gap is real, not
 * glossed over. The DS3231 breakout in the hardware shopping list is
 * external I2C hardware with no bus pins decided (kf_esp_pins.h has none
 * for it) and no driver written -- wiring it is Phase 1b work this ADR
 * 0020 slice does not claim to have done. What runs today is the chip's
 * own internal system clock (settimeofday()/gettimeofday(), backed by
 * esp_timer under the hood), seeded invalid on every cold boot exactly the
 * way kf/hal/time.h's kf_wall_time.valid documents for "a fresh device" --
 * which means the one thing doc 02 calls non-negotiable (the pet ages
 * while switched off) does NOT actually work on this backend yet. A
 * kf_power_deep_sleep_until() call still won't lose the wall clock across
 * a deep sleep and wake within the same power-on session (see
 * kf_time_set_wall() below), but a genuine power-off -- unplug the board,
 * come back next week -- currently returns to .valid == false, same as a
 * brand new device. This is the single most important item ESP32 HAL
 * scaffolding leaves for the real DS3231 driver to fix.
 */

#include "kf/hal/time.h"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <sys/time.h>

namespace {
bool g_wall_valid = false;
} // namespace

kf_result kf_time_init(void) {
    /* Deliberately not touching g_wall_valid here: a cold boot has no wall
     * clock yet (see this file's header comment), which is exactly the
     * "false before the RTC has ever been set" case kf/types.h documents
     * for kf_wall_time.valid. kf_time_set_wall() is what flips it true. */
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
    return KF_OK;
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
