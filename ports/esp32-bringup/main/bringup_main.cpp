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
#include "nvs.h"
#include "nvs_flash.h"
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

/* 4MHz, dropped from 20 after the first real bring-up: Chris's panel is on
 * a chain of Dupont jumpers with inline couplers, which is a long way from
 * a controlled-impedance trace. Prove the panel works at a clock no wiring
 * can plausibly break, then raise it. If it works here and fails at 20MHz,
 * that is a wire-length answer, not a wiring-order one.
 *
 * "Then raise it" is stage 2b's job, not a value to edit here. This constant
 * is the SAFE clock -- the one stage 2 proves the panel at, and the one the
 * sweep returns to afterwards so the button stage stays readable. The real
 * achievable figure belongs in KF_DISPLAY_SPI_HZ (kf/budget.h) once the
 * sweep has measured it. */
constexpr int kLcdClockHz = 4 * 1000 * 1000;

/* Stage 2b, the clock sweep. Off: it did its job on 2026-08-08. 40MHz was the
 * highest speed that rendered correctly on the ILI9341 and 80MHz came out
 * solid white, so KF_DISPLAY_SPI_HZ is now a measured 40MHz rather than an
 * assumed one, and running this every time would be 45 seconds spent
 * re-answering it.
 *
 * Turn it back on when the wiring or the panel changes -- specifically when
 * the replacement 2in ST7789 arrives, since that is the primary panel and
 * this figure was measured on the 2.8in ILI9341. */
constexpr bool kRunClockSweep = false;

/* One horizontal strip of the framebuffer at a time, so nothing here needs
 * a full 150KB frame and every transfer comes out of DMA-capable internal
 * RAM. 240 x 40 x 2 bytes = 19200 bytes. */

/* ------------------------------------------------------------------------
 * WHICH PANEL IS PLUGGED IN.
 *
 * false = Waveshare 2inch, ST7789V, PH2.0 cable.
 * true  = HiLetgo 2.8in, ILI9341, plain 0.1in pins.
 *
 * Flip this, rebuild, flash. Nothing else changes: the wires are the same
 * eight, and the 2.8in module has ordinary header pins so it goes straight
 * into the breadboard with no cable and no couplers.
 *
 * This exists as a control experiment. After a full evening of measurement
 * the ESP32 side is verified about as far as it can be -- all five control
 * lines swing a clean 3.3V at the panel's own pads under firmware, the
 * supply is 3.3V, and the init sequence matches the upstream Linux driver
 * written for that exact Waveshare module. If a different panel on the same
 * eight wires produces a picture, the Waveshare one is faulty. If it also
 * stays dark, everything I have concluded is wrong and the fault is on the
 * ESP32 side somewhere none of these tests has looked.
 * ---------------------------------------------------------------------- */
constexpr bool kPanelIsIli9341 = true;

/* ------------------------------------------------------------------------
 * DEAD-PANEL INVESTIGATION MODE.
 *
 * false (normal): the diagnostic answers "is this wired correctly" and gets
 * out of the way. About three minutes faster per run, which matters because
 * the RTC power-off check needs two runs.
 *
 * true: turns back on everything that was written to answer a different and
 * much worse question -- "the panel is dark and I do not know why."
 * Specifically stage 2a (drive each control line slowly enough to follow
 * with a multimeter at the panel's own pads) and stage 2's steps A/B/C
 * (send nothing, then one square, then a second square, so a panel that
 * dies on write can be told apart from one that never woke up). It also
 * holds the final test card for 90 seconds instead of 10, which is what you
 * want if you are photographing it rather than glancing at it.
 *
 * Kept rather than deleted because it earned its keep once already: it is
 * what proved the first Waveshare module's DC line was dead at the panel
 * while the GPIO drove it perfectly. When the replacement ST7789 arrives and
 * shows nothing, flip this to true before assuming the wiring is wrong.
 * ---------------------------------------------------------------------- */
constexpr bool kPanelDebugMode = false;

constexpr int kStripRows = 40;

/* ------------------------------------------------------------------------
 * BYTE ORDER. This is the whole reason the first ILI9341 run showed the
 * wrong colours, and it is worth writing down properly because it is
 * invisible from the outside.
 *
 * RGB565 is sixteen bits, and both of these controllers want the high byte
 * on the wire first. The ESP32-S3 is little-endian, so a uint16_t 0xF800
 * sitting in a framebuffer is stored as 0x00 0xF8 and esp_lcd hands the
 * bytes to SPI exactly as they lie in memory -- panel_io_spi_tx_color does
 * not touch colour data, only commands and parameters get byte-reversed.
 * The panel therefore reads 0x00F8, which is not red. It is blue.
 *
 * The ST7789 has a way out: RAMCTRL (0xB0) carries a little-endian bit, and
 * esp_lcd_new_panel_st7789() sets it from panel_dev_config.data_endian,
 * which this file sets to LITTLE. That is why the constants below were
 * written plain in the first place.
 *
 * But that bit is only ever sent by esp_lcd_panel_init(), and for the
 * ILI9341 this file deliberately SKIPS esp_lcd_panel_init(), because 0xB0
 * is a different register on that controller. So the request never reaches
 * the panel -- and the ILI9341 has no equivalent register anyway. It is
 * always big-endian. The framebuffer has to match it.
 *
 * Measured, not deduced. Full-screen fills on the 2.8in module read:
 *
 *   sent 0xF800 (red)   -> shown blue    0x00F8 = R0  G7  B24
 *   sent 0x07E0 (green) -> shown pink    0xE007 = R28 G0  B7
 *   sent 0x001F (blue)  -> shown green   0x1F00 = R3  G56 B0
 *   sent 0xFFFF (white) -> shown white   (byte order cannot show)
 *   sent 0x0000 (black) -> shown black   (byte order cannot show)
 *
 * Every one of those five matches a plain byte swap and nothing else. So
 * the colours below are stored pre-swapped for the ILI9341: what the name
 * says is what comes out of the glass.
 * ---------------------------------------------------------------------- */
