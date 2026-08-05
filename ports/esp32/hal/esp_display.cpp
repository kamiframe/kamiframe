/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * HAL backend: display, ESP32 implementation (ST7789 over SPI + DMA).
 *
 * Three decisions worth naming, matching sdl_display.cpp's own honesty
 * comment:
 *
 * 1. supports_partial_update is false. dirty_rects/dirty_rect_count are
 *    accepted (the signature requires it) but ignored, and every present()
 *    pushes the full KF_DISPLAY_WIDTH*KF_DISPLAY_HEIGHT frame with one
 *    esp_lcd_panel_draw_bitmap() call. Honouring dirty rects for real is a
 *    genuine future optimisation (union the rects, issue one bitmap call per
 *    rect, only wait once at the end) -- not done here because getting SPI
 *    + DMA compiling and linking against the real ST7789 driver at all is
 *    this slice's job (ADR 0020), and a wrong partial-update implementation
 *    that skips pixels it shouldn't is a worse bug than an honest full-frame
 *    push.
 *
 * 2. data_endian is LCD_RGB_DATA_ENDIAN_LITTLE, not because the wire format
 *    is little-endian -- ST7789 panels are natively big-endian (MSB first)
 *    over SPI -- but because esp_lcd_panel_st7789.c programs the panel's own
 *    RAMCTL register to accept little-endian input when told to (see that
 *    file's `ramctl_val_2 |= ST7789_DATA_LITTLE_ENDIAN_BIT` when this flag is
 *    set). That means kf_color's own contract (native-endian uint16_t
 *    RGB565, per kf/hal/display.h) can go straight to the panel with zero
 *    host-side byte swapping, which is both simpler and faster than
 *    swapping 76800 pixels in software every frame.
 *
 * 3. Backlight is a plain GPIO on/off, not PWM. has_backlight is true and
 *    kf_display_set_backlight() treats any level > 0 as "on" -- there is no
 *    LEDC (PWM) channel wired up here, so dimming is not implemented. This
 *    is a real, smaller gap than the wall-clock one in esp_time.cpp: adding
 *    LEDC brightness control later is a self-contained change to this one
 *    function, not an architectural one.
 */

#include "kf/hal/display.h"

#include "kf/budget.h"
#include "kf/hal/log.h"
#include "kf_esp_pins.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"

namespace {

constexpr const char *TAG = "display";

constexpr spi_host_device_t kSpiHost = SPI2_HOST;

esp_lcd_panel_io_handle_t g_io = nullptr;
esp_lcd_panel_handle_t g_panel = nullptr;
bool g_spi_bus_initialized = false;

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

} // namespace

kf_result kf_display_init(void) {
    /* No MISO: this panel is write-only, and leaving it at -1 tells the SPI
     * driver not to reserve a pin for it (matches kf_esp_pins.h's own
     * comment). max_transfer_sz covers one full frame in one DMA transfer,
     * since kf_display_present() below issues exactly one draw_bitmap()
     * call per frame. */
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
     * = 1, the standard "DC low = command, DC high = data" wiring this panel
     * uses -- confirmed against esp_lcd_panel_io_spi.c rather than assumed. */

    err = esp_lcd_new_panel_io_spi(
        static_cast<esp_lcd_spi_bus_handle_t>(kSpiHost), &io_config, &g_io);
    if (err != ESP_OK) {
        KF_LOGE(TAG, "esp_lcd_new_panel_io_spi failed: %d", err);
        return KF_ERR_IO;
    }

    esp_lcd_panel_dev_config_t panel_config{};
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.data_endian = LCD_RGB_DATA_ENDIAN_LITTLE;
    panel_config.bits_per_pixel = 16;
    panel_config.reset_gpio_num = KF_ESP_PIN_LCD_RST;
    panel_config.flags.reset_active_high = 0;

    err = esp_lcd_new_panel_st7789(g_io, &panel_config, &g_panel);
    if (err != ESP_OK) {
        KF_LOGE(TAG, "esp_lcd_new_panel_st7789 failed: %d", err);
        return KF_ERR_IO;
    }

    esp_lcd_panel_reset(g_panel);
    esp_lcd_panel_init(g_panel);
    esp_lcd_panel_invert_color(g_panel, false);
    esp_lcd_panel_disp_on_off(g_panel, true);

    /* Backlight GPIO, plain push-pull output, defaults to off until the
     * caller explicitly asks for one -- see this file's header comment on
     * why this is on/off rather than PWM. */
    gpio_config_t bl_config{};
    bl_config.pin_bit_mask = (1ULL << KF_ESP_PIN_LCD_BL);
    bl_config.mode = GPIO_MODE_OUTPUT;
    bl_config.pull_up_en = GPIO_PULLUP_DISABLE;
    bl_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    bl_config.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&bl_config);
    gpio_set_level(KF_ESP_PIN_LCD_BL, 0);

    KF_LOGI(TAG, "ST7789 up: %dx%d, %lu Hz SPI, RGB565", KF_DISPLAY_WIDTH,
            KF_DISPLAY_HEIGHT, static_cast<unsigned long>(KF_DISPLAY_SPI_HZ));
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
    const esp_err_t err = esp_lcd_panel_draw_bitmap(
        g_panel, 0, 0, KF_DISPLAY_WIDTH, KF_DISPLAY_HEIGHT, framebuffer);
    if (err != ESP_OK) {
        KF_LOGE(TAG, "esp_lcd_panel_draw_bitmap failed: %d", err);
        return KF_ERR_IO;
    }
    return KF_OK;
}

kf_result kf_display_set_backlight(uint8_t level) {
    /* On/off only -- see this file's header comment. */
    gpio_set_level(KF_ESP_PIN_LCD_BL, level > 0 ? 1 : 0);
    return KF_OK;
}

void kf_display_shutdown(void) {
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
