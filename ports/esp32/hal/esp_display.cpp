/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * HAL backend: display, ESP32 implementation.
 *
 * This file knows how to talk to a 240x320 SPI panel. It does NOT know which
 * one. Everything panel-specific -- init sequence, orientation, colour
 * inversion, framebuffer byte order, window offset -- is data in
 * kf_panel_profile.h, so supporting another module means adding a table
 * there and nothing here changes.
 *
 * That split is deliberate and is a product requirement, not a tidiness
 * preference: Kamiframe is meant to be buildable with whatever 240x320 SPI
 * panel someone can source. Two are supported today (see kf_panel_profile.h)
 * and neither is a special case in this file.
 *
 * Three decisions worth naming, matching sdl_display.cpp's own honesty
 * comment:
 *
 * 1. supports_partial_update is TRUE, and dirty rectangles are honoured. This
 *    started life as an optimisation to do later and turned out to be a
 *    correctness requirement, found on hardware: re-sending an unchanged
 *    frame is not merely wasteful. The panel scans its own memory out to the
 *    glass continuously on its own clock, and nothing on this board wires up
 *    a tearing-effect signal to say when writing is safe, so rewriting all
 *    153,600 bytes underneath that scan 30 times a second leaves a permanent
 *    moving boundary between old and new contents. To the eye that is a
 *    pulsing band travelling down the screen. A still pet now costs zero
 *    bytes and the flicker has nowhere to come from.
 *
 *    It is also most of the frame budget: a full frame is ~31ms of wire time
 *    at the measured 40MHz, against a 33ms budget at 30fps.
 *
 * 2. Byte order is per-panel, and on a panel that needs swapping it costs a
 *    pass over the frame. See kf_display_present() for the reasoning and
 *    what it costs; kf_panel_profile.h explains why the panels differ.
 *
 * 3. Backlight is a plain GPIO on/off, not PWM. kf_display_set_backlight()
 *    treats any level > 0 as "on" -- there is no LEDC channel wired up here,
 *    so dimming is not implemented. Adding it later is a self-contained
 *    change to one function. has_backlight (g_caps) reflects whether THIS
 *    build actually owns GPIO6 for it, which depends on the active panel
 *    profile -- see kf_display_init() and ADR 0039. kf_display_init() also
 *    calls kf_display_set_backlight() itself, once, right at the end: this
 *    is the one and only caller anywhere in the tree, and without it the
 *    GPIO is configured but left driven low forever, which is a black panel
 *    with a perfectly healthy log.
 */

#include "kf/hal/display.h"

#include "kf/budget.h"
#include "kf/hal/log.h"
#include "kf/hal/time.h" /* kf_time_mono_us() -- the vsync wait's clock, see push_rect() */
#include "kf_dbg_bridge.h" /* KF_DBG_BRIDGE_ENABLE -- gates the MISO/SCANLINE/vsync pieces below */
#include "kf_esp_display_diag.h"
#include "kf_esp_display_vsync.h"
#include "kf_esp_pins.h"
#include "kf_panel_profile.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstring>

