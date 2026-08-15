/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * HAL backend: display, headless.
 *
 * Draws nowhere. Keeps a checksum of every frame it is given, and remembers
 * the dirty rectangles it was told about.
 *
 * Two jobs:
 *
 *   1. CI can run the real firmware, with the real drawing code, and assert
 *      on the pixels. No window, no GPU, no display server.
 *
 *   2. It proves the backend swap works. Slotting the ESP32 backend in later
 *      uses this exact mechanism, and it is much better to find out now that
 *      the mechanism is sound than during hardware bring-up.
 */

#include "kf/hal/display.h"

#include "kf/budget.h"
#include "kf/hal/log.h"
#include "headless_probe.h"

#include <cstdint>

namespace {

constexpr const char *TAG = "display";

kf_display_caps g_caps = {
    KF_DISPLAY_WIDTH,
    KF_DISPLAY_HEIGHT,
    KF_PIXFMT_RGB565,
    /* supports_partial_update */ true,
    /* has_backlight: true. kf_display_set_backlight() below records the
     * level rather than refusing, so a caller gating a brightness control on
     * this cap behaves the same here as on the ST7789 and in the SDL
     * window. */
    true,
    /* Same device figure as the SDL backend, so the transfer estimate in the
     * budget report means the same thing in CI as it does on your desk. */
    KF_DISPLAY_SPI_HZ / 8u,
};

uint64_t g_checksum = 1469598103934665603ull; /* FNV-1a 64 offset basis */
uint64_t g_frames = 0;
uint64_t g_dirty_pixels_total = 0;

/* 255, not 0: the pre-existing behaviour was a backlight that was simply on,
 * and "on" meant full. A test that never touches brightness should read what
 * the hardware would have shown, not a value that implies someone set it. */
uint8_t g_backlight_level = 255u;

} // namespace

kf_result kf_display_init(void) {
    KF_LOGI(TAG, "headless %dx%d RGB565", KF_DISPLAY_WIDTH, KF_DISPLAY_HEIGHT);
    return KF_OK;
}

const kf_display_caps *kf_display_get_caps(void) { return &g_caps; }

kf_result kf_display_present(const kf_color *framebuffer,
                              const kf_rect *dirty_rects, int dirty_rect_count) {
    /* FNV-1a over the whole buffer. Cheap, order-sensitive, and good enough
     * to notice a one-pixel change. */
    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(framebuffer);
    for (size_t i = 0; i < KF_FRAMEBUFFER_BYTES; ++i) {
        g_checksum ^= bytes[i];
        g_checksum *= 1099511628211ull;
    }

    /* Sum of the actual rectangles, not a bounding box: this is what makes
     * headless_dirty_area mean the same thing kf_fb_dirty_bytes() does in
     * core, rather than re-inflating the exact number kf/framebuffer.h's
     * rectangle list exists to shrink. Rectangles never overlap (core's
     * guarantee), so a plain sum is exact, not an over-count. */
    for (int i = 0; i < dirty_rect_count; ++i) {
        const kf_rect r = dirty_rects[i];
        if (r.x1 > r.x0 && r.y1 > r.y0) {
            g_dirty_pixels_total += static_cast<uint64_t>(r.x1 - r.x0) *
                                    static_cast<uint64_t>(r.y1 - r.y0);
        }
    }
    g_frames++;
    return KF_OK;
}

kf_result kf_display_set_backlight(uint8_t level) {
    /* Records rather than refuses, so a test can assert what brightness
     * actually applied, not merely what it stored. There is nothing to
     * light here, but "nothing to light" and "the call did not happen" are
     * different failures and a test should be able to tell them apart.
     *
     * Deliberately does NOT touch the framebuffer or the checksum: dimming
     * is a property of the display, not of the frame Core rendered, so
     * every golden checksum sees identical bytes at any brightness. Same
     * split the real hardware has, and the same one sdl_display.cpp makes
     * by modulating the texture instead of the pixels. */
    g_backlight_level = level;
    return KF_OK;
}

void kf_display_shutdown(void) {}

uint8_t kf_headless_backlight_level(void) { return g_backlight_level; }

uint64_t kf_headless_checksum(void) { return g_checksum; }
uint64_t kf_headless_frames(void) { return g_frames; }
uint64_t kf_headless_dirty_pixels(void) { return g_dirty_pixels_total; }