constexpr uint16_t wire(uint16_t c) {
    return kPanelIsIli9341
               ? static_cast<uint16_t>((c >> 8) | (c << 8))
               : c;
}

constexpr uint16_t kBlack = wire(0x0000);
constexpr uint16_t kWhite = wire(0xFFFF);
constexpr uint16_t kRed = wire(0xF800);
constexpr uint16_t kGreen = wire(0x07E0);
constexpr uint16_t kBlue = wire(0x001F);
constexpr uint16_t kGrey = wire(0x2104);
constexpr uint16_t kYellow = wire(0xFFE0);
constexpr uint16_t kCyan = wire(0x07FF);
constexpr uint16_t kMagenta = wire(0xF81F);

struct Stage {
    const char *name;
    bool pass;
    const char *note;
};

constexpr int kStageCount = 5;
Stage g_stages[kStageCount] = {
    {"1. Backlight (GPIO6)", false, ""},
    {"2. Display (SPI2)", false, ""},
    {"3. I2C bus + DS3231 RTC", false, ""},
    {"4. microSD card (SPI3)", false, ""},
    {"5. Buttons", false, ""},
};

esp_lcd_panel_handle_t g_panel = nullptr;
esp_lcd_panel_io_handle_t g_panel_io = nullptr;
uint16_t *g_strip = nullptr;

i2c_master_bus_handle_t g_i2c_bus = nullptr;

/* ------------------------------------------------------------------------
 * The one thing stage 3 cannot prove in a single run: that the DS3231 keeps
 * time with the USB cable pulled out. That is the entire reason the RTC and
 * its coin cell are on the board -- kf_time_wall()'s valid flag, the offline
 * fast-forward in kf/pet.h, and every life-stage transition that happens
 * while the device is in a drawer all rest on it.
 *
 * docs/hardware-bringup.md used to hand this to the human: remember the time
 * printed, unplug for 30 seconds, plug back in, and decide for yourself
 * whether the number moved by roughly the right amount. That works, but it
 * is a comparison a person does badly at 1am and the firmware does perfectly
 * -- so the firmware does it. The last reading is written to NVS, which
 * lives in flash and therefore survives exactly the power cut being tested,
 * and the next run compares against it.
 *
 * Deliberately NOT a row in the stage table above. On a first run there is
 * nothing to compare against, and that is not a failure -- printing FAIL for
 * it would send someone hunting a fault that does not exist, which is the
 * specific thing this whole diagnostic is arranged to avoid.
 * ---------------------------------------------------------------------- */
enum class ColdBoot {
    NotChecked,    /* stage 3 never got far enough to read a time */
    BaselineSaved, /* first run: nothing to compare against yet */
    Passed,
    FailedOscillatorStopped,
    FailedNoAdvance,
};

ColdBoot g_cold_boot = ColdBoot::NotChecked;
int64_t g_cold_boot_gap_s = 0;

constexpr const char *kNvsNamespace = "kfbringup";
constexpr const char *kNvsKeyRtcEpoch = "rtc_epoch";

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


/* An ILI9341's power-up sequence. Same shape as the Waveshare table below:
 * the controller needs its power and gamma registers set before it will
 * drive glass, and neither ESP-IDF nor anything else sends them for you.
 *
 * The panel handle still comes from esp_lcd_new_panel_st7789(). That is not
 * a bodge: the only thing the driver does per-frame is CASET (0x2A), RASET
 * (0x2B) and RAMWR (0x2C), and those three are byte-identical on the two
 * controllers. Only the init differs, and this file sends that itself. What
 * IS skipped for this panel is esp_lcd_panel_init(), because it would send
 * RAMCTRL (0xB0), which on an ILI9341 is a completely different register.
 */
struct PanelCmd {
    uint8_t cmd;
    uint8_t data[16];
    uint8_t len;
    const char *what;
};

const PanelCmd kIli9341Init[] = {
    {0xEF, {0x03, 0x80, 0x02}, 3, "undocumented, in every driver"},
    {0xCF, {0x00, 0xC1, 0x30}, 3, "power control B"},
    {0xED, {0x64, 0x03, 0x12, 0x81}, 4, "power on sequence"},
    {0xE8, {0x85, 0x00, 0x78}, 3, "driver timing A"},
    {0xCB, {0x39, 0x2C, 0x00, 0x34, 0x02}, 5, "power control A"},
    {0xF7, {0x20}, 1, "pump ratio"},
    {0xEA, {0x00, 0x00}, 2, "driver timing B"},
    {0xC0, {0x23}, 1, "PWCTR1  power control 1"},
    {0xC1, {0x10}, 1, "PWCTR2  power control 2"},
    {0xC5, {0x3E, 0x28}, 2, "VMCTR1  vcom 1"},
    {0xC7, {0x86}, 1, "VMCTR2  vcom 2"},
    /* 0x88 = MY set, MX clear, BGR set. Portrait, with the module's pin
     * header at the TOP, which is how it gets mounted. The value every
     * Arduino library prints for this panel is 0x48, which is the same
     * thing rotated 180 degrees, i.e. header at the bottom. Both are
     * correct portrait; only the two mirror bits differ. Verified by
     * photograph of the test card 2026-08-08. */
    {0x36, {0x88}, 1, "MADCTL  orientation + BGR"},
    {0x3A, {0x55}, 1, "COLMOD  16 bit"},
    {0xB1, {0x00, 0x18}, 2, "FRMCTR1 frame rate"},
    {0xB6, {0x08, 0x82, 0x27}, 3, "DFUNCTR display function"},
    {0xF2, {0x00}, 1, "gamma disable"},
    {0x26, {0x01}, 1, "gamma curve select"},
    {0xE0, {0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E, 0xF1,
            0x37, 0x07, 0x10, 0x03, 0x0E, 0x09, 0x00}, 15, "GMCTRP1"},
    {0xE1, {0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31, 0xC1,
            0x48, 0x08, 0x0F, 0x0C, 0x31, 0x36, 0x0F}, 15, "GMCTRN1"},
    {0x11, {}, 0, "SLPOUT  wake up"},
};