namespace {

constexpr const char *TAG = "display";

constexpr spi_host_device_t kSpiHost = SPI2_HOST;

/* The panel this build drives. One line, resolved at compile time. */
const kf_panel_profile &kPanel = KF_PANEL_PROFILE;

/* Rows per staging transfer on panels that need a byte swap. 240 x 40 x 2 =
 * 19,200 bytes, which is a comfortable DMA-capable internal-RAM allocation --
 * a full frame would be 153,600 and internal RAM is only 512KB in total. */
constexpr int kSwapStripRows = 40;

/* TWO strip buffers, alternating, and the reason is not performance.
 *
 * esp_lcd_panel_draw_bitmap() does NOT block until the pixels are on the
 * wire. It queues a DMA transaction and returns. The transfer is only waited
 * for at the START of the next tx_color call, where esp_lcd_panel_io_spi.c
 * drains num_trans_inflight before queuing anything new.
 *
 * So a single shared strip buffer is a data race with DMA: the swap loop for
 * strip N+1 overwrites the very bytes strip N's transfer is still reading.
 * The panel then shows whichever side won, which looks like duplicated bands,
 * a picture shifted off the top of the screen, and a brightness flicker that
 * changes every frame -- all three at once, and all three only on hardware,
 * because no desktop backend has DMA.
 *
 * Alternating two buffers fixes it exactly: between two writes to the same
 * buffer there is always one intervening draw_bitmap() call, and that call
 * drains the earlier transfer before it queues its own. It also keeps the
 * CPU's byte-swap of the next strip overlapping the current transfer, which
 * a blocking wait would have thrown away.
 *
 * g_swap_index deliberately persists across frames rather than resetting to
 * zero. Resetting would be correct only while the strip count per frame
 * happens to be even -- 320/40 = 8 today -- and would silently become a race
 * again if either constant changed. */
constexpr int kSwapBufferCount = 2;

esp_lcd_panel_io_handle_t g_io = nullptr;
esp_lcd_panel_handle_t g_panel = nullptr;
bool g_spi_bus_initialized = false;
uint16_t *g_swap_strip[kSwapBufferCount] = {nullptr, nullptr};
int g_swap_index = 0;

kf_display_caps g_caps = {
    KF_DISPLAY_WIDTH,
    KF_DISPLAY_HEIGHT,
    KF_PIXFMT_RGB565,
    /* supports_partial_update: true -- see header comment above. */
    true,
    /* has_backlight: a placeholder default, corrected by kf_display_init()
     * before anything else reads g_caps -- it depends on whether this
     * build's panel profile has claimed GPIO6 for a read line instead (see
     * own_backlight_pin in kf_display_init() and ADR 0039). Left true here
     * rather than false only so a hypothetical caller reading caps before
     * init() has run sees the more common case; kf/hal/display.h's own
     * contract is that caps are only valid "after init" anyway. */
    true,
    /* link_bytes_per_second: the real, configured SPI clock, not an
     * estimate -- this is the one figure the desktop backend has to fake
     * by borrowing KF_DISPLAY_SPI_HZ from budget.h; here it is simply true. */
    KF_DISPLAY_SPI_HZ / 8u,
};

/* Send a profile's init table straight down the command channel. Commands and
 * their parameters ARE byte-reversed by esp_lcd, unlike colour data, so
 * nothing here needs the swapping present() does. */
void send_init_table(const kf_panel_profile &panel) {
    for (size_t i = 0; i < panel.init_count; ++i) {
        const kf_panel_cmd &c = panel.init[i];
        const esp_err_t err = esp_lcd_panel_io_tx_param(
            g_io, c.cmd, c.len ? c.data : nullptr, c.len);
        if (err != ESP_OK) {
            KF_LOGE(TAG, "init cmd 0x%02X failed: %d", c.cmd, err);
        }
        if (c.delay_ms != 0) {
            vTaskDelay(pdMS_TO_TICKS(c.delay_ms));
        }
    }
}

/* Tears down the current panel IO/panel (if any) and rebuilds both at
 * clock_hz, re-sending the panel's init table so the controller ends up in
 * exactly the same known state regardless of which clock it was built at.
 *
 * The ONE place that creates g_io/g_panel: kf_display_init() below calls it
 * once, at KF_DISPLAY_SPI_HZ, for the normal write path. The KFDBG SCANLINE
 * probe (near the bottom of this file, under KF_DBG_BRIDGE_ENABLE) calls it
 * twice more, at a slow read clock and back, to get a genuinely slower read
 * without a second SPI device fighting over the shared CS pin -- see
 * kf_esp_display_diag_begin_probe()'s own comment for why that is the only
 * safe way to do it. Having exactly one function build these handles is
 * what keeps the write path and the probe path from drifting out of sync
 * with each other.
 *
 * Mirrors ports/esp32-bringup/main/bringup_main.cpp's bring_panel_up(),
 * which does this same dance and is known to work on real hardware --
 * follow that structure if this ever needs to change.
 *
 * Returns false on failure. Whatever this function deleted before the
 * failing call stays deleted: esp_lcd_new_panel_io_spi() and esp_lcd_new_
 * panel_st7789() both leave their out-param untouched when they fail, and
 * this function had already nulled it going in. A false return therefore
 * can mean g_io and/or g_panel are null afterwards -- every caller must
 * treat that as "the display may now be down" rather than assume either
 * handle is still valid. kf_display_present() already tolerates
 * g_panel == nullptr (returns KF_ERR_UNAVAILABLE instead of dereferencing
 * it); every caller of this function relies on that guard as the safety
 * net for a failed rebuild. */
bool rebuild_panel_io(uint32_t clock_hz) {
    if (g_panel != nullptr) {
        esp_lcd_panel_del(g_panel);
        g_panel = nullptr;
    }
    if (g_io != nullptr) {
        esp_lcd_panel_io_del(g_io);
        g_io = nullptr;
    }

    esp_lcd_panel_io_spi_config_t io_config{};
    io_config.cs_gpio_num = KF_ESP_PIN_LCD_CS;
    io_config.dc_gpio_num = KF_ESP_PIN_LCD_DC;
    io_config.spi_mode = 0;
    io_config.pclk_hz = clock_hz;
    io_config.trans_queue_depth = 10;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    /* flags left at their default (all zero): dc_cmd_level = 0, dc_data_level
     * = 1, the standard "DC low = command, DC high = data" wiring these panels
     * use -- confirmed against esp_lcd_panel_io_spi.c rather than assumed.
     * Unchanged by which clock is in use. */

    esp_err_t err = esp_lcd_new_panel_io_spi(
        static_cast<esp_lcd_spi_bus_handle_t>(kSpiHost), &io_config, &g_io);
    if (err != ESP_OK) {
        KF_LOGE(TAG, "esp_lcd_new_panel_io_spi failed at %lu Hz: %d",
                static_cast<unsigned long>(clock_hz), err);
        return false;
    }

    esp_lcd_panel_dev_config_t panel_config{};
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    /* Only consulted on the built-in-init path: it is what makes
     * esp_lcd_panel_init() set RAMCTRL's little-endian bit. On a panel with
     * big_endian_fb the swap happens in present() instead, and this is
     * ignored because esp_lcd_panel_init() is never called. */
    panel_config.data_endian = LCD_RGB_DATA_ENDIAN_LITTLE;
    panel_config.bits_per_pixel = 16;
    panel_config.reset_gpio_num = KF_ESP_PIN_LCD_RST;
    panel_config.flags.reset_active_high = 0;

    /* esp_lcd_new_panel_st7789() supplies the handle for every panel here,
     * including the ILI9341. That is not a mistake: the handle only provides
     * the generic draw_bitmap/reset/invert plumbing, and the controller-
     * specific part is whether esp_lcd_panel_init() is allowed to run --
     * which is exactly what use_builtin_init decides. */
    err = esp_lcd_new_panel_st7789(g_io, &panel_config, &g_panel);
    if (err != ESP_OK) {
        KF_LOGE(TAG, "esp_lcd_new_panel_st7789 failed at %lu Hz: %d",
                static_cast<unsigned long>(clock_hz), err);
        return false;
    }

    esp_lcd_panel_reset(g_panel);

    /* Extra settle time beyond what esp_lcd_panel_reset() itself waits.
     *
     * That call's actual work (esp_lcd_panel_st7789.c's panel_st7789_reset(),
     * the shared handle every profile here goes through -- see this
     * function's own header comment) toggles the hardware reset line and
     * waits 10ms low, 10ms high: 20ms total, fixed, regardless of what state
     * the panel was in before the reset. That is enough for the ILI9341
     * datasheet's OWN number when reset is applied to a panel that was
     * already asleep -- true the very first time this function ever runs,
     * from kf_display_init(), because the controller powers on asleep by
     * default. It is NOT enough for the datasheet's number when reset is
     * applied to a panel that was awake and actively displaying -- which is
     * exactly what happens on the two extra calls KFDBG SCANLINE's probe
     * makes (kf_esp_display_diag_begin_probe() and _end_probe(), both
     * calling this same function while the panel was mid-render): the
     * datasheet documents a full 120ms of settling before the controller can
     * be trusted to take a Sleep Out command correctly in that case, not
     * 20ms. send_init_table() below sends roughly twenty short setup
     * commands before this profile's own Sleep Out (see kf_panel_profile.h's
     * ILI9341 table, which already budgets 150ms AFTER Sleep Out for the
     * same datasheet number, correctly) -- but nothing budgeted the 120ms
     * BEFORE it, on the awake-reset path, which is the confirmed, most
     * likely explanation for the horizontal banding left on the panel after
     * a SCANLINE run: the second and third resets in a run undershoot a
     * datasheet timing requirement the first one happens to satisfy by
     * accident.
     *
     * This one extra wait covers both cases rather than branching on which
     * reset this is: 120ms is wasted on the cold-boot call (which only
     * needed the datasheet's shorter already-asleep number), but that cost
     * is paid exactly once, at startup, not per frame and not per SCANLINE
     * sample, so it is not worth the extra state tracking a branch would
     * need. NOT verified against real hardware -- there is none on hand this
     * session, and this is reasoned from the documented ILI9341 reset/Sleep
     * Out timing rules, not measured. If banding is still visible after a
     * SCANLINE run once this is on real hardware, that is the first thing to
     * re-check; kf_debug.py's `scanline` command says as much in its own
     * output too, so a human is not left guessing. */
    vTaskDelay(pdMS_TO_TICKS(120));

    if (kPanel.use_builtin_init) {
        esp_lcd_panel_init(g_panel);
    }
    send_init_table(kPanel);
    esp_lcd_panel_invert_color(g_panel, kPanel.invert);
    if (kPanel.x_gap != 0 || kPanel.y_gap != 0) {
        esp_lcd_panel_set_gap(g_panel, kPanel.x_gap, kPanel.y_gap);
    }
    esp_lcd_panel_disp_on_off(g_panel, true);
    return true;
}

} // namespace

