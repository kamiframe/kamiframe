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

kf_rect g_dirty[KF_MAX_DIRTY_RECTS];
int g_dirty_count = 0;

/* True once a frame has fallen back to a single box, either because the
 * rectangle list filled up or because kf_fb_mark_all_dirty() made tracking
 * sub-rectangles pointless. Kept separate from g_dirty_count == 1 so a frame
 * that happens to draw exactly one thing does not skip the (cheap) merge
 * scan on its second draw call. */
bool g_collapsed = false;

constexpr kf_rect kFullScreen = {0, 0, static_cast<int16_t>(KF_DISPLAY_WIDTH),
                                 static_cast<int16_t>(KF_DISPLAY_HEIGHT)};

/* True if `a` expanded by one pixel in every direction would overlap `b`.
 * The expansion is what catches two rectangles that share an edge (like
 * neighbouring text cells in kf/font.h) but do not technically overlap: a
 * strict intersection test would leave every character in a HUD string as
 * its own rectangle, filling KF_MAX_DIRTY_RECTS before the first line
 * finishes drawing. int32_t intermediates: the +-1 pixel pad cannot
 * meaningfully overflow int16_t coordinates on a 240x320 panel, but there is
 * no reason to rely on that when the wider type is free. */
bool touches_or_overlaps(kf_rect a, kf_rect b) {
    const int32_t ax0 = static_cast<int32_t>(a.x0) - 1;
    const int32_t ay0 = static_cast<int32_t>(a.y0) - 1;
    const int32_t ax1 = static_cast<int32_t>(a.x1) + 1;
    const int32_t ay1 = static_cast<int32_t>(a.y1) + 1;
    return !(b.x1 <= ax0 || b.x0 >= ax1 || b.y1 <= ay0 || b.y0 >= ay1);
}

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
    const kf_rect c = kf_rect_intersect(r, kFullScreen);
    if (kf_rect_is_empty(c)) {
        return;
    }

    if (g_collapsed) {
        g_dirty[0] = kf_rect_union(g_dirty[0], c);
        return;
    }

    for (int i = 0; i < g_dirty_count; ++i) {
        if (touches_or_overlaps(g_dirty[i], c)) {
            g_dirty[i] = kf_rect_union(g_dirty[i], c);
            return;
        }
    }

    if (g_dirty_count < KF_MAX_DIRTY_RECTS) {
        g_dirty[g_dirty_count] = c;
        g_dirty_count++;
        return;
    }

    /* Out of slots. Fall back to one box covering everything so far plus
     * this new rectangle, and stay in that mode for the rest of the frame --
     * the same guarantee the original single-box design always gave: never
     * wrong, only ever over-sends. This is expected and fine for
     * KF_DEMO_FULLSCREEN, which redraws the whole screen every frame anyway
     * and has no sub-rectangle structure worth preserving. */
    kf_rect all = c;
    for (int i = 0; i < g_dirty_count; ++i) {
        all = kf_rect_union(all, g_dirty[i]);
    }
    g_dirty[0] = all;
    g_dirty_count = 1;
    g_collapsed = true;
}

void kf_fb_mark_all_dirty(void) {
    g_dirty[0] = kFullScreen;
    g_dirty_count = 1;
    /* No benefit tracking sub-rectangles when the whole screen is already
     * dirty; go straight to collapsed so the next mark_dirty call this frame
     * takes the cheap union path instead of scanning a one-entry list. */
    g_collapsed = true;
}

kf_dirty_rects kf_fb_dirty_rects(void) {
    kf_dirty_rects out{};
    out.count = g_dirty_count;
    for (int i = 0; i < g_dirty_count; ++i) {
        out.rects[i] = g_dirty[i];
    }
    return out;
}

void kf_fb_clear_dirty(void) {
    g_dirty_count = 0;
    g_collapsed = false;
}

size_t kf_fb_dirty_bytes(void) {
    uint32_t total_px = 0;
    for (int i = 0; i < g_dirty_count; ++i) {
        total_px += kf_rect_area(g_dirty[i]);
    }
    return static_cast<size_t>(total_px) * sizeof(kf_color);
}