/* The Waveshare 2inch module's own power-up sequence, from the upstream
 * Linux DRM driver written for that exact board rather than a generic
 * ST7789 example.
 *
 * panel_st7789_init() in esp_lcd_panel_st7789.c sends exactly four commands:
 * SLPOUT, MADCTL, COLMOD, RAMCTRL. That is enough for the controller to
 * accept traffic, which is why every transaction reports success. It is NOT
 * enough to drive this particular glass: nothing configures the common
 * voltage, the gate voltage, the VRH drive level or the power control, so
 * the panel runs on whatever its power-on defaults happen to be. On a module
 * whose defaults suit it that works; on this one it does not, and the
 * failure looks exactly like a wiring fault -- every call acknowledged, and
 * a dark screen.
 *
 * VCOMS, VRHS, VDVS and PWCTRL1 are the four that decide whether anything is
 * visible at all. The gamma tables only change how colours look.
 *
 * At namespace scope rather than inside stage_display() because the clock
 * sweep has to re-send it every time it rebuilds the panel at a new speed. */
const PanelCmd kWaveshareInit[] = {
    {0xB2, {0x0C, 0x0C, 0x00, 0x33, 0x33}, 5, "PORCTRL  porch control"},
    {0xB7, {0x35}, 1, "GCTRL    gate control"},
    {0xBB, {0x1F}, 1, "VCOMS    common voltage"},
    {0xC0, {0x2C}, 1, "LCMCTRL  lcd control"},
    {0xC2, {0x01}, 1, "VDVVRHEN vdv/vrh enable"},
    {0xC3, {0x12}, 1, "VRHS     vrh set"},
    {0xC4, {0x20}, 1, "VDVS     vdv set"},
    {0xC6, {0x0F}, 1, "FRCTRL2  frame rate"},
    {0xD0, {0xA4, 0xA1}, 2, "PWCTRL1  power control"},
    {0xE0, {0xD0, 0x04, 0x0D, 0x11, 0x13, 0x2B, 0x3F,
            0x54, 0x4C, 0x18, 0x0D, 0x0B, 0x1F, 0x23}, 14, "PVGAMCTRL"},
    {0xE1, {0xD0, 0x04, 0x0C, 0x11, 0x13, 0x2C, 0x3F,
            0x44, 0x51, 0x2F, 0x1F, 0x1F, 0x20, 0x23}, 14, "NVGAMCTRL"},
};

/* verbose=false during the clock sweep, which re-sends this table once per
 * frequency and would otherwise bury the sweep's own output in twenty lines
 * of register names per step. Failures still print either way. */
void send_init_table(const PanelCmd *table, int count, bool verbose = true) {
    for (int i = 0; i < count; ++i) {
        const esp_err_t err = esp_lcd_panel_io_tx_param(
            g_panel_io, table[i].cmd, table[i].len ? table[i].data : nullptr,
            table[i].len);
        if (verbose || err != ESP_OK) {
            printf("             0x%02X  %-30s %s\n", table[i].cmd, table[i].what,
                   err == ESP_OK ? "ok" : "FAILED");
        }
        if (table[i].cmd == 0x11) {
            vTaskDelay(pdMS_TO_TICKS(150)); /* datasheet wants 120ms */
        }
    }
}

/* Every esp_lcd call in this file used to have its return value thrown
 * away, including the reset, the init, the display-on and every single
 * draw. That is a real hole in a diagnostic: a panel whose transfers are
 * all failing looks identical, from the serial log, to a panel that is
 * wired perfectly and simply will not light. Nothing here ignores a return
 * code any more. */
int g_lcd_errors = 0;

bool lcd_ok(esp_err_t err, const char *what) {
    if (err == ESP_OK) {
        return true;
    }
    ++g_lcd_errors;
    printf("           ** %s FAILED: %s (0x%x)\n", what, esp_err_to_name(err),
           (int)err);
    return false;
}

/* A single small block, and nothing else on the panel touched. This is the
 * cheapest possible write: one draw_bitmap call, a few thousand pixels, no
 * loop. If a panel will show this and will not show a full frame, the two
 * failures are different and worth separating. */
void draw_square(uint16_t color, int x, int y, int size) {
    for (int i = 0; i < size * size; ++i) {
        g_strip[i] = color;
    }
    lcd_ok(esp_lcd_panel_draw_bitmap(g_panel, x, y, x + size, y + size, g_strip),
           "draw_bitmap (square)");
}

/* Three horizontal bands, for identifying colour-order faults. A solid fill
 * cannot tell you whether red and blue are swapped, because you have nothing
 * to compare it against and a wrong colour still looks like a colour. Three
 * named bands at once can: the human reads them off top to bottom and the
 * order they report tells you exactly which transform is wrong. */
[[maybe_unused]] void draw_bands(uint16_t top, uint16_t mid, uint16_t bot) {
    const int third = kHeight / 3;
    for (int y = 0; y < kHeight; y += kStripRows) {
        const int rows = (y + kStripRows > kHeight) ? (kHeight - y) : kStripRows;
        for (int r = 0; r < rows; ++r) {
            const int abs_y = y + r;
            const uint16_t c = (abs_y < third) ? top
                             : (abs_y < 2 * third) ? mid
                             : bot;
            for (int x = 0; x < kWidth; ++x) {
                g_strip[r * kWidth + x] = c;
            }
        }
        lcd_ok(esp_lcd_panel_draw_bitmap(g_panel, 0, y, kWidth, y + rows, g_strip),
               "draw_bitmap (bands)");
    }
}