kf_result kf_display_init(void) {
    KF_LOGI(TAG, "panel profile: %s", kPanel.name);

    /* MISO: reserved on the bus only when the debug bridge is compiled in
     * AND the active panel profile has a read line to reserve it for.
     *
     * Two independent questions used to be conflated into one: "is the
     * debug bridge compiled in" is a build-time policy decision about how
     * much of KFDBG to ship; "does GPIO6 carry a real signal" is a fact
     * about the panel screwed to the board, and the two panels this file
     * drives disagree about it. The HiLetgo ILI9341 has a real SDO pin,
     * wired to GPIO6 for the KFDBG SCANLINE diagnostic (see kf_esp_pins.h's
     * KF_ESP_PIN_LCD_MISO comment, and kf_dbg_bridge.cpp for what SCANLINE
     * does with it). The Waveshare ST7789's eight-pin header has no SDO at
     * all -- kf_panel_profile.h's has_read_line field is what tells this
     * function which case it is in, per ADR 0039.
     *
     * A plain `if`, not `#if`/`#else`: KF_DBG_BRIDGE_ENABLE is a compile-time
     * 0/1 and kPanel (line 73) resolves at compile time too, so the compiler
     * folds this exactly as a #if would, but the code on both sides of the
     * condition is real, type-checked C++ in every configuration -- the same
     * shape ADR 0035 chose for the KFDBG mutate gate, and for the same
     * reason: less code that can silently drift out of sync with its
     * `#else`.
     *
     * reserve_miso_for_read_line == false leaves miso_io_num at -1, which
     * reproduces this file's exact pre-diagnostic behaviour: no pin
     * reserved, no side effect on anything. That is now the ST7789's
     * permanent state, on every KF_DBG_BRIDGE_ENABLE setting, not just a
     * fallback for the bridge being off.
     *
     * When it IS reserved (only ever the ILI9341, and only when the bridge
     * is compiled in), there is a real cost beyond "one more pin is spoken
     * for", worth being honest about rather than burying in a
     * diagnostic-only comment: GPIO6 is not this bus's native IOMUX MISO pin
     * (that is GPIO13 -- already spent on I2C, see kf_esp_pins.h), and
     * ESP-IDF's spi_bus_initialize() only grants the IOMUX fast path when
     * EVERY configured pin -- MOSI, MISO, SCLK, CS -- matches the
     * peripheral's native set (confirmed by reading spicommon_bus_
     * initialize_io()'s bus_uses_iomux_pins() in ESP-IDF's spi_common.c, not
     * assumed). A non-native MISO therefore drops MOSI, SCLK and CS onto the
     * GPIO matrix too, not just MISO -- for the WHOLE bus, on every
     * ILI9341 + KF_DBG_BRIDGE_ENABLE=1 build, not only while a SCANLINE
     * command is actually running. docs/hardware-bringup.md's measured
     * 40MHz write ceiling was measured on the IOMUX path (CLK12/MOSI11/CS10
     * are exactly SPI2's native pins) and has never been re-measured on the
     * GPIO-matrix path this combination forces -- worth re-running that
     * clock sweep once real ILI9341 + bridge-enabled hardware is back on the
     * bench, rather than assuming 40MHz still holds. The ST7789 never pays
     * this cost at all: has_read_line == false keeps miso_io_num at -1
     * unconditionally for that profile, so its bus stays on the IOMUX fast
     * path the 40MHz figure was measured on -- moving this decision into the
     * panel profile is what hands the ST7789 back the fast path it would
     * otherwise have lost to a diagnostic pin it has no wiring for at all.
     * That is reasoning from ESP-IDF's own bus-matching rule, not a fresh
     * clock measurement -- there is no ST7789 on hand this session to
     * measure with.
     *
     * max_transfer_sz covers a full frame, since the no-swap path issues
     * exactly one draw_bitmap() per frame. */
    const bool reserve_miso_for_read_line = KF_DBG_BRIDGE_ENABLE && kPanel.has_read_line;

    spi_bus_config_t bus_config{};
    bus_config.mosi_io_num = KF_ESP_PIN_LCD_MOSI;
    bus_config.miso_io_num = reserve_miso_for_read_line ? KF_ESP_PIN_LCD_MISO : -1;
    bus_config.sclk_io_num = KF_ESP_PIN_LCD_SCLK;
    bus_config.quadwp_io_num = -1;
    bus_config.quadhd_io_num = -1;
    bus_config.max_transfer_sz =
        KF_DISPLAY_WIDTH * KF_DISPLAY_HEIGHT * static_cast<int>(sizeof(kf_color));

    esp_err_t err = spi_bus_initialize(kSpiHost, &bus_config, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        KF_LOGE(TAG, "spi_bus_initialize failed: %d", err);
        return KF_ERR_IO;
    }
    g_spi_bus_initialized = true;

    /* Builds g_io and g_panel at the normal write clock and sends the
     * panel's init table -- see rebuild_panel_io()'s own comment for why
     * this is the one function that does that, shared with the KFDBG
     * SCANLINE probe's temporary rebuild at a slow read clock and back. */
    if (!rebuild_panel_io(KF_DISPLAY_SPI_HZ)) {
        return KF_ERR_IO;
    }

    if (kPanel.big_endian_fb) {
        constexpr size_t kStripBytes = static_cast<size_t>(KF_DISPLAY_WIDTH) *
                                        kSwapStripRows * sizeof(uint16_t);
        for (int i = 0; i < kSwapBufferCount; ++i) {
            g_swap_strip[i] = static_cast<uint16_t *>(
                heap_caps_malloc(kStripBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
            if (g_swap_strip[i] == nullptr) {
                KF_LOGE(TAG, "could not allocate byte-swap strip %d (%zu bytes)",
                        i, kStripBytes);
                return KF_ERR_EXHAUSTED;
            }
        }
    }

    /* Backlight GPIO, plain push-pull output. Configured whenever this build
     * did NOT just reserve GPIO6 for MISO above -- own_backlight_pin is
     * exactly !reserve_miso_for_read_line, so the two blocks can never both
     * claim the pin. This is now a runtime condition on the panel profile,
     * not a `#if !KF_DBG_BRIDGE_ENABLE` -- see the MISO comment above for
     * why that shape was wrong in general (it asked "is the bridge
     * compiled in" when the real question is "does this profile have a
     * read line").
     *
     * Configuring GPIO6 as a push-pull output while the SPI peripheral's
     * input matrix is ALSO listening on it for KFDBG SCANLINE's reads would
     * put the ESP32's own output driver in direct electrical contention
     * with the panel's SDO pin -- not a theoretical concern, a dead short
     * between two active drivers the moment they disagree. That is exactly
     * the ILI9341 + KF_DBG_BRIDGE_ENABLE=1 case, and own_backlight_pin is
     * false there, so this block is skipped for it, same as before this
     * change. On that same panel it costs nothing real either way: its LED
     * pin is wired straight to 3V3 (kf_esp_pins.h), so this GPIO has never
     * controlled the ILI9341's actual backlight in hardware regardless of
     * which side of this `if` runs.
     *
     * The ST7789 is the profile this exists for: has_read_line == false
     * means own_backlight_pin is true unconditionally for it, on every
     * KF_DBG_BRIDGE_ENABLE setting, and its BL pin is real -- this is the
     * only path in the whole tree that will ever drive it.
     *
     * Configured OFF here (matches the pin's reset state and the fact that
     * nothing has been presented to the panel yet); turned on explicitly,
     * below, once the panel is fully brought up. */
    const bool own_backlight_pin = !reserve_miso_for_read_line;
    if (own_backlight_pin) {
        gpio_config_t bl_config{};
        bl_config.pin_bit_mask = (1ULL << KF_ESP_PIN_LCD_BL);
        bl_config.mode = GPIO_MODE_OUTPUT;
        bl_config.pull_up_en = GPIO_PULLUP_DISABLE;
        bl_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
        bl_config.intr_type = GPIO_INTR_DISABLE;
        gpio_config(&bl_config);
        gpio_set_level(KF_ESP_PIN_LCD_BL, 0);
    }
    g_caps.has_backlight = own_backlight_pin;

    /* Turn the backlight on, now that the panel is fully brought up: the
     * init table has run and rebuild_panel_io() above has already called
     * esp_lcd_panel_disp_on_off(g_panel, true), so the controller is in a
     * known, configured state rather than whatever it powered on with.
     * There is still no frame in the panel's own RAM yet -- the first
     * kf_display_present() call is what puts one there -- so this is "a
     * known blank/undefined pattern, in the right orientation and colour
     * space" rather than the worse "raw power-on noise" a light-first
     * ordering would show.
     *
     * THIS IS THE CALL. Before this task, grep found no caller of
     * kf_display_set_backlight() anywhere in the repository -- the function
     * was declared, defined in all three HAL backends, and never invoked.
     * On the ILI9341 that was invisible, because that module's LED pin is
     * soldered straight to 3V3 and lights regardless of what software does.
     * The Waveshare ST7789 has a real BL pin with nothing else driving it:
     * without this call, a build against that profile boots, initialises
     * the panel correctly, and shows a black screen with a perfectly
     * healthy log -- the exact failure ADR 0039 exists to prevent. Skipped
     * when !own_backlight_pin (only the ILI9341 + KF_DBG_BRIDGE_ENABLE=1
     * combination), since kf_display_set_backlight() would have nothing to
     * turn on in that configuration anyway -- see that function's own
     * comment for what it does instead. */
    if (own_backlight_pin) {
        kf_display_set_backlight(255);
    }

    KF_LOGI(TAG, "%s up: %dx%d, %lu Hz SPI, RGB565, %s framebuffer, backlight %s",
            kPanel.name, KF_DISPLAY_WIDTH, KF_DISPLAY_HEIGHT,
            static_cast<unsigned long>(KF_DISPLAY_SPI_HZ),
            kPanel.big_endian_fb ? "byte-swapped" : "native-endian",
            own_backlight_pin ? "on" : "not owned by this build");
    return KF_OK;
}

const kf_display_caps *kf_display_get_caps(void) { return &g_caps; }

namespace {

#if KF_DBG_BRIDGE_ENABLE

/* ============================================================================
 * Beam-racing (KFDBG VSYNC): wait for the panel's own scan-out to clear a
 * rectangle before writing it, instead of racing it blind. See
 * kf_esp_display_vsync.h for the KFDBG-facing control surface (on/off, the
 * stats KFDBG STATE reports) and push_rect() below for where this actually
 * gets used.
 *
 * WHY THIS IS SAFE TO DO ON THE WRITE CLOCK, WHEN THE SCANLINE DIAGNOSTIC
 * WAS NOT.
 *
 * KFDBG SCANLINE rebuilds the whole panel at a slow read clock (2MHz by
 * default) because the ILI9341's read cycle is only rated to ~150ns
 * (~6MHz), and the first attempt at reading Get_scanline at the normal
 * 40MHz write clock came back looking like noise: 5 distinct values, all
 * sitting on byte boundaries (0x00/0xFF/0x80-ish jumps), values.min_value=0/
 * max=511 -- the signature of sampling MISO faster than the panel can drive
 * it, not of a dead register. Rebuilding the panel per frame to get a clean
 * read is exactly what this feature cannot afford: a full rebuild re-sends
 * the whole init table and visibly glitches the screen (see
 * kf_esp_display_diag_begin_probe()'s own comment), nowhere close to a
 * per-rectangle operation.
 *
 * The measurement task this feature was built from re-examined that same
 * 40MHz data under the ALT framing (no dummy byte -- raw[0]/raw[1] instead
 * of raw[1]/raw[2], the framing a clean 1/2/4MHz sweep has since confirmed
 * is the correct one for this panel, all three clocks agreeing: a
 * monotonic 0..161 counter that wraps, 56 increases against 1 decrease per
 * 64 samples). The ONE raw sample preserved from that original 40MHz run
 * (first_raw_hex "006300" -- see kf_dbg_bridge.cpp's ScanlineSample comment)
 * decodes, under alt framing, to 0x063 = 99: comfortably inside the 0..161
 * range the slow-clock sweep established, not a byte-boundary artefact.
 * Under the OLD (datasheet, 1-dummy-byte) framing that same run's distinct
 * values were all built from raw[2] sitting at 0x00, 0x80 or 0xFF -- three
 * values, no in-between -- while raw[1] in the one fully-preserved sample
 * (0x63) was not one of those degenerate values at all. That is consistent
 * with the LAST byte of a 3-byte transaction being where 40MHz-vs-6MHz
 * corruption concentrates (each bit shifted out depends on the panel having
 * caught up from the one before it, so error compounds over the
 * transaction) and the first two bytes being comparatively clean -- which
 * is exactly the two bytes alt framing uses, and exactly why this read asks
 * for 2 bytes, not 3: it never asks the panel for the byte the evidence
 * says is the unreliable one.
 *
 * That is one raw sample, not sixty-four, and there is no hardware on hand
 * this session to gather more -- said plainly rather than papered over.
 * Given the evidence available, reading at 40MHz is a defensible bet, not a
 * proven fact, which is exactly why this whole feature sits behind a
 * runtime flag (default ON) rather than being unconditionally wired in:
 * KFDBG VSYNC 0 / `kf_debug.py vsync off`, and the rects_waited/avg_wait_us
 * fields in KFDBG STATE, are how a human with the real board turns this
 * from a bet into a measurement.
 *
 * WHY NO PANEL REBUILD IS NEEDED HERE, UNLIKE THE DIAGNOSTIC.
 *
 * This reads through g_io at whatever clock it is CURRENTLY built for.
 * push_rect() only ever runs from kf_app_frame(), called after
 * kf_dbg_bridge_frame() returns each iteration (see app_main.cpp's loop) --
 * and kf_dbg_bridge_frame() is the only thing that can leave g_io at a
 * clock other than KF_DISPLAY_SPI_HZ (KFDBG SCANLINE's probe), and it
 * always restores KF_DISPLAY_SPI_HZ, synchronously, before returning (see
 * kf_esp_display_diag_end_probe()). So g_io is guaranteed to be at the
 * normal write clock every time push_rect() runs -- there is no rebuild to
 * do, and doing one here would cost far more than the read it is trying to
 * save. */

/* ASSUMPTION, not a measured fact -- said once, here, in the one place this
 * mapping is derived, per the task that added this feature. The 1/2/4MHz
 * sweep found the scan counter monotonic over 0..161 (162 distinct values)
 * before it wraps. The panel is 320 physical scanlines; 162 * 2 = 324, close
 * enough to 320 that "one count is two scanlines" is the natural read, but
 * nothing here has confirmed it against a known scan position (e.g. by
 * writing a specific row and reading back the count that should correspond
 * to it -- not possible without hardware). If a future measurement shows a
 * different ratio, or that the mapping is not linear, this is the one
 * constant and the one function to change; nothing else in this file
 * encodes the 2 independently. */
constexpr int kVsyncRowsPerCount = 2;

/* Bounds how long push_rect() will ever block waiting for the scan to clear
 * a rectangle -- roughly one frame period at 30fps (33ms) would still be
 * too generous against a 40Hz-ish frame budget; ~12ms is closer to one full
 * scan lap (measured ~10ms) plus slack, long enough to cover a real wait
 * and short enough that a stuck or wildly wrong reading degrades to "one
 * slow frame", never a hang. On timeout, push_rect() writes anyway -- a
 * torn frame beats a stalled device, every time. */
constexpr uint32_t kVsyncMaxWaitUs = 12000;

/* FALSE, and this is a measured result rather than caution.
 *
 * It shipped defaulting true on the strength of one re-decoded raw sample
 * suggesting 40MHz scanline reads were usable. Real hardware disagreed:
 * with the wait enabled, the flicker on changing content was unchanged.
 * The bet lost, and the flag existing is what made that a one-command
 * answer instead of an argument.
 *
 * Exactly which link failed is not established -- 40MHz reads may be
 * unreliable in a way the slow sweep did not expose, or the count-to-row
 * mapping in kf_vsync_count_to_scan_row() may be wrong, or the wait may be
 * landing on the wrong side of the scan. Nobody chased it further, because
 * the ceiling on this whole approach was modest even if it worked: a read
 * costs ~50us against a ~64us scan step, so the granularity was never going
 * to be better than a couple of scan rows.
 *
 * Kept rather than deleted. It is a working implementation of a technique
 * that IS correct in principle, the measurement path (KFDBG SCANLINE) is
 * genuinely useful on any future panel, and a panel that answers reads more
 * cleanly -- or exposes TE, at which point the wait becomes an interrupt --
 * turns this back on with one command. Deleting it would throw away the
 * expensive half of the work and keep none of it.
 *
 * See ADR 0032 for what this cost, what it established, and what it means
 * for choosing a panel for the real board. */
bool g_vsync_enabled = false;

/* Per-second stats for KFDBG STATE -- see kf_esp_display_vsync.h's own
 * comment on kf_esp_display_vsync_get_stats() for the exact fields and why
 * "most recently completed window" rather than "the last wall-clock
 * second". Single-thread, no lock, same reasoning as every other piece of
 * state in this file and in kf_dbg_bridge.cpp: only ever touched from
 * push_rect(), on the main frame-loop thread. */
uint64_t g_vsync_window_start_us = 0;
uint32_t g_vsync_cur_written = 0;
uint32_t g_vsync_cur_waited = 0;
uint64_t g_vsync_cur_wait_us_sum = 0;
uint32_t g_vsync_last_written = 0;
uint32_t g_vsync_last_waited = 0;
uint32_t g_vsync_last_avg_wait_us = 0;

/* Maps a raw Get_scanline count to an approximate scan row -- see
 * kVsyncRowsPerCount's own comment for why this is an assumption. The only
 * place this feature turns "count" into "row"; every comparison against a
 * rectangle's y0/y1 goes through this. */
inline int kf_vsync_count_to_scan_row(uint16_t count) {
    return static_cast<int>(count) * kVsyncRowsPerCount;
}

/* One raw Get_scanline read at whatever clock g_io is currently built for
 * (the normal write clock, always, by the time push_rect() runs -- see the
 * header comment above). 2 bytes, not the diagnostic's 3: alt framing
 * (raw[0]/raw[1], no dummy byte -- see kVsyncRowsPerCount's sibling
 * comment above and kf_dbg_bridge.cpp's SCANLINE handler, which now treats
 * this same framing as primary) never looks at a third byte, and not
 * asking for it is one less byte of transfer time and one less byte the
 * panel has to get right under this feature's own read-speed bet.
 *
 * Returns false (out_row untouched) if the display isn't up or the SPI
 * transaction itself failed -- treated by every caller as "couldn't tell,
 * write now" rather than a reason to block, the same fail-open posture
 * kf_esp_display_diag_read_scanline() takes for the diagnostic. */
bool kf_vsync_read_scan_row(int *out_row) {
    if (g_io == nullptr) {
        return false;
    }
    uint8_t raw[2];
    const esp_err_t err = esp_lcd_panel_io_rx_param(
        g_io, KF_ESP_DISPLAY_DIAG_CMD_GET_SCANLINE, raw, sizeof raw);
    if (err != ESP_OK) {
        return false;
    }
    const uint16_t count = static_cast<uint16_t>(
        ((static_cast<uint16_t>(raw[0]) << 8) | raw[1]) & 0x03FFu);
    *out_row = kf_vsync_count_to_scan_row(count);
    return true;
}

/* Rolls the one-second stats window if needed, then records one push_rect()
 * call's outcome. Called exactly once per push_rect() invocation,
 * regardless of whether vsync is enabled -- rects_written is therefore a
 * write-workload baseline that holds steady whether or not the wait itself
 * is switched on, which is what lets `kf_debug.py vsync on` / `off` be
 * compared against each other on KFDBG STATE's fps/frame_us instead of two
 * differently-shaped workloads.
 *
 * Deliberately does not decay g_vsync_last_* back to zero on its own after
 * a second of silence -- there is no background task ticking this on a
 * clock of its own, only push_rect() calls, and a genuinely still pet (the
 * entire point of dirty-rect present: zero bytes when nothing moved) can
 * leave push_rect() uncalled for seconds at a time. KFDBG STATE keeps
 * reporting the last window that actually had writes in it, which is a
 * more useful "is this working" answer than a misleading 0/0/0 the moment
 * the screen holds still. */
void kf_vsync_note_rect_written(bool waited, uint32_t wait_us) {
    const uint64_t now_us = kf_time_mono_us();
    if (g_vsync_window_start_us == 0) {
        g_vsync_window_start_us = now_us;
    } else if (now_us - g_vsync_window_start_us >= 1000000ull) {
        g_vsync_last_written = g_vsync_cur_written;
        g_vsync_last_waited = g_vsync_cur_waited;
        g_vsync_last_avg_wait_us = (g_vsync_cur_waited > 0)
            ? static_cast<uint32_t>(g_vsync_cur_wait_us_sum / g_vsync_cur_waited)
            : 0u;
        g_vsync_cur_written = 0;
        g_vsync_cur_waited = 0;
        g_vsync_cur_wait_us_sum = 0;
        g_vsync_window_start_us = now_us;
    }

    g_vsync_cur_written++;
    if (waited) {
        g_vsync_cur_waited++;
        g_vsync_cur_wait_us_sum += wait_us;
    }
}

/* The one call site push_rect() below makes. Disabled: records the write
 * and returns immediately, no read at all -- byte-for-byte the behaviour
 * this codebase had before this feature existed. Enabled: one read; if the
 * scan has already passed y1 (the common case -- see push_rect()'s own
 * comment), writes immediately with no wait. Otherwise polls (bounded by
 * kVsyncMaxWaitUs, see that constant's comment) until either the scan
 * passes y1, a read fails, or the deadline is reached -- any of which ends
 * the wait and lets the caller write. taskYIELD() between polls is a
 * FreeRTOS courtesy, not a correctness requirement: this runs on the main
 * frame-loop thread at ESP_TASK_MAIN_PRIO (1), the SAME priority
 * kf_dbg_bridge.cpp's own rx/tx tasks run at (kTaskPriority,
 * tskIDLE_PRIORITY + 1 -- see that file's own comment on why), so a bare
 * busy-loop here for up to 12ms would starve them of any chance to run
 * until FreeRTOS's own tick-based time-slicing intervened; yielding each
 * iteration lets them in immediately instead of waiting on that. */
void kf_vsync_prepare_write(int y0, int y1) {
    if (!g_vsync_enabled) {
        kf_vsync_note_rect_written(false, 0);
        return;
    }

    const uint64_t t_start = kf_time_mono_us();
    int row = 0;
    if (!kf_vsync_read_scan_row(&row) || row >= y1) {
        kf_vsync_note_rect_written(false, 0);
        return;
    }

    const uint64_t deadline_us = t_start + kVsyncMaxWaitUs;
    for (;;) {
        if (kf_time_mono_us() >= deadline_us) {
            break; /* bounded wait exceeded -- write anyway */
        }
        taskYIELD();
        if (!kf_vsync_read_scan_row(&row)) {
            break; /* read failed mid-wait -- couldn't tell, write now */
        }
        if (row >= y1) {
            break; /* scan has passed -- safe to write */
        }
    }

    const uint64_t elapsed_us = kf_time_mono_us() - t_start;
    kf_vsync_note_rect_written(true, static_cast<uint32_t>(elapsed_us));
}

#endif /* KF_DBG_BRIDGE_ENABLE */

/* Push one rectangle, staging it through the alternating strip buffers.
 *
 * Everything goes through a staging copy, including panels that need no byte
 * swap, for two reasons. A rectangle narrower than the screen is not
 * contiguous in the framebuffer, so its rows have to be packed somewhere
 * regardless. And handing esp_lcd a pointer into the live framebuffer is the
 * same DMA race this file already fixed once: LVGL writes into that memory
 * between frames, with no regard for whether a transfer is still reading it.
 *
 * Now that only changed pixels are sent, the copy is proportional to what
 * actually moved rather than to the screen, which is what makes paying for it
 * unconditionally the cheaper trade.
 *
 * One more thing happens before any of that, when KF_DBG_BRIDGE_ENABLE has
 * the MISO pin to do it with: kf_vsync_prepare_write() below, once per call
 * to this function (not once per chunk in the loop further down -- a tall
 * rectangle spanning several kSwapStripRows chunks gets exactly one scan
 * check for the whole rectangle, against its full y0..y1 span, not one per
 * chunk). See that function's own comment, and the ~40-line comment above
 * it, for what it checks and the evidence behind reading at 40MHz at all. */
kf_result push_rect(const kf_color *framebuffer, int x0, int y0, int x1, int y1) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > KF_DISPLAY_WIDTH) x1 = KF_DISPLAY_WIDTH;
    if (y1 > KF_DISPLAY_HEIGHT) y1 = KF_DISPLAY_HEIGHT;

    const int w = x1 - x0;
    if (w <= 0 || y1 - y0 <= 0) {
        return KF_OK;
    }

#if KF_DBG_BRIDGE_ENABLE
    kf_vsync_prepare_write(y0, y1);
#endif

    constexpr int kCapacityPixels = KF_DISPLAY_WIDTH * kSwapStripRows;
    int rows_per_chunk = kCapacityPixels / w;
    if (rows_per_chunk < 1) {
        rows_per_chunk = 1;
    }

    for (int y = y0; y < y1; y += rows_per_chunk) {
        int rows = rows_per_chunk;
        if (y + rows > y1) {
            rows = y1 - y;
        }

        /* Alternate before writing, never after -- see kSwapBufferCount. */
        g_swap_index = (g_swap_index + 1) % kSwapBufferCount;
        uint16_t *strip = g_swap_strip[g_swap_index];

        for (int row = 0; row < rows; ++row) {
            const kf_color *src =
                framebuffer + static_cast<size_t>(y + row) * KF_DISPLAY_WIDTH + x0;
            uint16_t *dst = strip + static_cast<size_t>(row) * w;
            if (kPanel.big_endian_fb) {
                for (int i = 0; i < w; ++i) {
                    dst[i] = __builtin_bswap16(src[i]);
                }
            } else {
                memcpy(dst, src, static_cast<size_t>(w) * sizeof(uint16_t));
            }
        }

        const esp_err_t err =
            esp_lcd_panel_draw_bitmap(g_panel, x0, y, x1, y + rows, strip);
        if (err != ESP_OK) {
            KF_LOGE(TAG, "draw_bitmap(%d,%d,%d,%d) failed: %d", x0, y, x1,
                    y + rows, err);
            return KF_ERR_IO;
        }
    }
    return KF_OK;
}

/* The dirty list describes what changed since the previous present. Before
 * the first one there is no previous, so the panel's memory holds whatever it
 * powered up with and the whole screen has to go out once. */
bool g_first_present = true;

} // namespace

