/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 */

#include "kf/font.h"

#include "kf/blit.h"
#include "kf/budget.h"
#include "kf/framebuffer.h"

#include <cstdint>
#include <cstring>

#include "font_data.h"

#include "kf/poison.h"

namespace {

constexpr int16_t kW = static_cast<int16_t>(KF_DISPLAY_WIDTH);
constexpr int16_t kH = static_cast<int16_t>(KF_DISPLAY_HEIGHT);
constexpr kf_rect kFullScreen = {0, 0, kW, kH};

const uint8_t *glyph_for(char c) {
    const int code = static_cast<unsigned char>(c);
    if (code < KF_FONT_FIRST_CHAR || code > KF_FONT_LAST_CHAR) {
        return nullptr;
    }
    return kf_font_glyphs[code - KF_FONT_FIRST_CHAR];
}

} // namespace

int16_t kf_text_width(const char *str) {
    if (str == nullptr) {
        return 0;
    }
    return static_cast<int16_t>(std::strlen(str) * KF_FONT_CELL_W);
}

void kf_text_draw(int16_t x, int16_t y, const char *str, kf_color fg,
                   kf_color bg) {
    if (str == nullptr) {
        return;
    }

    kf_color *fb = kf_fb_pixels();
    uint32_t ink_pixels = 0u;
    int16_t cx = x;

    for (const char *p = str; *p != '\0';
         ++p, cx = static_cast<int16_t>(cx + KF_FONT_CELL_W)) {
        const kf_rect cell = {cx, y, static_cast<int16_t>(cx + KF_FONT_CELL_W),
                              static_cast<int16_t>(y + KF_FONT_CELL_H)};
        const kf_rect clip = kf_rect_intersect(cell, kFullScreen);
        if (kf_rect_is_empty(clip)) {
            continue;
        }

        /* Background: one row-contiguous fill per cell. kf/blit.h already
         * clips, dirty-marks and opaque-counts this, so text does not
         * repeat any of it -- the cell fill IS the "clear whatever was
         * there" step, not a separate one. */
        kf_fill_rect(cell, bg);

        const uint8_t *glyph = glyph_for(*p);
        if (glyph == nullptr) {
            continue; /* unsupported character: bg cell only, i.e. blank */
        }

        for (int row = 0; row < KF_FONT_GLYPH_H; ++row) {
            const int16_t py = static_cast<int16_t>(y + row);
            if (py < clip.y0 || py >= clip.y1) {
                continue;
            }
            const uint8_t bits = glyph[row];
            if (bits == 0u) {
                continue;
            }
            kf_color *row_ptr = fb + static_cast<size_t>(py) * KF_DISPLAY_WIDTH;
            for (int col = 0; col < KF_FONT_GLYPH_W; ++col) {
                if ((bits & (1u << (KF_FONT_GLYPH_W - 1 - col))) == 0u) {
                    continue;
                }
                const int16_t px = static_cast<int16_t>(cx + col);
                if (px < clip.x0 || px >= clip.x1) {
                    continue;
                }
                row_ptr[px] = fg;
                ink_pixels++;
            }
        }
    }

    /* Foreground pixels are plotted one at a time, not row-copied, so on
     * the device they cost like a colour-keyed blit rather than a fill --
     * see KF_DRAW_KEYED_PX_PER_US in kf/budget.h. Counted once for the
     * whole string: the counter only cares about the total. */
    if (ink_pixels > 0u) {
        kf_draw_count_pixels(true, ink_pixels);
    }
}