void fill_screen(uint16_t color) {
    fill_strip(color);
    for (int y = 0; y < kHeight; y += kStripRows) {
        const int rows = (y + kStripRows > kHeight) ? (kHeight - y) : kStripRows;
        lcd_ok(esp_lcd_panel_draw_bitmap(g_panel, 0, y, kWidth, y + rows, g_strip),
               "draw_bitmap (strip)");
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
[[maybe_unused]] void draw_alignment_frame() {
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
        lcd_ok(esp_lcd_panel_draw_bitmap(g_panel, 0, y, kWidth, y + rows, g_strip),
               "draw_bitmap (strip)");
    }
}

/* One static frame that answers colour order and orientation together, and
 * holds still long enough to photograph. Everything in it is positioned so
 * that a photo is unambiguous even if the module is held upside down:
 *
 *   - an 18px RED bar across the TOP, with a WHITE notch at its LEFT end
 *   - a 10px GREEN stripe down the LEFT edge only
 *   - an 18px BLUE bar across the BOTTOM
 *   - eight named patches in the middle, two across and four down, in a
 *     fixed reading order:
 *
 *         WHITE    BLACK
 *         RED      GREEN
 *         BLUE     YELLOW
 *         CYAN     MAGENTA
 *
 * Read off the photo: if the white notch is not in the top-left corner the
 * orientation bits are wrong, and which corner it lands in says exactly
 * which of MX/MY/MV to change. If the notch is right but a patch is the
 * wrong colour, that is the colour path, and the pattern of which patches
 * are wrong says whether it is the BGR bit (red and blue exchange, the
 * greys and the greens stay put) or byte order (nothing sane happens, but
 * white and black stay correct). Those two are impossible to tell apart
 * from three bands, which is why the three-band test wasted an evening. */
void draw_test_card() {
    constexpr int kBar = 18;
    constexpr int kStripe = 10;
    constexpr int kNotch = 24;

    const uint16_t card[8] = {kWhite, kBlack, kRed,  kGreen,
                              kBlue,  kYellow, kCyan, kMagenta};

    const int iy0 = kBar;
    const int iy1 = kHeight - kBar;
    const int cw = (kWidth - kStripe) / 2;
    const int ch = (iy1 - iy0) / 4;

    for (int y = 0; y < kHeight; y += kStripRows) {
        const int rows = (y + kStripRows > kHeight) ? (kHeight - y) : kStripRows;
        for (int r = 0; r < rows; ++r) {
            const int ay = y + r;
            for (int x = 0; x < kWidth; ++x) {
                uint16_t c;
                if (ay < kBar) {
                    c = (x < kNotch) ? kWhite : kRed;
                } else if (ay >= iy1) {
                    c = kBlue;
                } else if (x < kStripe) {
                    c = kGreen;
                } else {
                    int col = (x - kStripe) / cw;
                    int row = (ay - iy0) / ch;
                    if (col > 1) col = 1;
                    if (row > 3) row = 3;
                    c = card[row * 2 + col];
                }
                g_strip[r * kWidth + x] = c;
            }
        }
        lcd_ok(esp_lcd_panel_draw_bitmap(g_panel, 0, y, kWidth, y + rows, g_strip),
               "draw_bitmap (test card)");
    }
}

/* ------------------------------------------------------------------------
 * Stage 2a: is each control line actually arriving at the panel?
 *
 * This exists because of a mistake worth naming. For several rounds of
 * debugging I described the panel as "accepting" and "acknowledging" the
 * data, because every esp_lcd call returned ESP_OK. That was wrong. This
 * panel has no SDO pin -- the header is VCC GND DIN CLK CS DC RST BL --
 * so the bus is write-only and nothing is ever read back. ESP_OK means the
 * ESP32 clocked bytes out of its own pin. It says nothing whatsoever about
 * whether the panel received them.
 *
 * A continuity test does not close that gap either: it proves copper joins
 * two points while the board is switched off. It cannot prove a pin is
 * being driven, or driven to the right voltage, under firmware.
 *
 * So: drive each line by hand, slowly enough that a multimeter can follow,
 * and let a human confirm the voltage at the panel's own pad. That tests
 * the whole chain -- GPIO, breadboard, jumper, coupler, cable, connector,
 * pad -- which is the thing nothing else here has actually tested.
 * ---------------------------------------------------------------------- */
void stage_pin_wiggle() {
    banner("STAGE 2a: are the control lines arriving AT THE PANEL?");

    struct Line {
        gpio_num_t pin;
        const char *panel_pad;
    };
    constexpr Line kLines[] = {
        {KF_ESP_PIN_LCD_RST, "RST"}, {KF_ESP_PIN_LCD_DC, "DC"},
        {KF_ESP_PIN_LCD_CS, "CS"},   {KF_ESP_PIN_LCD_SCLK, "CLK"},
        {KF_ESP_PIN_LCD_MOSI, "DIN"},
    };

    printf("  Put your multimeter on DC volts. Black probe on the panel's\n");
    printf("  GND pad. Leave the red probe on the pad named below.\n\n");
    printf("  Each line is held HIGH for 6 seconds, then LOW for 6. You are\n");
    printf("  looking for roughly 3.3V, then roughly 0V, at the PANEL end.\n\n");
    printf("  Anything that never moves is not reaching the panel, whatever\n");
    printf("  a continuity test says. Anything that swings but only reaches\n");
    printf("  1-2V is being loaded down by something.\n");

    uint64_t mask = 0;
    for (const Line &l : kLines) {
        mask |= (1ULL << l.pin);
    }
    gpio_config_t cfg{};
    cfg.pin_bit_mask = mask;
    cfg.mode = GPIO_MODE_OUTPUT;
    cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&cfg);
    for (const Line &l : kLines) {
        gpio_set_level(l.pin, 0);
    }

    for (const Line &l : kLines) {
        printf("\n  ---- probe the panel's %s pad now ----\n", l.panel_pad);
        printf("       GPIO%-2d  HIGH ... expect about 3.3V\n", (int)l.pin);
        gpio_set_level(l.pin, 1);
        vTaskDelay(pdMS_TO_TICKS(6000));
        printf("       GPIO%-2d  LOW  ... expect about 0V\n", (int)l.pin);
        gpio_set_level(l.pin, 0);
        vTaskDelay(pdMS_TO_TICKS(6000));
    }

    /* Hand the pins back so the SPI peripheral can claim them cleanly. */
    for (const Line &l : kLines) {
        gpio_reset_pin(l.pin);
    }
    printf("\n  Done. Also measure the panel's own VCC pad against GND while\n");
    printf("  you are there: it should be about 3.3V. If it reads 5V, this\n");
    printf("  module's own documentation says the supply and the logic have\n");
    printf("  to match, and 3.3V logic into a 5V-powered panel will not work.\n");
}