kf_result kf_display_present(const kf_color *framebuffer,
                              const kf_rect *dirty_rects, int dirty_rect_count) {
    if (g_panel == nullptr) {
        return KF_ERR_UNAVAILABLE;
    }

    if (g_first_present) {
        g_first_present = false;
        return push_rect(framebuffer, 0, 0, KF_DISPLAY_WIDTH, KF_DISPLAY_HEIGHT);
    }

    /* Nothing changed: send nothing.
     *
     * This is the line that stops the flicker. Re-sending an identical frame
     * is not a harmless waste -- the panel is scanning its memory out to the
     * glass continuously, on its own clock, with no tearing-effect signal
     * wired to tell us when it is safe to write. Rewriting all 153,600 bytes
     * underneath that scan 30 times a second means the scan permanently sees
     * a moving boundary between the old contents and the identical new ones,
     * which reads to the eye as a pulsing band travelling down the screen.
     *
     * It is also most of the frame budget: a full frame is ~31ms of wire time
     * at the measured 40MHz (KF_DISPLAY_SPI_HZ), against a 33ms budget. A pet
     * standing still should cost nothing at all, and now does. */
    if (dirty_rects == nullptr || dirty_rect_count <= 0) {
        return KF_OK;
    }

    for (int i = 0; i < dirty_rect_count; ++i) {
        const kf_rect &r = dirty_rects[i];
        const kf_result res =
            push_rect(framebuffer, r.x0, r.y0, r.x1, r.y1);
        if (res != KF_OK) {
            return res;
        }
    }
    return KF_OK;
}

