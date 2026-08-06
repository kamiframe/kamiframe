/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * Hardware bring-up diagnostic. This is NOT the firmware -- it shares
 * nothing with hakoniwaos except ../esp32/hal/kf_esp_pins.h, on purpose.
 *
 * The job of this program is to answer one question per peripheral, in the
 * order docs/hardware-bringup.md tells you to wire them: "is this thing
 * connected to the pins the firmware thinks it is?" It deliberately does not
 * use the HAL, the arena allocator, LVGL, Lua or the pet, because when
 * nothing appears on a screen you have just wired for the first time, you
 * want the shortest possible list of things that could be wrong.
 *
 * Every stage prints PASS or FAIL and, on FAIL, the specific wire to go and
 * look at. Stages run in wiring order and a failure never stops the run --
 * you get the full picture in one pass rather than fixing one wire per flash
 * cycle.
 *
 * There is no serial input handling and no menu. To re-run everything, press
 * the RST button on the devkit. That is one less thing that can go wrong at
 * a bench.
 *
 *     cd ports/esp32-bringup
 *     idf.py set-target esp32s3
 *     idf.py flash monitor
 */

#include "kf_esp_pins.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

/* ------------------------------------------------------------------------
 * Panel geometry. Hardcoded rather than pulled from kf/budget.h so this
 * program stays buildable with nothing but ESP-IDF -- see the file header.
 * Keep in sync with KF_DISPLAY_WIDTH/KF_DISPLAY_HEIGHT if those ever change.
 * ---------------------------------------------------------------------- */
constexpr int kWidth = 240;
constexpr int kHeight = 320;

/* SPI clock for the display during bring-up: 20MHz, deliberately half of
 * kf/budget.h's KF_DISPLAY_SPI_HZ. Long breadboard jumper wires are not
 * controlled-impedance anything, and a panel that works at 20MHz on a
 * breadboard and fails at 40 is a wiring-length problem, not a wiring-order
 * problem. Prove it works slowly first; measure the real ceiling later. */
constexpr int kLcdClockHz = 20 * 1000 * 1000;

/* One horizontal strip of the framebuffer at a time, so nothing here needs
 * a full 150KB frame and every transfer comes out of DMA-capable internal
 * RAM. 240 x 40 x 2 bytes = 19200 bytes. */
constexpr int kStripRows = 40;

constexpr uint16_t kBlack = 0x0000;
constexpr uint16_t kWhite = 0xFFFF;
constexpr uint16_t kRed = 0xF800;
constexpr uint16_t kGreen = 0x07E0;
constexpr uint16_t kBlue = 0x001F;
constexpr uint16_t kGrey = 0x2104;

struct Stage {
    const char *name;
    bool pass;
    const char *note;
};

constexpr int kStageCount = 5;
Stage g_stages[kStageCount] = {
    {"1. Backlight (GPIO6)", false, ""},
    {"2. Display (ST7789 on SPI2)", false, ""},
    {"3. I2C bus + DS3231 RTC", false, ""},
    {"4. microSD card (SPI3)", false, ""},
    {"5. Buttons", false, ""},
};

esp_lcd_panel_handle_t g_panel = nullptr;
esp_lcd_panel_io_handle_t g_panel_io = nullptr;
uint16_t *g_strip = nullptr;

i2c_master_bus_handle_t g_i2c_bus = nullptr;

/* ------------------------------------------------------------------------
 * Output helpers. Plain printf, not ESP_LOG, so the diagnostic output is
 * not interleaved with driver log lines at whatever level the menuconfig
 * happens to be set to.
 * ---------------------------------------------------------------------- */

void rule() {
    printf("--------------------------------------------------------------\n");
}

void banner(const char *title) {
    printf("\n");
    rule();
    printf("  %s\n", title);
    rule();
}

void pass(int stage, const char *fmt = nullptr, ...) {
    g_stages[stage].pass = true;
    printf("  [ PASS ]");
    if (fmt != nullptr) {
        va_list args;
        va_start(args, fmt);
        printf("  ");
        vprintf(fmt, args);
        va_end(args);
    }
    printf("\n");
}

