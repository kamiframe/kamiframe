/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * Bitmap text. Minimal, in the same spirit as kf/blit.h: one fixed-width
 * font, no kerning, no wrapping, no line breaks. Enough to put readable
 * numbers and labels on screen, which is the whole reason this exists --
 * see docs/architecture/adr-0010-bitmap-text.md.
 *
 * Glyphs are 5x7 pixels in a 6x8 cell (1px margin right and bottom), which
 * divides the 240x320 panel evenly: 40 columns by 40 rows of text space,
 * with no partial cell at either edge.
 *
 * Character set: space, 0-9, A-Z (uppercase only), the punctuation the
 * constraint HUD needs: . , : - / % + ( ), and "!" (added for Task 8 of
 * the screens/clock/sleep plan's attention
 * signal). Anything else in a string draws
 * as a blank cell, LOWERCASE LETTERS INCLUDED -- this module itself does no
 * case-folding. That matters well beyond the HUD this set was originally
 * sized for: every kf.text() call a Lua cartridge makes (sdk/lua/
 * kf_lua_scene.cpp's lua_kf_text(), kf_scene_add_text() underneath) goes
 * through this same font. The Lua binding already covers for it, though --
 * kf_lua_scene.cpp's uppercase_and_warn() uppercases ASCII letters on every
 * text write before it ever reaches this module, precisely so a script
 * typing `kf.text("Hello")` gets "HELLO" on screen, not four blank cells,
 * and logs once (not once per frame) for any OTHER character the font
 * genuinely cannot draw. A caller that talks to this module directly in
 * C++, bypassing that binding, gets no such help -- blank cells for
 * anything outside the set above, silently. Extending the set is a
 * mechanical edit to tools/make_font.py, not a redesign -- see the ADR for
 * why lowercase was left out of slice two.
 *
 * All coordinates are in framebuffer space, top-left of the string. All
 * functions clip. All functions mark the region they touched as dirty and
 * feed the same draw counters kf/blit.h uses, so text drawn on screen never
 * goes missing from the frame budget report.
 */

#ifndef KF_FONT_H
#define KF_FONT_H

#include "kf/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KF_FONT_GLYPH_W 5
#define KF_FONT_GLYPH_H 7
#define KF_FONT_CELL_W  6
#define KF_FONT_CELL_H  8

/* Draw `str` (NUL-terminated) with its top-left at (x, y). `bg` fills the
 * whole cell of every character, including the margin around the glyph, so
 * text stays legible over whatever was already drawn -- callers never need
 * to clear a rectangle first. There is no transparent mode: if you want
 * text with nothing behind it, that is a second slice, not a flag on this
 * one, because a flag nobody has needed yet is speculative surface. */
void kf_text_draw(int16_t x, int16_t y, const char *str, kf_color fg,
                   kf_color bg);

/* Width of `str` if drawn, in pixels: strlen(str) * KF_FONT_CELL_W. Exposed
 * so a caller can right-align or measure before drawing rather than guess. */
int16_t kf_text_width(const char *str);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KF_FONT_H */
