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
 * 1. supports_partial_update is false. dirty_rects/dirty_rect_count are
 *    accepted (the signature requires it) but ignored, and every present()
 *    pushes the full frame. Honouring dirty rects for real is a genuine
 *    future optimisation -- union the rects, one bitmap call per rect, wait
 *    once at the end -- not done here because a wrong partial-update
 *    implementation that skips pixels it shouldn't is a worse bug than an
 *    honest full-frame push.
 *
 * 2. Byte order is per-panel, and on a panel that needs swapping it costs a
 *    pass over the frame. See kf_display_present() for the reasoning and
 *    what it costs; kf_panel_profile.h explains why the panels differ.
 *
 * 3. Backlight is a plain GPIO on/off, not PWM. has_backlight is true and
 *    kf_display_set_backlight() treats any level > 0 as "on" -- there is no
 *    LEDC channel wired up here, so dimming is not implemented. Adding it
 *    later is a self-contained change to one function.
 */

#include "kf/hal/display.h"

#include "kf/budget.h"
#include "kf/hal/log.h"
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
    /* supports_partial_update: false -- see header comment above. */
    false,
    /* has_backlight: true -- on/off only, see header comment above. */
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

} // namespace

kf_result kf_display_init(void) {
    KF_LOGI(TAG, "panel profile: %s", kPanel.name);

    /* No MISO: these panels are write-only, and leaving it at -1 tells the
     * SPI driver not to reserve a pin. max_transfer_sz covers a full frame,
     * since the no-swap path issues exactly one draw_bitmap() per frame. */
    spi_bus_config_t bus_config{};
    bus_config.mosi_io_num = KF_ESP_PIN_LCD_MOSI;
    bus_config.miso_io_num = -1;
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

    esp_lcd_panel_io_spi_config_t io_config{};
    io_config.cs_gpio_num = KF_ESP_PIN_LCD_CS;
    io_config.dc_gpio_num = KF_ESP_PIN_LCD_DC;
    io_config.spi_mode = 0;
    io_config.pclk_hz = KF_DISPLAY_SPI_HZ;
    io_config.trans_queue_depth = 10;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    /* flags left at their default (all zero): dc_cmd_level = 0, dc_data_level
     * = 1, the standard "DC low = command, DC high = data" wiring these panels
     * use -- confirmed against esp_lcd_panel_io_spi.c rather than assumed. */

    err = esp_lcd_new_panel_io_spi(
        static_cast<esp_lcd_spi_bus_handle_t>(kSpiHost), &io_config, &g_io);
    if (err != ESP_OK) {
        KF_LOGE(TAG, "esp_lcd_new_panel_io_spi failed: %d", err);
        return KF_ERR_IO;
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
        KF_LOGE(TAG, "esp_lcd_new_panel_st7789 failed: %d", err);
        return KF_ERR_IO;
    }

    esp_lcd_panel_reset(g_panel);
    if (kPanel.use_builtin_init) {
        esp_lcd_panel_init(g_panel);
    }
    send_init_table(kPanel);
    esp_lcd_panel_invert_color(g_panel, kPanel.invert);
    if (kPanel.x_gap != 0 || kPanel.y_gap != 0) {
        esp_lcd_panel_set_gap(g_panel, kPanel.x_gap, kPanel.y_gap);
    }
    esp_lcd_panel_disp_on_off(g_panel, true);

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

    /* Backlight GPIO, plain push-pull output, defaults to off until the
     * caller explicitly asks for one. */
    gpio_config_t bl_config{};
    bl_config.pin_bit_mask = (1ULL << KF_ESP_PIN_LCD_BL);
    bl_config.mode = GPIO_MODE_OUTPUT;
    bl_config.pull_up_en = GPIO_PULLUP_DISABLE;
    bl_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    bl_config.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&bl_config);
    gpio_set_level(KF_ESP_PIN_LCD_BL, 0);

    KF_LOGI(TAG, "%s up: %dx%d, %lu Hz SPI, RGB565, %s framebuffer",
            kPanel.name, KF_DISPLAY_WIDTH, KF_DISPLAY_HEIGHT,
            static_cast<unsigned long>(KF_DISPLAY_SPI_HZ),
            kPanel.big_endian_fb ? "byte-swapped" : "native-endian");
    return KF_OK;
}