void fail(int stage, const char *note, const char *check) {
    g_stages[stage].pass = false;
    g_stages[stage].note = note;
    printf("  [ FAIL ]  %s\n", note);
    printf("            GO LOOK AT: %s\n", check);
}

void info(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    printf("           ");
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
}

/* ------------------------------------------------------------------------
 * Stage 0: what the chip says about itself. Not a pass/fail stage -- it is
 * the check that the board is the board you think it is before any wiring
 * is blamed for anything. A N16R8 that reports 0 bytes of PSRAM is a
 * menuconfig problem or a counterfeit module, and either way every later
 * stage would be debugging the wrong thing.
 * ---------------------------------------------------------------------- */
void stage_board_info() {
    banner("STAGE 0: the board itself (no wiring involved)");

    esp_chip_info_t chip{};
    esp_chip_info(&chip);
    printf("  chip           : %s, %d core(s), silicon revision %d\n",
           CONFIG_IDF_TARGET, chip.cores, chip.revision);

    uint32_t flash_size = 0;
    if (esp_flash_get_size(nullptr, &flash_size) == ESP_OK) {
        printf("  flash          : %" PRIu32 " MB\n", flash_size / (1024u * 1024u));
        if (flash_size < 16u * 1024u * 1024u) {
            printf("  ** expected 16MB for an N16R8. Check the module's own\n");
            printf("     printed part number, and CONFIG_ESPTOOLPY_FLASHSIZE.\n");
        }
    }

    const size_t psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    printf("  PSRAM          : %u bytes (%u MB)\n", (unsigned)psram,
           (unsigned)(psram / (1024u * 1024u)));
    if (psram < 4u * 1024u * 1024u) {
        printf("  ** an N16R8 should report about 8MB. Zero here means octal\n");
        printf("     PSRAM is not enabled: check CONFIG_SPIRAM and\n");
        printf("     CONFIG_SPIRAM_MODE_OCT in sdkconfig.defaults.\n");
    }

    printf("  free heap      : %" PRIu32 " bytes\n", esp_get_free_heap_size());
    printf("  CPU frequency  : %d MHz\n", CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
}

/* ------------------------------------------------------------------------
 * Stage 1: backlight. The single cheapest wiring check on the board, and
 * deliberately first: it needs two wires (power and one GPIO) and it tells
 * you whether the panel has power at all before any SPI is involved.
 *
 * Some ST7789 modules tie the backlight permanently on and expose no BL
 * pin. On those this stage cannot fail visibly, which is why it asks rather
 * than asserts.
 * ---------------------------------------------------------------------- */
void stage_backlight() {
    banner("STAGE 1: display backlight");
    info("Watch the panel. It should flash ON and OFF five times.");
    info("If it never lights: check the module's VCC and GND first,");
    info("not the BL pin -- an unpowered panel cannot glow.");

    gpio_config_t cfg{};
    cfg.pin_bit_mask = (1ULL << KF_ESP_PIN_LCD_BL);
    cfg.mode = GPIO_MODE_OUTPUT;
    cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;

    if (gpio_config(&cfg) != ESP_OK) {
        fail(0, "could not configure GPIO6 as an output",
             "nothing physical -- this is a firmware bug, not a wire");
        return;
    }

    for (int i = 0; i < 5; ++i) {
        gpio_set_level(KF_ESP_PIN_LCD_BL, 1);
        vTaskDelay(pdMS_TO_TICKS(400));
        gpio_set_level(KF_ESP_PIN_LCD_BL, 0);
        vTaskDelay(pdMS_TO_TICKS(400));
    }
    gpio_set_level(KF_ESP_PIN_LCD_BL, 1);

    /* Left on for every later stage. Marked pass because the firmware side
     * genuinely succeeded; whether light came out is the one thing on this
     * board only a human can confirm. */
    pass(0, "GPIO6 driven high/low 5 times, now left ON.");
    info("If you saw nothing at all, this stage did NOT really pass.");
}

/* ------------------------------------------------------------------------
 * Stage 2: the panel over SPI.
 * ---------------------------------------------------------------------- */

void fill_strip(uint16_t color) {
    for (int i = 0; i < kWidth * kStripRows; ++i) {
        g_strip[i] = color;
    }
}

void fill_screen(uint16_t color) {
    fill_strip(color);
    for (int y = 0; y < kHeight; y += kStripRows) {
        const int rows = (y + kStripRows > kHeight) ? (kHeight - y) : kStripRows;
        esp_lcd_panel_draw_bitmap(g_panel, 0, y, kWidth, y + rows, g_strip);
    }
}

/* A frame that makes wiring and geometry problems visible rather than
 * merely suspected: a white field, a 6px red border touching all four
 * edges, and a green square in the top-left corner only.
 *
 * If the border is cut off on one side, or wraps around, the panel needs an
 * x/y offset (esp_lcd_panel_set_gap) -- very common on 240x320 ST7789
 * modules. If the green square is not top-left, the rotation or mirror
 * settings are wrong. Both are one-line fixes, but only if you can see
 * them. */
void draw_alignment_frame() {
    constexpr int kBorder = 6;
    constexpr int kCorner = 40;

    for (int y = 0; y < kHeight; y += kStripRows) {
        const int rows = (y + kStripRows > kHeight) ? (kHeight - y) : kStripRows;
        for (int r = 0; r < rows; ++r) {
            const int abs_y = y + r;
            for (int x = 0; x < kWidth; ++x) {
                uint16_t c = kWhite;
                if (abs_y < kBorder || abs_y >= kHeight - kBorder ||
                    x < kBorder || x >= kWidth - kBorder) {
                    c = kRed;
                } else if (abs_y < kCorner && x < kCorner) {
                    c = kGreen;
                }
                g_strip[r * kWidth + x] = c;
            }
        }
        esp_lcd_panel_draw_bitmap(g_panel, 0, y, kWidth, y + rows, g_strip);
    }
}

void stage_display() {
    banner("STAGE 2: display over SPI (ST7789, SPI2)");
    info("SCLK=GPIO%d MOSI=GPIO%d CS=GPIO%d DC=GPIO%d RST=GPIO%d",
         KF_ESP_PIN_LCD_SCLK, KF_ESP_PIN_LCD_MOSI, KF_ESP_PIN_LCD_CS,
         KF_ESP_PIN_LCD_DC, KF_ESP_PIN_LCD_RST);

    g_strip = static_cast<uint16_t *>(heap_caps_malloc(
        kWidth * kStripRows * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    if (g_strip == nullptr) {
        fail(1, "could not allocate a DMA strip buffer",
             "nothing physical -- out of internal RAM, a firmware problem");
        return;
    }

    spi_bus_config_t bus{};
    bus.mosi_io_num = KF_ESP_PIN_LCD_MOSI;
    bus.miso_io_num = -1; /* write-only panel */
    bus.sclk_io_num = KF_ESP_PIN_LCD_SCLK;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = kWidth * kStripRows * static_cast<int>(sizeof(uint16_t));

    if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        fail(1, "SPI2 bus would not initialise",
             "a pin conflict in kf_esp_pins.h, not a wire");
        return;
    }

    esp_lcd_panel_io_spi_config_t io{};
    io.cs_gpio_num = KF_ESP_PIN_LCD_CS;
    io.dc_gpio_num = KF_ESP_PIN_LCD_DC;
    io.spi_mode = 0;
    io.pclk_hz = kLcdClockHz;
    io.trans_queue_depth = 10;
    io.lcd_cmd_bits = 8;
    io.lcd_param_bits = 8;

    if (esp_lcd_new_panel_io_spi(static_cast<esp_lcd_spi_bus_handle_t>(SPI2_HOST),
                                 &io, &g_panel_io) != ESP_OK) {
        fail(1, "could not attach the panel to SPI2",
             "the CS (GPIO10) and DC (GPIO9) wires");
        return;
    }

    esp_lcd_panel_dev_config_t dev{};
    dev.reset_gpio_num = KF_ESP_PIN_LCD_RST;
    dev.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    dev.data_endian = LCD_RGB_DATA_ENDIAN_LITTLE;
    dev.bits_per_pixel = 16;
    dev.flags.reset_active_high = 0;

    if (esp_lcd_new_panel_st7789(g_panel_io, &dev, &g_panel) != ESP_OK) {
        fail(1, "the ST7789 driver would not initialise",
             "the RST wire (GPIO8), and whether this panel is really an "
             "ST7789 rather than an ILI9341");
        return;
    }

    esp_lcd_panel_reset(g_panel);
    esp_lcd_panel_init(g_panel);
    esp_lcd_panel_invert_color(g_panel, false);
    esp_lcd_panel_disp_on_off(g_panel, true);

    info("Watch the panel: RED, GREEN, BLUE, WHITE, then a test frame.");

    const uint16_t sequence[] = {kRed, kGreen, kBlue, kWhite, kBlack};
    for (uint16_t c : sequence) {
        fill_screen(c);
        vTaskDelay(pdMS_TO_TICKS(600));
    }
    draw_alignment_frame();

    pass(1, "the panel accepted %d full frames at %d MHz.",
         (int)(sizeof(sequence) / sizeof(sequence[0])) + 1, kLcdClockHz / 1000000);
    info("NOW LOOK AT THE SCREEN. You should see a WHITE screen with a");
    info("RED border on all four edges and a GREEN square top-left.");
    info("  nothing at all      -> SCLK (GPIO12) or MOSI (GPIO11)");
    info("  white noise/garbage -> DC (GPIO9), or wires too long for 20MHz");
    info("  border clipped      -> the panel needs an offset "
         "(esp_lcd_panel_set_gap)");
    info("  colours swapped     -> rgb_ele_order should be BGR for this panel");
    info("  green square not");
    info("    in the top-left   -> mirror/swap_xy settings");
}

/* ------------------------------------------------------------------------
 * Stage 3: I2C, then the DS3231 specifically.
 *
 * Two separate claims, checked separately, because they fail for completely
 * different reasons: "something is answering on this bus" is a wiring
 * question, and "the RTC is actually keeping time" is a battery question.
 * ---------------------------------------------------------------------- */

const char *i2c_name_for(uint8_t addr) {
    switch (addr) {
    case KF_ESP_I2C_ADDR_DS3231:
        return "DS3231 RTC (or an MPU-6050 -- they share this address)";
    case KF_ESP_I2C_ADDR_AT24C32:
        return "AT24C32 EEPROM (the second chip on the DS3231 module)";
    case KF_ESP_I2C_ADDR_BH1750:
        return "BH1750 ambient light sensor";
    case KF_ESP_I2C_ADDR_DRV2605:
        return "DRV2605L haptic driver";
    case 0x69:
        return "MPU-6050 with AD0 pulled high";
    case KF_ESP_I2C_ADDR_BME280:
    case 0x77:
        return "BME280 temperature/pressure/humidity";
    default:
        return "unknown device";
    }
}

uint8_t bcd_to_bin(uint8_t v) { return static_cast<uint8_t>((v >> 4) * 10 + (v & 0x0F)); }
uint8_t bin_to_bcd(uint8_t v) {
    return static_cast<uint8_t>(((v / 10) << 4) | (v % 10));
}

bool ds3231_read(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *out, size_t len) {
    return i2c_master_transmit_receive(dev, &reg, 1, out, len, 200) == ESP_OK;
}

bool ds3231_write(i2c_master_dev_handle_t dev, uint8_t reg, const uint8_t *data,
                  size_t len) {
    uint8_t buf[8];
    if (len + 1 > sizeof(buf)) {
        return false;
    }
    buf[0] = reg;
    memcpy(&buf[1], data, len);
    return i2c_master_transmit(dev, buf, len + 1, 200) == ESP_OK;
}

void stage_i2c_and_rtc() {
    banner("STAGE 3: I2C bus and the DS3231 real-time clock");
    info("SDA=GPIO%d SCL=GPIO%d", KF_ESP_PIN_I2C_SDA, KF_ESP_PIN_I2C_SCL);

    i2c_master_bus_config_t bus{};
    bus.i2c_port = I2C_NUM_0;
    bus.sda_io_num = KF_ESP_PIN_I2C_SDA;
    bus.scl_io_num = KF_ESP_PIN_I2C_SCL;
    bus.clk_source = I2C_CLK_SRC_DEFAULT;
    bus.glitch_ignore_cnt = 7;
    bus.flags.enable_internal_pullup = true;

    if (i2c_new_master_bus(&bus, &g_i2c_bus) != ESP_OK) {
        fail(2, "the I2C controller would not start",
             "a pin conflict in kf_esp_pins.h, not a wire");
        return;
    }

    printf("  scanning addresses 0x08-0x77...\n");
    int found = 0;
    for (uint8_t addr = 0x08; addr < 0x78; ++addr) {
        if (i2c_master_probe(g_i2c_bus, addr, 50) == ESP_OK) {
            printf("    0x%02X  %s\n", addr, i2c_name_for(addr));
            ++found;
        }
    }
    if (found == 0) {
        fail(2, "nothing answered on the I2C bus at all",
             "SDA (GPIO13) and SCL (GPIO14) -- and check they are not swapped, "
             "which looks exactly like this");
        info("Also confirm the module has 3V3 and GND. A device with no");
        info("power cannot answer, and an empty scan looks the same either way.");
        return;
    }
    printf("  %d device(s) found.\n", found);

    i2c_device_config_t dev_cfg{};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = KF_ESP_I2C_ADDR_DS3231;
    dev_cfg.scl_speed_hz = 100000;

    i2c_master_dev_handle_t rtc = nullptr;
    if (i2c_master_bus_add_device(g_i2c_bus, &dev_cfg, &rtc) != ESP_OK) {
        fail(2, "could not open the device at 0x68", "the DS3231 module's wiring");
        return;
    }

    /* The DS3231's temperature register is what distinguishes it from an
     * MPU-6050, which answers on the same address. A room-temperature
     * reading here means it really is the RTC. */
    uint8_t temp_raw[2] = {0, 0};
    if (!ds3231_read(rtc, 0x11, temp_raw, 2)) {
        fail(2, "0x68 answered a probe but would not return data",
             "the SDA wire -- a half-working bus usually means a loose jumper");
        return;
    }
    const float celsius =
        static_cast<int8_t>(temp_raw[0]) + ((temp_raw[1] >> 6) * 0.25f);
    info("on-chip temperature: %.2f C", celsius);
    if (celsius < -10.0f || celsius > 60.0f) {
        info("** that is not a room temperature. The device at 0x68 is");
        info("   probably an MPU-6050, not the DS3231.");
    }

    uint8_t status = 0;
    ds3231_read(rtc, 0x0F, &status, 1);
    const bool oscillator_stopped = (status & 0x80) != 0;

    if (oscillator_stopped) {
        info("OSF flag is SET: the clock has lost time since it was last set.");
        info("Expected on the very first power-up. If you see this EVERY");
        info("boot, the CR2032 backup cell is dead, in backwards, or the");
        info("module's coin-cell holder is not making contact.");

        /* Seed a known time so the "did it tick?" check below is meaningful.
         * The date is arbitrary and only has to be valid. */
        const uint8_t seed[7] = {
            bin_to_bcd(0),  /* seconds */
            bin_to_bcd(0),  /* minutes */
            bin_to_bcd(12), /* hours, 24h mode (bit 6 clear) */
            bin_to_bcd(4),  /* day of week */
            bin_to_bcd(6),  /* date */
            bin_to_bcd(8),  /* month */
            bin_to_bcd(26), /* year, 2026 */
        };
        ds3231_write(rtc, 0x00, seed, sizeof(seed));

        const uint8_t cleared = static_cast<uint8_t>(status & 0x7F);
        ds3231_write(rtc, 0x0F, &cleared, 1);
        info("seeded the clock to 2026-08-06 12:00:00 and cleared OSF.");
    }

    uint8_t t1[7] = {};
    if (!ds3231_read(rtc, 0x00, t1, sizeof(t1))) {
        fail(2, "could not read the time registers", "the SDA wire");
        return;
    }
    printf("  time now       : 20%02u-%02u-%02u %02u:%02u:%02u\n",
           bcd_to_bin(t1[6]), bcd_to_bin(static_cast<uint8_t>(t1[5] & 0x1F)),
           bcd_to_bin(t1[4]), bcd_to_bin(static_cast<uint8_t>(t1[2] & 0x3F)),
           bcd_to_bin(t1[1]), bcd_to_bin(static_cast<uint8_t>(t1[0] & 0x7F)));

    info("waiting 2 seconds to see whether the oscillator is actually running...");
    vTaskDelay(pdMS_TO_TICKS(2000));

    uint8_t t2[7] = {};
    ds3231_read(rtc, 0x00, t2, sizeof(t2));
    if (t1[0] == t2[0]) {
        fail(2, "the seconds register did not advance -- the clock is frozen",
             "the DS3231 crystal. A module that answers I2C but never ticks "
             "is usually a dead or counterfeit chip, not a wiring fault");
        return;
    }

    pass(2, "DS3231 present, readable, and ticking.");
    info("THE REAL TEST IS NEXT, and only you can do it:");
    info("  1. unplug the board completely for 30 seconds");
    info("  2. plug it back in and re-run");
    info("  3. OSF must still be clear and the time must have moved forward");
    info("That is what proves the pet can age while switched off.");
}

/* ------------------------------------------------------------------------
 * Stage 4: microSD, on its own SPI bus. See kf_esp_pins.h for why it is not
 * sharing SPI2 with the display.
 * ---------------------------------------------------------------------- */
void stage_sd_card() {
    banner("STAGE 4: microSD card (its own SPI bus, SPI3)");
    info("SCLK=GPIO%d MOSI=GPIO%d MISO=GPIO%d CS=GPIO%d", KF_ESP_PIN_SD_SCLK,
         KF_ESP_PIN_SD_MOSI, KF_ESP_PIN_SD_MISO, KF_ESP_PIN_SD_CS);

    spi_bus_config_t bus{};
    bus.mosi_io_num = KF_ESP_PIN_SD_MOSI;
    bus.miso_io_num = KF_ESP_PIN_SD_MISO;
    bus.sclk_io_num = KF_ESP_PIN_SD_SCLK;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = 4000;

    if (spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        fail(3, "SPI3 bus would not initialise",
             "a pin conflict in kf_esp_pins.h, not a wire");
        return;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI3_HOST;
    /* 10MHz rather than the 20MHz default: same reasoning as the display
     * clock above, breadboard jumpers are not a transmission line. */
    host.max_freq_khz = 10000;

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = KF_ESP_PIN_SD_CS;
    slot.host_id = SPI3_HOST;

    esp_vfs_fat_sdmmc_mount_config_t mount{};
    /* Deliberately false. Formatting a card that failed to mount would
     * destroy whatever is on it to fix a problem that is usually a loose
     * wire. */
    mount.format_if_mount_failed = false;
    mount.max_files = 4;
    mount.allocation_unit_size = 16 * 1024;

    sdmmc_card_t *card = nullptr;
    const esp_err_t err =
        esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot, &mount, &card);

    if (err != ESP_OK) {
        if (err == ESP_FAIL) {
            fail(3, "the card responded but the filesystem would not mount",
                 "the card itself -- reformat it as FAT32 on your PC. exFAT "
                 "and NTFS will not work");
        } else {
            fail(3, "no card responded at all",
                 "MISO (GPIO41) first -- it is the one wire the other three "
                 "cannot compensate for -- then CS (GPIO42), then whether the "
                 "module has 5V rather than 3V3");
        }
        info("Also: is a card actually pushed all the way into the slot?");
        return;
    }

    printf("  card           : %s\n", card->cid.name);
    printf("  capacity       : %llu MB\n",
           ((uint64_t)card->csd.capacity * card->csd.sector_size) / (1024 * 1024));
    printf("  speed          : %" PRIu32 " kHz\n", card->max_freq_khz);

    const char *path = "/sdcard/kf_bringup.txt";
    const char *payload = "kamiframe bring-up write test\n";

    FILE *f = fopen(path, "w");
    if (f == nullptr) {
        fail(3, "the card mounted but would not accept a new file",
             "the card's write-protect state and free space");
        esp_vfs_fat_sdcard_unmount("/sdcard", card);
        return;
    }
    fputs(payload, f);
    fclose(f);

    char readback[64] = {};
    f = fopen(path, "r");
    if (f == nullptr || fgets(readback, sizeof(readback), f) == nullptr) {
        if (f != nullptr) {
            fclose(f);
        }
        fail(3, "wrote a file but could not read it back",
             "the MISO wire (GPIO41) -- writes can appear to succeed on a "
             "one-way bus");
        esp_vfs_fat_sdcard_unmount("/sdcard", card);
        return;
    }
    fclose(f);

    const bool match = strcmp(readback, payload) == 0;
    remove(path);
    esp_vfs_fat_sdcard_unmount("/sdcard", card);

    if (!match) {
        fail(3, "the file read back as different bytes than were written",
             "signal quality -- shorten the jumper wires, and try lowering "
             "host.max_freq_khz");
        return;
    }

    pass(3, "mounted, wrote a file, read it back byte-identical, removed it.");
}

/* ------------------------------------------------------------------------
 * Stage 5: buttons. Never returns -- this is the stage you leave running
 * while you press things.
 * ---------------------------------------------------------------------- */

struct ButtonBinding {
    gpio_num_t pin;
    const char *name;
    uint16_t color;
};

constexpr ButtonBinding kButtons[] = {
    {KF_ESP_PIN_BTN_UP, "UP", kRed},      {KF_ESP_PIN_BTN_DOWN, "DOWN", 0xFD20},
    {KF_ESP_PIN_BTN_LEFT, "LEFT", 0xFFE0}, {KF_ESP_PIN_BTN_RIGHT, "RIGHT", kGreen},
    {KF_ESP_PIN_BTN_A, "A", 0x07FF},      {KF_ESP_PIN_BTN_B, "B", kBlue},
    {KF_ESP_PIN_BTN_MENU, "MENU", 0xF81F},
};
constexpr int kButtonCount = sizeof(kButtons) / sizeof(kButtons[0]);

void draw_button_bands(uint32_t pressed_mask) {
    if (g_panel == nullptr || g_strip == nullptr) {
        return;
    }
    for (int y = 0; y < kHeight; y += kStripRows) {
        const int rows = (y + kStripRows > kHeight) ? (kHeight - y) : kStripRows;
        for (int r = 0; r < rows; ++r) {
            for (int x = 0; x < kWidth; ++x) {
                int band = (x * kButtonCount) / kWidth;
                if (band >= kButtonCount) {
                    band = kButtonCount - 1;
                }
                const bool held = (pressed_mask & (1u << band)) != 0;
                g_strip[r * kWidth + x] = held ? kButtons[band].color : kGrey;
            }
        }
        esp_lcd_panel_draw_bitmap(g_panel, 0, y, kWidth, y + rows, g_strip);
    }
}

[[noreturn]] void stage_buttons() {
    banner("STAGE 5: buttons (this stage runs forever -- press RST to restart)");

    uint64_t mask = 0;
    for (const ButtonBinding &b : kButtons) {
        mask |= (1ULL << b.pin);
    }

    gpio_config_t cfg{};
    cfg.pin_bit_mask = mask;
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&cfg);

    printf("  Buttons are active-low with the chip's internal pull-ups on, so\n");
    printf("  each one needs exactly two wires: its GPIO, and GND.\n\n");
    for (const ButtonBinding &b : kButtons) {
        printf("    %-6s GPIO%-3d\n", b.name, (int)b.pin);
    }
    printf("\n  Press each one. The screen is split into %d vertical bands,\n",
           kButtonCount);
    printf("  left to right in the order above; a band lights while its\n");
    printf("  button is held.\n\n");
    printf("  A button that reads HELD when you are not touching it is\n");
    printf("  wired across the wrong pair of legs. On a 4-pin tactile switch\n");
    printf("  the legs are shorted together in pairs already -- use legs that\n");
    printf("  are DIAGONALLY OPPOSITE and the switch will behave.\n\n");

    uint32_t last = 0xFFFFFFFF;
    int64_t last_report_us = 0;

    for (;;) {
        uint32_t now_mask = 0;
        for (int i = 0; i < kButtonCount; ++i) {
            if (gpio_get_level(kButtons[i].pin) == 0) { /* active low */
                now_mask |= (1u << i);
            }
        }

        if (now_mask != last) {
            printf("  [%8lld ms] ", esp_timer_get_time() / 1000);
            if (now_mask == 0) {
                printf("(nothing held)\n");
            } else {
                for (int i = 0; i < kButtonCount; ++i) {
                    if (now_mask & (1u << i)) {
                        printf("%s ", kButtons[i].name);
                    }
                }
                printf("\n");
            }
            draw_button_bands(now_mask);
            last = now_mask;
            last_report_us = esp_timer_get_time();
        }

        /* A stuck-high reading is the failure mode worth naming out loud,
         * because it looks like "the button does not work" when it is
         * actually "the button is permanently pressed". */
        if (now_mask != 0 && esp_timer_get_time() - last_report_us > 5000000) {
            printf("  ** something has been held for 5 seconds. If you are not\n");
            printf("     touching anything, that switch is wired across a pair\n");
            printf("     of legs that are already connected. Rotate it 90 "
                   "degrees.\n");
            last_report_us = esp_timer_get_time();
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void print_summary() {
    banner("SUMMARY");
    int failures = 0;
    for (int i = 0; i < kStageCount - 1; ++i) { /* buttons never completes */
        printf("  %-32s %s\n", g_stages[i].name, g_stages[i].pass ? "PASS" : "FAIL");
        if (!g_stages[i].pass) {
            ++failures;
        }
    }
    printf("\n");
    if (failures == 0) {
        printf("  Everything the firmware can check by itself passed. Confirm\n");
        printf("  with your eyes that the screen showed the test frame, then\n");
        printf("  go press some buttons.\n");
    } else {
        printf("  %d stage(s) failed. Fix them in the order printed above --\n",
               failures);
        printf("  a later stage failing is often just an earlier one's fault.\n");
    }
}

} // namespace

extern "C" void app_main(void) {
    /* A pause before anything else: `idf.py monitor` usually attaches a
     * moment after the board resets, and diagnostics you cannot read are
     * not diagnostics. */
    vTaskDelay(pdMS_TO_TICKS(1500));

    printf("\n\n");
    rule();
    printf("  KAMIFRAME HARDWARE BRING-UP\n");
    printf("  Wire it in the order docs/hardware-bringup.md gives, then run\n");
    printf("  this. Every stage says what to go and look at when it fails.\n");
    rule();

    stage_board_info();
    stage_backlight();
    stage_display();
    stage_i2c_and_rtc();
    stage_sd_card();
    print_summary();
    stage_buttons();
}