/* ------------------------------------------------------------------------
 * Bring the panel up at a given SPI clock, tearing down any previous one.
 *
 * Split out of stage_display() because the clock sweep needs it: esp_lcd
 * fixes pclk_hz when the panel IO handle is created, so there is no way to
 * change speed other than destroying the handle and building another. The
 * panel handle is built on top of the IO handle, so that goes too, and a new
 * panel has forgotten its init sequence -- hence re-sending the table here
 * rather than in the caller.
 *
 * The SPI *bus* is deliberately not touched. It is initialised once in
 * stage_display() and outlives every panel built on it.
 * ---------------------------------------------------------------------- */
bool bring_panel_up(int clock_hz, bool verbose) {
    if (g_panel != nullptr) {
        esp_lcd_panel_del(g_panel);
        g_panel = nullptr;
    }
    if (g_panel_io != nullptr) {
        esp_lcd_panel_io_del(g_panel_io);
        g_panel_io = nullptr;
    }

    esp_lcd_panel_io_spi_config_t io{};
    io.cs_gpio_num = KF_ESP_PIN_LCD_CS;
    io.dc_gpio_num = KF_ESP_PIN_LCD_DC;
    io.spi_mode = 0;
    io.pclk_hz = clock_hz;
    io.trans_queue_depth = 10;
    io.lcd_cmd_bits = 8;
    io.lcd_param_bits = 8;

    if (esp_lcd_new_panel_io_spi(static_cast<esp_lcd_spi_bus_handle_t>(SPI2_HOST),
                                 &io, &g_panel_io) != ESP_OK) {
        if (verbose) {
            fail(1, "could not attach the panel to SPI2",
                 "the CS and DC wires (see the pin line printed above)");
        }
        return false;
    }

    esp_lcd_panel_dev_config_t dev{};
    dev.reset_gpio_num = KF_ESP_PIN_LCD_RST;
    dev.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    dev.data_endian = LCD_RGB_DATA_ENDIAN_LITTLE;
    dev.bits_per_pixel = 16;
    dev.flags.reset_active_high = 0;

    /* esp_lcd_new_panel_st7789() supplies the handle for BOTH panels. On the
     * ILI9341 path its own init is skipped below and this file's table is
     * sent instead -- see kIli9341Init's comment for why that is sound. */
    if (esp_lcd_new_panel_st7789(g_panel_io, &dev, &g_panel) != ESP_OK) {
        if (verbose) {
            fail(1, "the panel driver would not initialise",
                 "the RST wire (see the pin line printed above)");
        }
        return false;
    }

    lcd_ok(esp_lcd_panel_reset(g_panel), "esp_lcd_panel_reset");
    if (!kPanelIsIli9341) {
        lcd_ok(esp_lcd_panel_init(g_panel), "esp_lcd_panel_init");
    }

    if (kPanelIsIli9341) {
        if (verbose) {
            info("Panel selected: ILI9341 (the 2.8in HiLetgo). Sending its own");
            info("full init, and skipping ESP-IDF's ST7789 init entirely:");
        }
        send_init_table(kIli9341Init,
                        (int)(sizeof(kIli9341Init) / sizeof(kIli9341Init[0])),
                        verbose);
    } else {
        if (verbose) {
            info("Panel selected: ST7789 (the 2in Waveshare). Sending the power");
            info("and gamma registers ESP-IDF's generic driver leaves at their");
            info("power-on defaults:");
        }
        send_init_table(kWaveshareInit,
                        (int)(sizeof(kWaveshareInit) / sizeof(kWaveshareInit[0])),
                        verbose);
    }

    lcd_ok(esp_lcd_panel_disp_on_off(g_panel, true), "disp_on_off(true)");
    return true;
}