const kf_display_caps *kf_display_get_caps(void) { return &g_caps; }

kf_result kf_display_present(const kf_color *framebuffer,
                              const kf_rect *dirty_rects, int dirty_rect_count) {
    (void)dirty_rects;
    (void)dirty_rect_count;

    if (g_panel == nullptr) {
        return KF_ERR_UNAVAILABLE;
    }

    /* Half-open x_end/y_end, exactly kf_rect's own convention (kf/types.h)
     * -- draw_bitmap's x_end/y_end are documented the same way, so the full
     * frame is [0, WIDTH) x [0, HEIGHT) with no off-by-one adjustment. */
    if (!kPanel.big_endian_fb) {
        const esp_err_t err = esp_lcd_panel_draw_bitmap(
            g_panel, 0, 0, KF_DISPLAY_WIDTH, KF_DISPLAY_HEIGHT, framebuffer);
        if (err != ESP_OK) {
            KF_LOGE(TAG, "esp_lcd_panel_draw_bitmap failed: %d", err);
            return KF_ERR_IO;
        }
        return KF_OK;
    }

    /* ------------------------------------------------------------------
     * The byte-swapping path.
     *
     * kf_color is native-endian RGB565 by contract (kf/hal/display.h) and
     * that contract is worth keeping: the blitter, the font renderer, the
     * sprite data in flash and LVGL all produce and consume colours without
     * caring what panel is attached. So the swap happens here, at the one
     * place that knows the panel is big-endian, rather than leaking a
     * panel's quirk into every producer of a pixel.
     *
     * Strip at a time rather than one big staging frame: a second full
     * framebuffer would be another 150KB of internal RAM on a chip with
     * 512KB, to save a handful of draw_bitmap calls. The strip costs 19KB.
     *
     * Cost is one pass over 76,800 pixels. Against ~31ms of wire time at
     * 40MHz (KF_DISPLAY_SPI_HZ, measured) that is small, but it is not free,
     * and it is a genuine reason to prefer a panel that does not need it --
     * which is the ST7789 profile's advantage over the ILI9341 one.
     * ------------------------------------------------------------------ */
    for (int y = 0; y < KF_DISPLAY_HEIGHT; y += kSwapStripRows) {
        int rows = kSwapStripRows;
        if (y + rows > KF_DISPLAY_HEIGHT) {
            rows = KF_DISPLAY_HEIGHT - y;
        }

        /* Alternate before writing, never after -- see kSwapBufferCount's
         * comment. The buffer picked here must be one no in-flight transfer
         * is still reading. */
        g_swap_index = (g_swap_index + 1) % kSwapBufferCount;
        uint16_t *strip = g_swap_strip[g_swap_index];

        const kf_color *src = framebuffer + static_cast<size_t>(y) * KF_DISPLAY_WIDTH;
        const int count = rows * KF_DISPLAY_WIDTH;
        for (int i = 0; i < count; ++i) {
            strip[i] = __builtin_bswap16(src[i]);
        }

        const esp_err_t err = esp_lcd_panel_draw_bitmap(
            g_panel, 0, y, KF_DISPLAY_WIDTH, y + rows, strip);
        if (err != ESP_OK) {
            KF_LOGE(TAG, "esp_lcd_panel_draw_bitmap failed at row %d: %d", y, err);
            return KF_ERR_IO;
        }
    }
    return KF_OK;
}

kf_result kf_display_set_backlight(uint8_t level) {
    /* On/off only -- see this file's header comment. */
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
