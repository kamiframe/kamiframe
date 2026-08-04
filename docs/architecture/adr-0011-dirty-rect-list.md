# ADR 0011: A dirty-rectangle list, not a single box

**Status:** Accepted, 2026-08-04
**Reversal cost:** Low. `kf_fb_dirty_rects()` is the only new surface core
exposes; a caller that wants the old behaviour back gets it by unioning the
list itself, which is exactly what overflow already falls back to.

## Requirement

ADR 0010 measured the cost of the single-bounding-box model named in
`kf/framebuffer.h` and put a number on it: a fixed HUD in one corner and a
sprite in the opposite one drove `mean-dirty-percent` to 97%, because a
single box has to span from wherever the HUD is to wherever the sprite is,
not just the two regions' actual areas. ADR 0010 kept the HUD off by default
to avoid that cost and named the real fix for later: *"a smarter dirty-rect
strategy (a short list of rects instead of one box) ... revisit only when
measurement says the box is costing real transfer time. It now has a real
number to point at."*

That revisit came sooner than planned. Watching the HUD running live, the
person building this project saw the frame rate collapse toward the sub-50fps
range whenever the sprite drifted away from the HUD corner, and asked
directly for the fix rather than an "it's a known trade-off, here is the
number" explanation: the HUD is expected to be visible more or less
permanently in the real virtual-pet product, so 97%-dirty is not an edge case
to defer around, it is close to the common case.

## Decision: a capped list of independent rectangles

`kf/framebuffer.h` now tracks up to `KF_MAX_DIRTY_RECTS` (8) rectangles
instead of one. `kf_fb_mark_dirty()` merges a new rectangle into an existing
one only when they touch or overlap (a deliberate 1px expansion on the
touch test, so adjacent glyph cells in a text run coalesce into one
rectangle rather than staying eight separate ones); otherwise it takes a new
slot. If a frame dirties more independent regions than there are slots, the
whole list collapses into a single union box for that frame -- the exact
ADR 0010 behaviour, and always correct, only ever a return to the old cost.

Two things follow from the list existing:

- **`kf_display_present()`'s signature changed shape**: a single `kf_rect`
  becomes `const kf_rect *dirty_rects, int dirty_rect_count`.
  `KF_HAL_DISPLAY_VERSION` moved 1 -> 2 for it. Desktop (SDL) still ignores
  the rectangles -- uploading 153KB to a GPU texture costs nothing worth
  optimising -- but the signature now carries what the device backend will
  need, on day one, the same reasoning ADR 0009 used for transfer cost.
- **The transfer-cost estimate gained a per-rectangle term.**
  `KF_DISPLAY_RECT_OVERHEAD_BYTES` (11, modelling an ST7789's
  CASET+RASET+RAMWR addressing sequence) is now added once per rectangle in
  `kf_app_frame()`'s budget accounting, so splitting one region into many
  small ones is not free in the estimate the way it would be free in the
  code. Eight tiny rectangles costing 88 bytes of pure addressing overhead
  before a single pixel moves is a real number a HUD-heavy screen layout
  needs to see.

The HUD's own budget line now shows the rectangle count (`R<n>`) next to the
dirty percentage, so a developer watching the HUD can see the list saturate
and collapse before `dirty_percent` jumps back toward single-box territory.

## A second bug found while verifying this one

Testing the rewrite against the existing golden-checksum `ctest` targets
surfaced a genuinely non-deterministic checksum in `headless_fullscreen`:
identical `--seed` and `--frames`, different output, run to run. AddressSanitizer
and UndefinedBehaviorSanitizer builds were clean, and the checksum in
`headless_display.cpp` reads only framebuffer bytes, independent of the dirty
rectangles -- so whatever was wrong had to be producing genuinely different
pixels, not corrupting memory or miscounting rectangles.

It was not this slice. `simulator/src/headless/headless_input.cpp` fed core's
button debounce (`kDebounceUs = 8000` in `app.cpp`) a `sampled_at_us` drawn
from `kf_time_mono_us()` -- the real host clock. That is correct on real
hardware and in the SDL simulator, where a frame really does take about as
long as it says. But the headless runner disables real-time pacing
(`kf_host_time_set_realtime(false)`) so a 300-frame run takes milliseconds
rather than ten seconds, and never sleeps between polls -- so the real
elapsed time the debounce measured between frames was pure scheduler noise:
how many microseconds a given frame's CPU work happened to take on that
run, on that machine, under whatever else was competing for the CPU.
Reproduced reliably by running the binary repeatedly under artificial CPU
load (`yes > /dev/null &` a few times over): 2 of 15 runs came back with a
different checksum from the rest, confirmed by inspection to be a debounce
edge landing on a different simulated frame each time, not any drawing code
disagreeing with itself.