void stage_display() {
    banner(kPanelIsIli9341 ? "STAGE 2: display over SPI (ILI9341, SPI2)"
                           : "STAGE 2: display over SPI (ST7789, SPI2)");
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

    if (!bring_panel_up(kLcdClockHz, /*verbose=*/true)) {
        return;
    }

    /* ------------------------------------------------------------------
     * Named colours, one at a time, slowly, with the serial line saying
     * which one is on screen right now.
     *
     * The point of announcing each fill is to separate two failures that
     * look identical to someone glancing at a panel: "the colours are
     * flickering past too fast to read" and "the panel is showing noise."
     * If the serial line says RED and the panel is solid anything, the SPI
     * path works and the remaining questions are about the controller's
     * settings. If the panel is fizzing static while the serial line says
     * RED, the data is arriving corrupted and no amount of controller
     * configuration will fix it.
     * ------------------------------------------------------------------ */
    struct NamedColor {
        const char *name;
        uint16_t value;
    };
    constexpr NamedColor kSequence[] = {
        {"RED", kRed}, {"GREEN", kGreen}, {"BLUE", kBlue},
        {"WHITE", kWhite}, {"BLACK", kBlack},
    };

    /* ------------------------------------------------------------------
     * Bisect before filling anything.
     *
     * "Noise, then black" has two completely different causes and they
     * need different fixes. Either the panel is fine and our pixel writes
     * are being executed as commands (which would eventually hit 0x28,
     * display off), or the panel never came up properly and goes dark on
     * its own. Writing a full red frame immediately destroys the evidence
     * for telling those apart, so: write nothing, then write almost
     * nothing, then write everything, narrating each step.
     * ------------------------------------------------------------------ */
    if (kPanelDebugMode) {
        info("");
        info("STEP A. The panel is initialised and switched on, and I am now");
        info("deliberately sending it NOTHING for 8 seconds. Whatever is on");
        info("the screen right now is the panel's own uninitialised memory.");
        info("  still fuzzy after 8s -> the panel is alive and our writes are");
        info("                          what kills it. That is a DC problem.");
        info("  goes black by itself -> the panel is not staying awake, which");
        info("                          is reset or power, not the data path.");
        vTaskDelay(pdMS_TO_TICKS(8000));

        info("");
        info("STEP B. Writing ONE small red square, 60x60, near the top-left.");
        info("Nothing else on the screen is touched.");
        info("  a red square appears -> the data path works and the problem is");
        info("                          somewhere in the full-frame path");
        info("  no square, and the");
        info("    screen goes black  -> those pixel bytes were executed as");
        info("                          commands. DC again, whatever continuity");
        info("                          says -- check it while the board runs.");
        draw_square(kRed, 20, 20, 60);
        vTaskDelay(pdMS_TO_TICKS(8000));

        info("");
        info("STEP C. A second square, GREEN, further down. If the red one is");
        info("still there and a green one joins it, the panel is genuinely");
        info("working and only the full-frame writes are the problem.");
        draw_square(kGreen, 20, 120, 60);
        vTaskDelay(pdMS_TO_TICKS(8000));
    }

    /* Kept out of the debug gate above: seven seconds, and it is the one
     * test that separates a byte-order fault from a BGR fault, because white
     * and black are invariant under both while the saturated fills are not.
     * That is worth paying for on every run. */
    info("");
    info("Full-screen fills, one second each.");
    for (const NamedColor &c : kSequence) {
        printf("           ... now showing %s\n", c.name);
        fill_screen(c.value);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    /* Inversion OFF is the settled answer for this module, measured: the
     * test frame came out on a white field with it off and a black one with
     * it on. This used to be an A/B pair that drew the frame twice and asked
     * which looked right; now that the answer is known it is one explicit
     * call, so the panel is never left in an unknown inversion state.
     *
     * Worth keeping in mind for the next panel: ST7789 silicon defaults to
     * inversion OFF, but many IPS modules built around it are wired to need
     * INVON instead, so this is a per-module question rather than a
     * per-controller one. */
    lcd_ok(esp_lcd_panel_invert_color(g_panel, false), "invert_color(false)");

    const int card_hold_ms = kPanelDebugMode ? 90000 : 10000;

    info("");
    info("The test card. It stays up for %d seconds.", card_hold_ms / 1000);
    if (kPanelDebugMode) {
        info("That is long enough to photograph -- take a photo and send it.");
    }
    info("");
    info("What SHOULD be on the glass:");
    info("  a RED bar across the top, with a WHITE notch at its LEFT end");
    info("  a GREEN stripe down the LEFT edge only");
    info("  a BLUE bar across the bottom");
    info("  and eight patches in the middle, two across, four down:");
    info("        WHITE   BLACK");
    info("        RED     GREEN");
    info("        BLUE    YELLOW");
    info("        CYAN    MAGENTA");
    info("");
    info("This is drawn for the module mounted with its PIN HEADER AT THE");
    info("TOP -- the mounting ADR 0024 settled on, and what MADCTL 0x88");
    info("encodes. Header at the bottom is the same image upside down, and");
    info("is a one-byte change (0x48) rather than a fault.");
    draw_test_card();
    vTaskDelay(pdMS_TO_TICKS(card_hold_ms));

    /* Left in whichever state the panel was last put into, so the button
     * stage in stage 5 is readable either way. */
    if (g_lcd_errors == 0) {
        pass(1, "%d frames pushed at %d MHz, every esp_lcd call returned OK.",
             (int)(sizeof(kSequence) / sizeof(kSequence[0])) + 4,
             kLcdClockHz / 1000000);
        info("That means the ESP32 clocked the bytes out. It does NOT mean");
        info("the panel received them: this bus is write-only, there is no");
        info("SDO pin on this module, and nothing is ever read back.");
    } else {
        fail(1, "some esp_lcd calls returned errors -- see the ** lines above",
             "the errors themselves, not the wiring");
    }
    info("");
    info("Reading the test card -- what each fault looks like:");
    info("  card is correct       -> nothing to change, the panel is done");
    info("  WHITE and BLACK are");
    info("    right, the colour");
    info("    patches are not     -> framebuffer byte order. This is the one");
    info("                           that cost an evening: white and black");
    info("                           survive a byte swap, saturated colours");
    info("                           do not. See kPanelIsIli9341's comment.");
    info("  EVERY patch is wrong,");
    info("    but each is SOLID   -> rgb_ele_order should be BGR, not RGB");
    info("  whole card inverted   -> this panel needs invert_color(true)");
    info("  fizzing static        -> DC (GPIO%d) first, then wire length",
         KF_ESP_PIN_LCD_DC);
    info("  nothing at all        -> SCLK (GPIO%d) or MOSI (GPIO%d)",
         KF_ESP_PIN_LCD_SCLK, KF_ESP_PIN_LCD_MOSI);
    info("  bars clipped at an");
    info("    edge                -> the panel needs an offset "
         "(esp_lcd_panel_set_gap)");
    info("  green stripe on the");
    info("    RIGHT, not the left -> MADCTL mirror/swap_xy settings");
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

/* Days since 1970-01-01 from a civil date -- the standard days_from_civil
 * algorithm. Written out rather than reached for via mktime() because
 * mktime() applies a timezone and the DS3231 has no concept of one. The
 * comparison here only needs two readings to sit on the same monotonic
 * scale, and a hand-rolled conversion cannot be wrong about a timezone it
 * never consults. Valid for any date this project will ever see. */
int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= (m <= 2) ? 1 : 0;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    /* March-based month index, 0-11: March is 0, February is 11. Spelled out
     * rather than the usual `m + (m > 2 ? -3 : 9)` so it does not depend on
     * unsigned wraparound to be correct. */
    const unsigned mp = (m > 2) ? (m - 3u) : (m + 9u);
    const unsigned doy = (153u * mp + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) - 719468;
}

/* The DS3231's seven time registers (0x00-0x06) as a single comparable
 * number. Masks match the datasheet: bit 7 of seconds is unused, bit 6 of
 * hours selects 12/24h mode (this diagnostic always writes 24h), and the
 * top three bits of the month register carry the century flag. */
int64_t ds3231_regs_to_epoch(const uint8_t regs[7]) {
    const unsigned sec = bcd_to_bin(static_cast<uint8_t>(regs[0] & 0x7F));
    const unsigned min = bcd_to_bin(regs[1]);
    const unsigned hour = bcd_to_bin(static_cast<uint8_t>(regs[2] & 0x3F));
    const unsigned day = bcd_to_bin(static_cast<uint8_t>(regs[4] & 0x3F));
    const unsigned month = bcd_to_bin(static_cast<uint8_t>(regs[5] & 0x1F));
    const unsigned year = 2000u + bcd_to_bin(regs[6]);

    return days_from_civil(static_cast<int>(year), month, day) * 86400 +
           static_cast<int64_t>(hour) * 3600 + static_cast<int64_t>(min) * 60 +
           static_cast<int64_t>(sec);
}

/* Compare this run's clock reading against the one the previous run stored,
 * then store this one for the next. `oscillator_was_stopped` is the OSF flag
 * as it was found on entry to stage 3, BEFORE that stage reseeds the clock --
 * by the time this is called the flag has already been cleared, so it has to
 * be passed in rather than re-read. */
void check_cold_boot(const uint8_t now_regs[7], bool oscillator_was_stopped) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        info("could not open NVS (%d) -- skipping the power-off check.", err);
        return;
    }

    nvs_handle_t nvs = 0;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &nvs) != ESP_OK) {
        info("could not open NVS namespace -- skipping the power-off check.");
        return;
    }

    const int64_t now_epoch = ds3231_regs_to_epoch(now_regs);

    int64_t previous = 0;
    const bool have_previous =
        nvs_get_i64(nvs, kNvsKeyRtcEpoch, &previous) == ESP_OK;

    if (!have_previous) {
        g_cold_boot = ColdBoot::BaselineSaved;
    } else if (oscillator_was_stopped) {
        /* Checked before the time comparison on purpose: when OSF is set the
         * clock has already been reseeded above, so the times would also look
         * wrong -- but "the backup cell is not holding" is the real finding
         * and "time went backwards" is only its symptom. */
        g_cold_boot = ColdBoot::FailedOscillatorStopped;
    } else if (now_epoch > previous) {
        g_cold_boot = ColdBoot::Passed;
        g_cold_boot_gap_s = now_epoch - previous;
    } else {
        g_cold_boot = ColdBoot::FailedNoAdvance;
        g_cold_boot_gap_s = now_epoch - previous;
    }

    /* Stored unconditionally, including after a failure: a failed run should
     * still leave a usable baseline for the next attempt rather than making
     * someone run it twice more to get back to a comparison. */
    nvs_set_i64(nvs, kNvsKeyRtcEpoch, now_epoch);
    nvs_commit(nvs);
    nvs_close(nvs);
}