kf_result kf_display_set_backlight(uint8_t level) {
    /* On/off only -- see this file's header comment.
     *
     * kf_display_init() is this function's only caller today (right at the
     * end of bring-up, once the panel is initialised -- see that function's
     * "THIS IS THE CALL" comment for why calling it there, and calling it
     * at all, is the point of this whole task). It gates the call on
     * own_backlight_pin, so this body does not need to re-derive that here
     * -- but a future second caller (a settings screen, a sleep/wake path)
     * would reach this same gpio_set_level() unconditionally, so the
     * degraded case is worth documenting on the function itself, not just
     * at its one call site.
     *
     * A genuine no-op on the one profile/flag combination where this GPIO
     * was claimed for something else: ILI9341 + KF_DBG_BRIDGE_ENABLE=1 (see
     * kf_panel_profile.h's has_read_line and kf_display_init()'s MISO
     * comment). In that configuration the pin is never put into
     * GPIO_MODE_OUTPUT, so this call still compiles and still returns
     * KF_OK, but the level it writes has no electrical effect -- ESP-IDF
     * does not drive a pin's output register onto the pad unless the pin's
     * mode says to. Not a bug in that specific case either: that module's
     * LED pin is soldered straight to 3V3 regardless (kf_esp_pins.h), so
     * there is nothing this call could have done there that it is now
     * failing to do. Every other profile/flag combination -- which as of
     * ADR 0039 includes the ST7789 unconditionally -- owns this pin for
     * real and this call has a real, visible effect. */
    gpio_set_level(KF_ESP_PIN_LCD_BL, level > 0 ? 1 : 0);
    return KF_OK;
}