The fix stayed inside `headless_input.cpp`: `sampled_at_us` now advances by
one `KF_FRAME_BUDGET_US` tick per poll, a synthetic clock rather than a
passthrough to the real one. Debounce sees exactly what a correctly-paced
30fps run would see, independent of how fast the host executes the loop.
Verified deterministic across 30 `--stress` runs and 15 plain runs under
sustained artificial CPU load (4 `yes` processes competing for the CPU), and
across 10 full `ctest` passes under the same load, after being flaky within
15 runs before the fix. This was a latent bug in the headless backend, not
introduced by the dirty-rect-list change -- it simply had never been
exercised enough times, under enough load, to show up before now.

Because the fix changes what real button presses fire on, in demo terms, on
which frame, both golden checksums moved: `964b2c7426d41723` ->
`2aceae654b21ca1b` (sprite mode) and `6b9e3d13c8b9af4f` -> `3fbdcabfba49e9ef`
(`--stress`). These are not arbitrary: they are what a correctly-paced run
has always been meant to produce, and are now provably stable rather than
provably not.

## A third, smaller bug found the same way

Forcing the HUD on to verify the rectangle count visually turned up stale
text: `kf_text_draw()` only paints cells for the characters it is given, and
that painting IS the "clear whatever was there" step (ADR 0010's own
reasoning for why the background fill needs no separate clear call). Every
HUD field varies in digit width frame to frame (fps, microseconds, percent,
rectangle count, arena kilobytes), so a line that got shorter than the
previous frame left that frame's trailing glyphs sitting uncleared -- visible
in a verification screenshot as `D8% R2R1`, a "2" glyph with a stale "1" and
"R" from an earlier, longer line behind it. Pre-existing since ADR 0010, not
introduced here, but this slice's own screenshot is what caught it, so it is
fixed here rather than filed and left for later: `draw_hud()` now pads every
line to a fixed 40-column width (exactly `KF_DISPLAY_WIDTH / KF_FONT_CELL_W`,
so it never exceeds the panel) before drawing, so each frame's line always
overwrites the widest a line has ever been. The HUD is off by default and
untouched by the golden-frame tests, so this has no effect on either
checksum.

## Verified

- Full clean rebuild, GCC 13, same warning set as ADR 0010, `-Werror` clean.
- `tools/check_no_heap.py`: clean. The dirty-rect list is a fixed
  `kf_rect[KF_MAX_DIRTY_RECTS]`, no allocation.
- All three `ctest` targets pass against the updated golden checksums --
  confirmed not just once but across 8-10 repeated full-suite runs under
  sustained artificial CPU load, the exact condition that exposed the
  headless-input bug in the first place.
- The headless `--stress` and plain runs individually verified deterministic
  across 30 and 15 repeated invocations respectively, under the same
  artificial load, both before establishing the bug (flaky) and after the
  fix (stable).
- The actual fix this slice exists for, measured: with the HUD forced on and
  the sprite in the far corner from it, `mean-dirty-percent` is **9-15%**
  (varies with how much of the run the HUD spends adjacent to versus far
  from the sprite), against ADR 0010's measured 97% for the same scenario
  under the old single-box model -- and the frame budget report shows the
  HUD row and the sprite tracked as 2 independent rectangles, not collapsed
  into one spanning both. Confirmed both from the headless summary output
  and visually, via `kamiframe-sim` run under Xvfb, screenshotted, and read
  back.
- The stale-HUD-text bug fixed above verified the same way: screenshotted
  before (stale trailing glyphs visible) and after (clean) the padding fix.

## Accepted cost

Eight rectangles is a guess, not a measurement of the eventual real UI: a
constraint HUD (5 lines) plus one moving sprite needs at most 2-6 depending
on how much text changes in a given frame, but a busier future screen layout
could saturate the list and silently fall back to the old single-box cost.
`dirty_rect_count` is exposed specifically so that regression is visible on
the HUD itself rather than discovered as an unexplained frame-rate drop.

The per-rectangle transfer overhead (`KF_DISPLAY_RECT_OVERHEAD_BYTES = 11`)
is a reasonable estimate for an ST7789-class panel's addressing sequence, not
a value taken from this project's actual hardware -- there is no device yet
to measure it on. It exists so the estimate cannot lie by omission, not
because the exact figure is load-bearing yet.

## Later

- Revisit `KF_MAX_DIRTY_RECTS` once there is a real UI layout (not just the
  HUD-plus-sprite demo) to measure against.
- The device backend actually using the rectangle list to skip pixels, once
  there is a device.
