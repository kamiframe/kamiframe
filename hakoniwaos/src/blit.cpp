/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 */

#include "kf/blit.h"

#include "kf/budget.h"
#include "kf/framebuffer.h"
#include "kf/hal/log.h"

#include <cstdint>
#include <cstring>

#include "kf/poison.h"

namespace {

constexpr int16_t kW = static_cast<int16_t>(KF_DISPLAY_WIDTH);
constexpr int16_t kH = static_cast<int16_t>(KF_DISPLAY_HEIGHT);
constexpr kf_rect kFullScreen = {0, 0, kW, kH};

kf_draw_counters g_counters = {0u, 0u};

int16_t clamp16(int32_t v, int32_t lo, int32_t hi) {
    if (v < lo) {
        v = lo;
    }
    if (v > hi) {
        v = hi;
    }
    return static_cast<int16_t>(v);
}

} // namespace

bool kf_rect_is_empty(kf_rect r) { return r.x1 <= r.x0 || r.y1 <= r.y0; }

uint32_t kf_rect_area(kf_rect r) {
    if (kf_rect_is_empty(r)) {
        return 0u;
    }
    return static_cast<uint32_t>(r.x1 - r.x0) *
           static_cast<uint32_t>(r.y1 - r.y0);
}

void kf_draw_counters_reset(void) {
    g_counters.opaque_pixels = 0u;
    g_counters.keyed_pixels = 0u;
}

kf_draw_counters kf_draw_counters_get(void) { return g_counters; }

void kf_draw_count_pixels(bool keyed, uint32_t count) {
    if (keyed) {
        g_counters.keyed_pixels += count;
    } else {
        g_counters.opaque_pixels += count;
    }
}

kf_rect kf_rect_intersect(kf_rect a, kf_rect b) {
    kf_rect out;
    out.x0 = a.x0 > b.x0 ? a.x0 : b.x0;
    out.y0 = a.y0 > b.y0 ? a.y0 : b.y0;
    out.x1 = a.x1 < b.x1 ? a.x1 : b.x1;
    out.y1 = a.y1 < b.y1 ? a.y1 : b.y1;
    if (kf_rect_is_empty(out)) {
        return kf_rect{0, 0, 0, 0};
    }
    return out;
}

kf_rect kf_rect_union(kf_rect a, kf_rect b) {
    if (kf_rect_is_empty(a)) {
        return b;
    }
    if (kf_rect_is_empty(b)) {
        return a;
    }
    kf_rect out;
    out.x0 = a.x0 < b.x0 ? a.x0 : b.x0;
    out.y0 = a.y0 < b.y0 ? a.y0 : b.y0;
    out.x1 = a.x1 > b.x1 ? a.x1 : b.x1;
    out.y1 = a.y1 > b.y1 ? a.y1 : b.y1;
    return out;
}

void kf_fill(kf_color color) { kf_fill_rect(kFullScreen, color); }

void kf_fill_rect(kf_rect r, kf_color color) {
    const kf_rect c = kf_rect_intersect(r, kFullScreen);
    if (kf_rect_is_empty(c)) {
        return;
    }

    kf_color *fb = kf_fb_pixels();
    const int width = c.x1 - c.x0;

    for (int16_t y = c.y0; y < c.y1; ++y) {
        kf_color *row = fb + (static_cast<size_t>(y) * KF_DISPLAY_WIDTH) + c.x0;
        for (int x = 0; x < width; ++x) {
            row[x] = color;
        }
    }

    g_counters.opaque_pixels += kf_rect_area(c);
    kf_fb_mark_dirty(c);
}

void kf_blit(const kf_sprite *sprite, int16_t x, int16_t y) {
    KF_ASSERT(sprite != nullptr, "kf_blit(nullptr)");
    KF_ASSERT(sprite->pixels != nullptr, "sprite has no pixels");

    const kf_rect want = {x, y,
                          clamp16(static_cast<int32_t>(x) + sprite->width,
                                  INT16_MIN, INT16_MAX),
                          clamp16(static_cast<int32_t>(y) + sprite->height,
                                  INT16_MIN, INT16_MAX)};
    const kf_rect c = kf_rect_intersect(want, kFullScreen);
    if (kf_rect_is_empty(c)) {
        return;
    }

    kf_color *fb = kf_fb_pixels();
    const int src_x0 = c.x0 - x;
    const int src_y0 = c.y0 - y;
    const int width = c.x1 - c.x0;
    const int height = c.y1 - c.y0;

    if (!sprite->has_color_key) {
        g_counters.opaque_pixels += kf_rect_area(c);
        /* Opaque: whole rows at a time. This is the case worth making fast,
         * because it is what a full-screen background costs. */
        for (int row = 0; row < height; ++row) {
            const kf_color *src =
                sprite->pixels +
                (static_cast<size_t>(src_y0 + row) * sprite->width) + src_x0;
            kf_color *dst =
                fb + (static_cast<size_t>(c.y0 + row) * KF_DISPLAY_WIDTH) + c.x0;
            memcpy(dst, src, static_cast<size_t>(width) * sizeof(kf_color));
        }
    } else {
        g_counters.keyed_pixels += kf_rect_area(c);
        const kf_color key = sprite->color_key;
        for (int row = 0; row < height; ++row) {
            const kf_color *src =
                sprite->pixels +
                (static_cast<size_t>(src_y0 + row) * sprite->width) + src_x0;
            kf_color *dst =
                fb + (static_cast<size_t>(c.y0 + row) * KF_DISPLAY_WIDTH) + c.x0;
            for (int col = 0; col < width; ++col) {
                const kf_color p = src[col];
                if (p != key) {
                    dst[col] = p;
                }
            }
        }
    }

    kf_fb_mark_dirty(c);
}
