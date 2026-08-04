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
    /* has_backlight */ false,
    /* Same device figure as the SDL backend, so the transfer estimate in the
     * budget report means the same thing in CI as it does on your desk. */
    KF_DISPLAY_SPI_HZ / 8u,
};

uint64_t g_checksum = 1469598103934665603ull; /* FNV-1a 64 offset basis */
uint64_t g_frames = 0;
uint64_t g_dirty_pixels_total = 0;

} // namespace

kf_result kf_display_init(void) {
    KF_LOGI(TAG, "headless %dx%d RGB565", KF_DISPLAY_WIDTH, KF_DISPLAY_HEIGHT);
    return KF_OK;
}

const kf_display_caps *kf_display_get_caps(void) { return &g_caps; }

kf_result kf_display_present(const kf_color *framebuffer, kf_rect dirty) {
    /* FNV-1a over the whole buffer. Cheap, order-sensitive, and good enough
     * to notice a one-pixel change. */
    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(framebuffer);
    for (size_t i = 0; i < KF_FRAMEBUFFER_BYTES; ++i) {
        g_checksum ^= bytes[i];
        g_checksum *= 1099511628211ull;
    }

    if (dirty.x1 > dirty.x0 && dirty.y1 > dirty.y0) {
        g_dirty_pixels_total += static_cast<uint64_t>(dirty.x1 - dirty.x0) *
                                static_cast<uint64_t>(dirty.y1 - dirty.y0);
    }
    g_frames++;
    return KF_OK;
}

kf_result kf_display_set_backlight(uint8_t level) {
    (void)level;
    return KF_ERR_UNAVAILABLE;
}

void kf_display_shutdown(void) {}

uint64_t kf_headless_checksum(void) { return g_checksum; }
uint64_t kf_headless_frames(void) { return g_frames; }
uint64_t kf_headless_dirty_pixels(void) { return g_dirty_pixels_total; }