void kf_display_shutdown(void) {
    for (int i = 0; i < kSwapBufferCount; ++i) {
        if (g_swap_strip[i] != nullptr) {
            heap_caps_free(g_swap_strip[i]);
            g_swap_strip[i] = nullptr;
        }
    }
    if (g_panel != nullptr) {
        esp_lcd_panel_del(g_panel);
        g_panel = nullptr;
    }
    if (g_io != nullptr) {
        esp_lcd_panel_io_del(g_io);
        g_io = nullptr;
    }
    if (g_spi_bus_initialized) {
        spi_bus_free(kSpiHost);
        g_spi_bus_initialized = false;
    }
}

#if KF_DBG_BRIDGE_ENABLE

/* KFDBG SCANLINE's one raw primitive -- see kf_esp_display_diag.h for the
 * full contract and kf_dbg_bridge.cpp's handle_scanline() for the sampling
 * loop and statistics built on top of it. Reads through g_io at whatever
 * clock it is CURRENTLY configured for -- kf_esp_display_diag_begin_probe()
 * below is what changes that clock for the duration of a SCANLINE run, so
 * this function itself never needs to know or care which clock is active. */
bool kf_esp_display_diag_read_scanline(uint8_t *out_bytes, size_t byte_count) {
    if (g_io == nullptr) {
        return false;
    }
    const esp_err_t err = esp_lcd_panel_io_rx_param(
        g_io, KF_ESP_DISPLAY_DIAG_CMD_GET_SCANLINE, out_bytes, byte_count);
    return err == ESP_OK;
}

