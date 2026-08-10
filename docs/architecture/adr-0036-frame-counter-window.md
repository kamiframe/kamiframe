# ADR 0036: The frame budget counters count the frame the port actually drew

**Status:** Accepted
**Date:** 2026-08-11

## Context

`docs/superpowers/plans/2026-08-11-hardware-bringup.md` (Task 1) exists
because the hardware bring-up plan's own risk audit found that the number
the plan's later, on-device budget task is supposed to read was, at the
time of writing, **provably zero, structurally** -- not a measurement that
happened to come back small.

`hakoniwaos/src/app.cpp`'s `kf_app_frame()` called `kf_draw_counters_
reset()` at its own top, then `kf_demo_update()`/`kf_demo_draw()`, then
`kf_draw_counters_get()` near its end. On every real device build,
`kf_app_init(KF_DEMO_NONE)` is used (`ports/esp32/main/app_main.cpp`), and
`KF_DEMO_NONE` makes `kf_demo_update()`/`kf_demo_draw()` return immediately
(`hakoniwaos/src/demo.cpp`) -- nothing draws inside `kf_app_frame()` at
all. The creature is drawn by a **port**, after `kf_app_frame()` returns:
`kf_screen_nav_frame()` at `app_main.cpp:290` on device, the SDL
simulator's equivalent call on desktop. So the sequence on every device
build was: reset counters, draw nothing, read counters (still zero),
return; only *after* that does anything draw. `opaque_pixels`,
`keyed_pixels` and the `draw_us` estimate derived from them read `0` on
every frame, forever -- and would have kept reading `0` even if the
indexed blit (the actual risk the bring-up plan's risk #2 names) were a
hundred times slower than `kf/budget.h`'s assumed rate.

The dirty rectangles were **not** affected by the equivalent problem:
`kf_fb_dirty_rects()`/`kf_fb_clear_dirty()` run near the top of
`kf_app_frame()` too, but a rectangle a port marks dirty *after*
`kf_app_frame()` returns survives `kf_fb_clear_dirty()` (that call already
ran) and is read back on the *next* `kf_app_frame()` call, one frame late.
`dirty_rect_count`/`transfer_us` were already correct on device, just
delayed by one frame -- which is why the plan's own risk audit could
observe "`dirty_rect_count` looks right on device while `keyed_pixels` is
0" as two different facts about the same bug.

Separately, `KFDBG STATE`'s JSON (`kf_dbg_bridge.cpp`'s `handle_state()`)
never serialised any of `kf_frame_stats`/`kf_frame_summary` except `fps`
(derived) and `frame_us` (`cpu_us`). Even with the counters fixed, there
was no wire command that could read `keyed_pixels`, `dirty_rect_count`, or
any of the rest off a real device. Two independent defects, one task,
because they are the same measurement: a counter nothing resets correctly,
serialised by a protocol that does not carry it.

## Decision

### 1. Move the reset, not the read

`kf_draw_counters_reset()` moves from the top of `kf_app_frame()` to
immediately after `kf_draw_counters_get()`, a few lines before the
function returns. The instinctive fix -- "reset later, just before the
port's drawing" -- is not available: `kf_app_frame()` is Core, and Core
has no way to know a port draws anything after it returns at all, so there
is no point inside this function that is "just before" work that happens
entirely outside it.

The move that works is smaller and initially looks like it does nothing:
reset right after the read, instead of at the top. This changes what the
counters' window *means*. It is no longer "pixels drawn during this call
to `kf_app_frame()`" -- it never actually was that, on `KF_DEMO_NONE` --
it becomes "pixels drawn since the last time this was read," which spans
from just after the *previous* call's `kf_draw_counters_get()` to just
after *this* call's. That window now includes whatever a port drew in
between the two calls -- exactly the drawing that happens after
`kf_app_frame()` returns and before the next one is called. This is
**deliberately the same window** `kf_fb_dirty_rects()`/
`kf_fb_clear_dirty()` already use for the identical reason: counters and
dirty rectangles now describe the same set of drawing, one frame late, on
every backend, instead of disagreeing the way `dirty_rect_count` (right)
and `keyed_pixels` (wrong) used to.

`KF_DEMO_FULLSCREEN` (the desktop `--stress` path) is unaffected by the
move: `kf_demo_draw()` runs *inside* `kf_app_frame()` on that path, so its
one draw and its one get-then-reset land in the same call they always did.
`docs/frame-budget.md`'s published `--stress` numbers do not move.

### 2. `post_us` lives in the port, not in Core

The segment a port spends drawing after `kf_app_frame()` returns has no
home inside `kf_frame_stats` -- that struct is Core's own accounting of
what happens *inside* `kf_app_frame()`, and `cpu_us` keeps meaning exactly
what it means today (dominated, on device, by `kf_display_present()`'s
real SPI transfer). `ports/esp32/main/app_main.cpp`'s loop now brackets
the port-owned segment -- `kf_pet_session_frame()`, `kf_screen_nav_frame()`
(the actual creature draw), the conditional `kf_lvgl_port_pump()`,
`kf_lua_port_frame()` -- with two `kf_time_mono_us()` reads, stores the
difference in a file-static `uint32_t`, and exposes it as
`kf_app_post_frame_us()`. Declared in a new header,
`ports/esp32/main/kf_app_post_frame.h`, rather than folded into
`kf_dbg_bridge.h`: the two files run in opposite directions here --
`kf_dbg_bridge.h` is the bridge exposing state *to* `app_main.cpp`
(`kf_dbg_input_mask()`, `kf_dbg_time_multiplier()`); this is `app_main.cpp`
exposing state *to* the bridge, so `kf_dbg_bridge.cpp` can serialise it.

### 3. Thirteen new `KFDBG STATE` keys, existing keys untouched

`handle_state()` (`kf_dbg_bridge.cpp`) gains: `draw_us`, `transfer_us`,
`cpu_us`, `post_us`, `dirty_rects`, `dirty_pct`, `opaque_px`, `keyed_px`,
`over_budget`, `worst_us`, `p99_us`, `frames`, `over_budget_frames`.
Everything but `post_us` comes straight out of `kf_app_last_frame()`
(`kf_frame_stats`) and `kf_app_frame_summary()` (`kf_frame_summary`) --
Core already computed all of it, it just was never serialised.
`cpu_us` duplicates `frame_us`'s value under this ADR's new name; `frame_us`
stays exactly as it was, since existing hosts and habits depend on it.

The reply buffer grows from 512 to 1024 bytes. Computed, not guessed: the
literal text of the new format string (every key name, quote, colon and
comma, no substituted values) is 430 bytes; the worst-case width of every
substituted value across the whole JSON -- `uint64_t` fields as 20 digits,
`uint32_t`/`size_t` fields as 10, the two `%s` fields as `"false"` (5) --
sums to 342; plus the trailing NUL `snprintf` always writes, the true
worst case is 773 bytes. 512 fit the old 16-field JSON with room to spare
but not this one; 1024 clears the measured worst case with margin for a
field or two more before the buffer is the next thing that needs raising.

### 4. The host tool gets a budget line, and `watch` stays short

`tools/kf_debug.py`'s `cmd_state` (non-`--json`) keeps its existing
per-key dump (which already shows every field including the new ones, by
construction -- it prints whatever keys arrive) and gains one more line,
`_format_budget_line()`, collecting the thirteen new fields into a single
readable summary. `cmd_watch`'s one-line-per-poll format does **not** grow
to match -- adding thirteen keys to a dump that already showed sixteen
would make each line unreadable, defeating the reason `watch` exists
("the command Chris will actually stare at for minutes at a time"). It
switches from a generic key dump to a curated line
(`_format_watch_summary()`): `fps`, `cpu_us`, `post_us`, `dirty_rects`
(labelled `rects`) -- the frame-budget numbers `watch` is for. Every other
field remains a `kf_debug.py state` away.

Both formatters use `.get(key, "?")`, never `obj[key]`, for every one of
the new fields: an older firmware on the bench, predating this ADR
entirely, is a real bring-up scenario, and a `KeyError` there would read
as a hardware fault rather than a version mismatch.

### 5. The desktop proof: `--verify-frame-counters`

`simulator/src/headless/headless_main.cpp` gains
`run_frame_counters_check()`, registered as `frame_counters_check` in
CTest. It drives `kf_app_init(KF_DEMO_NONE)` against the real
`creature_demo` pack (not the default pack -- see below), then
`kf_pet_session_init()`/`kf_creature_screen_init()`, then three iterations
of `kf_app_frame()` followed by `kf_creature_screen_frame(33u)` -- the
same creature-screen entry point `run_creature_screen_check()` already
uses, in the same relative order `app_main.cpp:247-290` uses on device
(`kf_app_frame()` returns, *then* something draws), without needing to
stand up LVGL to get there.

Three iterations, not one: the counters' window is now one frame **behind**
the draw that fills it, so a single iteration reads a window nothing had
drawn into yet. Two asserts: `keyed_pixels > 0` (the bug this exists to
catch), and `keyed_pixels` in `[2000, 8000]` -- an order-of-magnitude
window for a single 48x48 sprite draw, not an exact figure, because pose
and mess legitimately move the exact count. A third asserts
`dirty_rect_count` is in `[1, KF_MAX_DIRTY_RECTS]`.

**Needs the real `creature_demo` pack, not the default one.** Only a real,
colour-keyed sprite blit posts to the *keyed* draw-counter bucket
(`kf/blit.h`); the placeholder `kf_fill_rect()` the default pack falls
back to (it carries no creature art) posts to the *opaque* bucket instead.
Mounting the default pack here would have made `keyed_pixels` read `0` for
a reason that has nothing to do with the bug this check exists to catch --
exactly the vacuous-pass trap `2026-08-09-creature-on-screen.md` already
names once for this codebase ("an anti-vacuity assertion turned out to
pass with the entire drawing path deleted").

## What this task does NOT reach

- **The budget measurement itself.** This task builds the instrument;
  Task 7 of the hardware bring-up plan takes the reading on real silicon
  and, per `kf/budget.h`'s own banner, may correct
  `KF_DRAW_KEYED_PX_PER_US` *from that data* -- not before.
- **`tools/kf_panel.py`** -- out of scope, the same boundary every KFDBG-
  touching ADR before this one has drawn for its own new fields/commands.
- **The care-guide label question** (`1:FEED` etc. naming keyboard keys
  the device does not have) -- unrelated, deliberately left to the plan's
  own later tasks.

## Verified

- **Desktop.** `cmake --build build -j8` clean, zero new warnings.
  `./build/kamiframe-headless --verify-frame-counters` confirmed **failing**
  on `keyed_pixels > 0` before the reset moved (both `opaque_pixels` and
  `keyed_pixels` read `0`, `dirty_rect_count` read `1` -- matching the plan's
  own diagnosis exactly), then **passing** after, reporting `keyed_pixels=
  2304`, `opaque_pixels=2304`, `dirty_rect_count=1` for a known,
  fully-opaque 48x48 sprite (`48*48=2304`; the sprite's own colour key is
  never actually hit, but the whole blit still posts to the keyed bucket
  per `kf/blit.h`'s "cost shape, not literal key" rule). `ctest --test-dir
  build`: **38/38** (37 + this task's new check). `headless_determinism`
  and `headless_fullscreen`'s golden checksums are unchanged -- the reset
  move is a no-op for `KF_DEMO_FULLSCREEN`, as designed.
- **`python3 tools/check_no_heap.py .`** passes: `hakoniwaos/src/app.cpp`'s
  change is a function-call relocation and a comment, no allocation, no
  floating point.
- **`python3 tools/kf_debug_selftest.py`**: all checks pass, including
  three new ones -- a full `KFDBG STATE` reply carrying every ADR 0036
  field prints a budget line containing all thirteen values; a reply
  missing all of them (simulated older firmware) prints `?` in their place
  without raising `KeyError`; `_format_watch_summary()` does the same for
  `watch`'s curated line, both with every key present and with none.
- **`idf.py build` clean for esp32s3** against the real ESP-IDF v6.0.2
  install, zero warnings. `kamiframe-firmware.bin`: **660,896 bytes**
  (0xa15a0), up from ADR 0035's 660,336-byte baseline by +560 bytes --
  the new `KFDBG STATE` field set, the doubled reply buffer, the
  `post_frame_start_us`/`g_post_frame_us` bracketing in `app_main.cpp`,
  and `kf_app_post_frame_us()` itself. `ota_0` partition free percentage
  is unaffected at this scale (still 58%, unchanged to the percentage this
  tool reports).

## Not verified

- **Nothing in this task has run on real hardware.** No board is reachable
  from this environment. `keyed_px` reading non-zero and `post_us` reading
  a plausible non-zero number over an actual `KFDBG STATE` request to a
  physical device -- the two facts that would retire the bring-up plan's
  risk #2 -- are both still open. Per the plan's own words: "if `keyed_px`
  is still `0` on device after this task, the fix did not take and no
  budget claim in Task 7 is admissible." This ADR does not claim it took
  on device, only that it took in the one place it can currently be
  checked -- the desktop simulator running the same `hakoniwaos/` and
  `simulator/src/pet/` sources against a different HAL backend.
- **The exact `keyed_pixels` figure a real ESP32-S3 will report** may
  differ from the desktop's 2304: both run identical code against an
  identical framebuffer size and an identical sprite, so it should not,
  but "should not" is not the same claim as "measured."

## Cost to change

**Adding a fourteenth `KFDBG STATE` field:** one more `%lu`/`%llu`/`%s` in
`handle_state()`'s format string, one more argument, and a fresh worst-case
buffer-size computation the same way this ADR's did -- 1024 has margin for
one or two more before that math needs redoing.

**Reverting the counter-reset move:** would reintroduce exactly the defect
this ADR closes -- `keyed_pixels`/`opaque_pixels` reading `0` on every
`KF_DEMO_NONE` frame regardless of real draw cost. Not a one-line revert
with no cost: `run_frame_counters_check()` would immediately catch it,
since it is built to fail exactly that way.
