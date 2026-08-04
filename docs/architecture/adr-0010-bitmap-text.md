# ADR 0010: Bitmap text and the constraint HUD

**Status:** Accepted, 2026-08-04
**Reversal cost:** Low. The character set and cell size are a generator
script and a two-function API; nothing else in core depends on their shape.

## Requirement

ADR 0006 named this explicitly and deferred it: *"A constraint HUD in the
simulator (frame ms, arena high-water marks, Lua heap, dirty-rect percentage
as an overlay) needs font rendering, so it is a later slice."* This is that
slice. The numbers already existed -- `kf_app_log_budget_report()` has
printed them to the console since slice one -- the gap was putting them on
the panel itself, which is the only place they will exist on the device.

## Decision: a small hand-authored font, a strict two-function API, an opt-in HUD

**1. The font is authored, not imported.** `tools/make_font.py` defines 46
glyphs (space, `0-9`, `A-Z`, and `. , : - / % + ( )`) as 5x7 ASCII art and
packs them into a generated header, the same pattern `make_test_sprite.py`
already established for the demo sprite. Nothing is extracted from an
existing font file or library: there is no third-party bitmap data in the
repo to license, and the character set is exactly what the HUD needs today,
not a guess at what a future consumer might want. Extending it is an edit to
the `GLYPHS` dict and a re-run of the script, not a redesign.

**2. 5x7 glyphs in a 6x8 cell**, because 240/6 = 40 and 320/8 = 40: the panel
divides into a whole number of text columns and rows with no partial cell at
either edge.

**3. `kf/font.h` is two functions**, matching the size of `kf/blit.h`:
`kf_text_draw` and `kf_text_width`. No wrapping, no kerning, no colour
per-character, no transparent mode. `kf_text_draw`'s `bg` parameter always
fills the whole cell, so a caller never needs a separate clear step -- the
same reasoning `kf_fill_rect` already uses.

**4. Text feeds the same draw counters `kf/blit.h` does.** A new
`kf_draw_count_pixels(keyed, count)` in blit.h lets font.cpp (or any future
drawing module) contribute to `kf_draw_counters` without font.cpp reaching
into blit.cpp's private state. Background cells go through `kf_fill_rect`
directly and are counted opaque automatically; foreground glyph pixels are
plotted one at a time and counted keyed, because that is their real cost
shape on the device, whether or not an actual colour key is involved. A
budget you can draw around is not a budget.

**5. The HUD is off by default, toggled with `KF_BTN_MENU`.** This is the
one design choice in this slice with a real trade-off behind it, below.

## Why the HUD defaults off

The dirty-rectangle model in `kf/framebuffer.h` tracks one bounding box, not
a list, and says so plainly: *"cheap, never wrong, and only ever
over-sends."* A fixed HUD in the top-left corner and a sprite roaming the
whole screen are exactly the case that costs the most: the bounding box
spans from wherever the HUD is to wherever the sprite is, not just the two
regions' actual areas.

This was measured, not assumed. With the HUD forced on and the sprite in the
bottom-right corner, `mean-dirty-percent` was **97%**, against roughly 2%
with the HUD off -- confirmed by running `kamiframe-sim` under Xvfb and
reading the frame budget report. A HUD that redraws every frame regardless
of position is not free; combined with a moving sprite it is close to a full
frame every frame.

Two consequences followed from that number:

- The HUD stays **opt-in**, toggled with a button the headless CI script
  never presses (see `simulator/src/headless/headless_input.cpp`). The
  golden-frame and dirty-area regression tests keep exercising the demo
  alone, unchanged, and both checksums (`964b2c7426d41723`,
  `6b9e3d13c8b9af4f`) are untouched by this slice -- confirmed by a clean
  `ctest` run before and after.
- `kf/demo.h` gained one new function, `kf_demo_request_full_repaint()`,
  called from `kf/app.h` on every MENU press in either direction. Turning
  the HUD off leaves its last frame's pixels sitting in the framebuffer with
  nothing left to redraw them; only the demo knows its own background colour
  well enough to clear them. It is a narrow, deliberate crack in demo.cpp's
  placeholder status, and it closes when demo.cpp does.

A smarter dirty-rect strategy (a short list of rects instead of one box) is
the real fix, and framebuffer.h already says to revisit only when
measurement says the box is costing real transfer time. It now has a real
number to point at, when that revisit happens.

## Verified

- Full clean rebuild, GCC 13, `-Wall -Wextra -Wshadow -Wconversion
  -Wsign-conversion -Wcast-qual -Wdouble-promotion -Werror`. One false
  positive (`-Wmaybe-uninitialized` on a pointer computed from an
  uninitialised `char[48]`) fixed by zero-initialising the buffer rather
  than suppressing the warning.
- `tools/check_no_heap.py`: clean. Text drawing allocates nothing; the
  glyph table is `static const`, generated into `hakoniwaos/src/font_data.h`
  exactly like the demo sprite.
- All three `ctest` targets pass, with both golden checksums byte-identical
  to their pre-slice values.
- Every glyph rendered to a PNG via `tools/make_font.py --preview` and
  read back before any C++ was written against the data, catching
  transcription mistakes while they were still one line to fix.
- The HUD itself verified end-to-end: `kamiframe-sim` run under Xvfb with
  the visibility default temporarily forced on, screenshotted, and read back
  -- all three digits, all 26 letters and every symbol in the character set
  appear somewhere in the two HUD lines the demo exercises, rendered through
  the real framebuffer and blit path, not a synthetic test harness.

## Accepted cost

Uppercase only, 46 glyphs. Log messages and any future in-game text wanting
lowercase or punctuation outside this set need another pass at
`tools/make_font.py`. That is deliberate scope, not an oversight: this slice
exists to unblock the HUD ADR 0006 already asked for, not to build a general
text system speculatively.

## Later

- Lowercase, and the rest of printable ASCII, when something other than the
  HUD needs them.
- A smarter dirty-rect strategy, if the HUD (or anything else with a fixed
  screen position alongside moving content) needs to be on by default.
- Whatever the LVGL-versus-custom sprite engine evaluation decides may
  replace this outright. Nothing here prejudges it: `kf/font.h` sits above
  `kf/framebuffer.h` exactly as `kf/blit.h` does, and is exactly as
  disposable.
