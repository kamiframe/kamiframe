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

namespace {

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
 * unconditionally the cheaper trade. */
kf_result push_rect(const kf_color *framebuffer, int x0, int y0, int x1, int y1) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > KF_DISPLAY_WIDTH) x1 = KF_DISPLAY_WIDTH;
    if (y1 > KF_DISPLAY_HEIGHT) y1 = KF_DISPLAY_HEIGHT;

    const int w = x1 - x0;
    if (w <= 0 || y1 - y0 <= 0) {
        return KF_OK;
    }

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
