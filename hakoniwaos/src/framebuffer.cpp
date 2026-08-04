/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 */

#include "kf/framebuffer.h"

#include "kf/arena.h"
#include "kf/blit.h"
#include "kf/hal/log.h"

#include <cstdint>
#include <cstring>

#include "kf/poison.h"

namespace {

constexpr const char *TAG = "fb";

kf_color *g_pixels = nullptr;
kf_rect g_dirty = {0, 0, 0, 0};

constexpr kf_rect kFullScreen = {0, 0, static_cast<int16_t>(KF_DISPLAY_WIDTH),
                                 static_cast<int16_t>(KF_DISPLAY_HEIGHT)};

} // namespace

void kf_fb_init(void) {
    KF_ASSERT(g_pixels == nullptr, "kf_fb_init called twice");

    /* 32-byte alignment because the S3's DMA and cache both want it. Asking
     * for it on desktop too keeps the two builds honest. */
    g_pixels = static_cast<kf_color *>(
        kf_arena_alloc(KF_ARENA_FRAMEBUFFER, KF_FRAMEBUFFER_BYTES, 32));

    memset(g_pixels, 0, KF_FRAMEBUFFER_BYTES);
    kf_fb_mark_all_dirty();

    KF_LOGI(TAG, "framebuffer %dx%d RGB565, %d bytes", KF_DISPLAY_WIDTH,
            KF_DISPLAY_HEIGHT, KF_FRAMEBUFFER_BYTES);
}

kf_color *kf_fb_pixels(void) {
    KF_ASSERT(g_pixels != nullptr, "kf_fb_pixels before kf_fb_init");
    return g_pixels;
}

void kf_fb_mark_dirty(kf_rect r) {
    const kf_rect clipped = kf_rect_intersect(r, kFullScreen);
    if (kf_rect_is_empty(clipped)) {
        return;
    }
    g_dirty = kf_rect_union(g_dirty, clipped);
}

void kf_fb_mark_all_dirty(void) { g_dirty = kFullScreen; }

kf_rect kf_fb_dirty_rect(void) { return g_dirty; }

void kf_fb_clear_dirty(void) { g_dirty = kf_rect{0, 0, 0, 0}; }

size_t kf_fb_dirty_bytes(void) {
    return static_cast<size_t>(kf_rect_area(g_dirty)) * sizeof(kf_color);
}