/* Tears down the write panel IO/panel and rebuilds them at read_hz, so
 * kf_esp_display_diag_read_scanline() above reads at a clock the ILI9341's
 * ~150ns read cycle (about 6MHz max, per the datasheet) can actually keep
 * up with, instead of the 40MHz write clock every draw_bitmap() call uses --
 * 6-20x too fast, and the concrete, already-measured explanation for why the
 * first SCANLINE run at the write clock came back looking like noise (runs
 * of all-zero/all-one bytes jumping by exactly 127: the classic signature of
 * sampling MISO too fast, not of a real counter).
 *
 * A second, slower esp_lcd_panel_io_handle_t sharing this display's CS pin
 * was the plan going in, and was rejected for a concrete, verified reason,
 * not time pressure: esp_lcd hangs every transaction off ONE
 * spi_device_handle_t per IO handle, with the clock fixed at creation --
 * there is no per-transaction clock override anywhere in esp_lcd_panel_io_
 * spi.c. And ESP-IDF's spi_bus_add_device() reassigns a shared CS pin's
 * GPIO-matrix routing to whichever device was added last
 * (spicommon_cs_initialize() in spi_common.c): adding a second device on
 * GPIO10 (this display's CS) steals the write device's own CS0 signal, and
 * removing the temporary device again leaves NEITHER device's CS connected
 * (spicommon_cs_free_io() calls gpio_output_disable() on the pin).
 * Confirmed by reading spi_common.c, not assumed.
 *
 * The way through instead of around: there is only ever one IO handle,
 * g_io, and this function tears it down and rebuilds it -- via
 * rebuild_panel_io(), the SAME function kf_display_init() itself uses for
 * the normal write clock, so the two paths cannot drift apart -- at
 * read_hz instead. kf_esp_display_diag_end_probe() below rebuilds it back
 * at KF_DISPLAY_SPI_HZ once the probe is done. Both directions re-send the
 * panel's full init table, so the controller is never left in a state
 * neither clock configured it for.
 *
 * Runs on the frame-loop thread, inside kf_dbg_bridge_frame() -- see
 * kf_dbg_bridge.h's own "WHY A BACKGROUND TASK" comment for that thread
 * model: kf_dbg_bridge_frame() runs, and returns, before kf_app_frame()
 * ever touches g_panel again for the same iteration, and nothing else in
 * this codebase calls into g_io/g_panel from any other thread. That is what
 * makes tearing them down here, synchronously, safe -- confirmed by reading
 * that file's comment, not assumed. The screen WILL visibly glitch for the
 * duration of a SCANLINE run: the panel is reset and re-initialised twice,
 * once at read_hz and once back at KF_DISPLAY_SPI_HZ. That is expected, not
 * a bug -- kf_dbg_bridge.cpp's handle_scanline() says as much in its own
 * reply text too, so it is not mistaken for a new one on the wire.
 *
 * Returns false if the rebuild at read_hz failed -- see rebuild_panel_io()'s
 * own comment for what that leaves g_io/g_panel as. The caller (handle_
 * scanline()) does not treat that as a reason to skip the sampling loop:
 * kf_esp_display_diag_read_scanline() above already returns false cleanly
 * whenever g_io is null, so a failed begin_probe() just means every sample
 * fails, which is itself a reportable result rather than a crash. */