/* Printed under the summary table rather than in it -- see ColdBoot's own
 * comment for why this is not a PASS/FAIL row. */
void print_cold_boot_verdict() {
    switch (g_cold_boot) {
    case ColdBoot::NotChecked:
        printf("  RTC survives power-off             NOT CHECKED\n");
        printf("    Stage 3 did not get far enough to read a time.\n");
        break;

    case ColdBoot::BaselineSaved:
        printf("  RTC survives power-off             RUN AGAIN TO FIND OUT\n");
        printf("    This run saved a baseline. Now do exactly this:\n");
        printf("      1. unplug the board completely -- USB out, not just RST\n");
        printf("      2. wait 30 seconds\n");
        printf("      3. plug it back in and let this run again\n");
        printf("    The next run will compare the clock against the baseline\n");
        printf("    and tell you PASS or FAIL itself. Nothing to remember.\n");
        break;

    case ColdBoot::Passed:
        printf("  RTC survives power-off             PASS\n");
        printf("    The clock advanced %lld seconds since the previous run,\n",
               static_cast<long long>(g_cold_boot_gap_s));
        printf("    across a power cut, on coin-cell power alone. That is the\n");
        printf("    whole feature: the pet can age while the device is off.\n");
        break;

    case ColdBoot::FailedOscillatorStopped:
        printf("  RTC survives power-off             FAIL\n");
        printf("    The OSF flag was set again this boot, which means the\n");
        printf("    clock lost time while unpowered. In order of likelihood:\n");
        printf("      - the CR2032 is in backwards (+ faces up, away from the board)\n");
        printf("      - the coin cell holder is not making contact -- press it\n");
        printf("      - the cell is flat. A fresh one reads 3.0V+ on a meter\n");
        printf("      - the trickle-charge resistor marked 201 was never removed,\n");
        printf("        and has been quietly killing a non-rechargeable cell\n");
        break;

    case ColdBoot::FailedNoAdvance:
        printf("  RTC survives power-off             FAIL\n");
        printf("    OSF stayed clear, but the clock did not move forward:\n");
        printf("    it read %lld seconds relative to the previous run.\n",
               static_cast<long long>(g_cold_boot_gap_s));
        printf("    A clock that holds a value but does not advance while\n");
        printf("    unpowered usually means the crystal is not oscillating\n");
        printf("    off USB power -- most often a counterfeit DS3231 module.\n");
        break;
    }
}