bool kf_esp_display_diag_begin_probe(uint32_t read_hz) {
    return rebuild_panel_io(read_hz);
}

/* Restores normal operation after a SCANLINE probe: rebuilds g_io/g_panel
 * at KF_DISPLAY_SPI_HZ, the clock kf_display_present() and every other
 * frame's draw_bitmap() calls expect. Called unconditionally by handle_
 * scanline(), even when kf_esp_display_diag_begin_probe() itself failed, so
 * a probe that could not even get INTO its slow clock still gets a chance
 * to come back OUT to the normal one, rather than being left however
 * begin_probe() left it.
 *
 * If this rebuild ALSO fails -- both attempts failing on the same physical
 * bus, likely for the same underlying reason (a bad wire), is a real
 * possibility worth naming here rather than a purely theoretical one --
 * g_io and g_panel are left null. That is the safe failure mode this whole
 * design is built around: kf_display_present() already checks
 * `g_panel == nullptr` at its own top and returns KF_ERR_UNAVAILABLE rather
 * than dereferencing it, so the degraded result is "no more display updates
 * until the next boot," logged loudly here so it reads as a clear failure
 * rather than a silent hang -- never a crash. */
void kf_esp_display_diag_end_probe(void) {
    if (!rebuild_panel_io(KF_DISPLAY_SPI_HZ)) {
        KF_LOGE(TAG,
                "SCANLINE: could not restore the display panel at %lu Hz after "
                "the probe -- g_panel is now null, so every kf_display_present() "
                "call will return KF_ERR_UNAVAILABLE (no crash, no hang, just no "
                "more screen updates) until the device is reset",
                static_cast<unsigned long>(KF_DISPLAY_SPI_HZ));
    }
}

/* Mirrors kPanel.has_read_line (kf_panel_profile.h) out to kf_dbg_bridge.cpp,
 * which has no other way to reach it -- kPanel itself is this file's own
 * anonymous-namespace reference, not part of any header. SCANLINE and VSYNC
 * both call this first and refuse outright when it is false, rather than
 * reading a MISO pin that was never reserved on the bus (see
 * kf_display_init()'s reserve_miso_for_read_line) and reporting whatever a
 * floating input happens to return -- ADR 0039's whole point about
 * SCANLINE. */
bool kf_esp_display_has_read_line(void) { return kPanel.has_read_line; }

/* The active profile's display name, e.g. "ST7789 (Waveshare 2in)", for the
 * refusal text above -- so a human reading a KFDBG reply knows which panel
 * without cross-referencing a build flag or a KF_PANEL value. */
const char *kf_esp_display_panel_name(void) { return kPanel.name; }

/* -----------------------------------------------------------------------
 * KFDBG VSYNC / KFDBG STATE's control surface over the beam-racing feature
 * -- see kf_esp_display_vsync.h for the contract each of these implements,
 * and the big comment above push_rect()'s vsync helpers (top of this file)
 * for the feature itself. Thin wrappers around the file-scope state those
 * helpers already own; kept here, at the bottom, alongside the SCANLINE
 * diagnostic's own equivalent wrappers, rather than beside the helpers
 * themselves, so every extern "C" entry point this file exposes lives in
 * one place. ----------------------------------------------------------- */

void kf_esp_display_vsync_set_enabled(bool enabled) { g_vsync_enabled = enabled; }

bool kf_esp_display_vsync_enabled(void) { return g_vsync_enabled; }

void kf_esp_display_vsync_get_stats(uint32_t *rects_written, uint32_t *rects_waited,
                                     uint32_t *avg_wait_us) {
    *rects_written = g_vsync_last_written;
    *rects_waited = g_vsync_last_waited;
    *avg_wait_us = g_vsync_last_avg_wait_us;
}

#endif /* KF_DBG_BRIDGE_ENABLE */