/* ------------------------------------------------------------------------
 * Stage 2b: how fast can this panel actually be driven?
 *
 * KF_DISPLAY_SPI_HZ in kf/budget.h is 40MHz and is marked ASSUMPTION, NOT
 * MEASURED. Every claim the desktop simulator makes about transfer cost and
 * frame budget rests on it, so it is the one number bring-up exists to
 * replace with a measured figure.
 *
 * A full 240x320 RGB565 frame is 153,600 bytes, so the clock maps straight
 * onto a frame rate ceiling:
 *
 *     4MHz  -> 307ms/frame ->  ~3fps
 *    10MHz  -> 123ms/frame ->  ~8fps
 *    20MHz  ->  61ms/frame -> ~16fps
 *    40MHz  ->  31ms/frame -> ~32fps     <- what budget.h assumes
 *    80MHz  ->  15ms/frame -> ~65fps
 *
 * The firmware cannot score this itself, and it is important to be honest
 * about why: this bus is write-only, there is no data-out pin on either
 * module, and nothing is ever read back. esp_lcd returning ESP_OK means the
 * ESP32 clocked bytes out of its own pin. At the speed where the wires give
 * up, every call still returns ESP_OK and the picture is simply wrong. So
 * the panel is driven at each speed in turn and a human reads the glass.
 *
 * 80MHz is on the list because SPI2's IOMUX pins on the ESP32-S3 are exactly
 * the ones this pinout uses -- CLK=12, MOSI=11, CS=10 -- which is the
 * configuration that can bypass the GPIO matrix and reach the full rate.
 * Whether breadboard jumpers can is the actual question.
 * ---------------------------------------------------------------------- */
void stage_clock_sweep() {
    banner("STAGE 2b: how fast can the panel be driven?");

    if (g_panel == nullptr) {
        info("Skipped: stage 2 never brought a panel up.");
        return;
    }

    constexpr int kClocks[] = {4, 10, 20, 40, 80};
    constexpr int kHoldMs = 7000;

    info("The test card will be redrawn at each speed below, %d seconds",
         kHoldMs / 1000);
    info("each. WATCH THE GLASS, not this log -- the log will say every");
    info("step succeeded even at a speed that is plainly broken.");
    info("");
    info("You are looking for the highest speed where the card is still");
    info("PERFECT: solid patches, clean edges, no fizzing, no torn or");
    info("shifted rows, no colour speckle. Note the first speed that");
    info("misbehaves; the one below it is the answer.");
    info("");

    int last_good_attempted = 0;
    for (const int mhz : kClocks) {
        const int hz = mhz * 1000 * 1000;
        const int errors_before = g_lcd_errors;

        printf("\n  ---- now driving at %d MHz ", mhz);
        printf("(%d ms per full frame, ~%d fps ceiling) ----\n",
               (240 * 320 * 2 * 8) / (mhz * 1000), mhz * 1000000 / (240 * 320 * 2 * 8));

        if (!bring_panel_up(hz, /*verbose=*/false)) {
            printf("     esp_lcd REFUSED this clock outright -- the driver would\n");
            printf("     not create a panel at %d MHz. That is a hard ceiling in\n", mhz);
            printf("     software, not a wiring limit. Stopping here.\n");
            break;
        }

        draw_test_card();
        vTaskDelay(pdMS_TO_TICKS(kHoldMs));

        if (g_lcd_errors > errors_before) {
            printf("     ** %d esp_lcd call(s) reported an error at this speed.\n",
                   g_lcd_errors - errors_before);
            printf("     That is unusual -- normally the calls succeed and only\n");
            printf("     the picture degrades. Treat this speed as failed.\n");
        } else {
            last_good_attempted = mhz;
        }
    }

    /* Back to the known-good clock so stage 5's button bands are readable
     * regardless of how far up the sweep got. */
    printf("\n");
    info("Sweep done. Returning the panel to %d MHz for the button stage.",
         kLcdClockHz / 1000000);
    bring_panel_up(kLcdClockHz, /*verbose=*/false);
    draw_test_card();

    info("");
    info("Every speed up to %d MHz was driven without a driver error.",
         last_good_attempted);
    info("That is NOT the answer -- only your eyes are. Tell me the highest");
    info("speed where the card still looked perfect, and I will set");
    info("KF_DISPLAY_SPI_HZ in kf/budget.h to it and drop its ASSUMPTION");
    info("banner. If even 4 MHz looked wrong, something else is at fault");
    info("and the speed is not the problem.");
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

    /* t2 rather than t1: it is two seconds newer and both were read from a
     * clock already confirmed to be advancing. */
    check_cold_boot(t2, oscillator_stopped);
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
    {KF_ESP_PIN_BTN_UP, "UP", kRed},       {KF_ESP_PIN_BTN_DOWN, "DOWN", wire(0xFD20)},
    {KF_ESP_PIN_BTN_LEFT, "LEFT", kYellow}, {KF_ESP_PIN_BTN_RIGHT, "RIGHT", kGreen},
    {KF_ESP_PIN_BTN_A, "A", kCyan},        {KF_ESP_PIN_BTN_B, "B", kBlue},
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
        lcd_ok(esp_lcd_panel_draw_bitmap(g_panel, 0, y, kWidth, y + rows, g_strip),
               "draw_bitmap (strip)");
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
    print_cold_boot_verdict();

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
    if (kPanelDebugMode) {
        /* 60 seconds and a multimeter. Off unless a panel is actually dark
         * -- see kPanelDebugMode's own comment. */
        stage_pin_wiggle();
    }
    stage_display();
    if (kRunClockSweep) {
        stage_clock_sweep();
    }
    stage_i2c_and_rtc();
    stage_sd_card();
    print_summary();
    stage_buttons();
}
