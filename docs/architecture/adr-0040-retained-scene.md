# ADR 0040: A retained scene and a coalescing differ, in Core

**Status:** Accepted
**Date:** 2026-08-12

## Context

Task 1 of the Lua game layer plan
(`docs/superpowers/plans/2026-08-12-lua-game-layer.md`) gave the demo pet a
real `.lua` file. That script still cannot draw a single pixel: nothing in
`sdk/lua/kf_lua_port.cpp` exposes `kf_fill_rect`, `kf_blit_frame`,
`kf_text_draw`, or the framebuffer to Lua at all. This task is the piece that
makes drawing from a script possible without breaking the frame budget.

The obvious binding — `draw_sprite(x, y)`, called every frame from Lua — would
destroy the display. `KF_MAX_DIRTY_RECTS` is 8
(`hakoniwaos/include/kf/framebuffer.h`); past that, `kf_fb_mark_dirty()`
collapses every tracked rectangle into one screen-sized box
(`hakoniwaos/src/framebuffer.cpp`). A full 240x320 RGB565 frame is

    240 * 320 * 2 bytes * 8 bits = 1,228,800 bits
    1,228,800 / 40,000,000 Hz    = 30.7 ms

against a 33.3ms budget at 30fps (`KF_FRAME_BUDGET_US`, `kf/budget.h`) — a
single full-screen redraw eats essentially the whole frame. An immediate-mode
API gives Core no way to know what actually changed between two calls, so it
has exactly two options: mark everything dirty every frame (the 30.7ms cost,
paid constantly), or make the script author track and mark dirty rectangles
by hand. The second is the one this project's audience — "a WordPress or
jQuery developer should not have too much trouble" (`CLAUDE.md`) — cannot be
asked to do.

The current worst case, measured, is 5 dirty rectangles for one frame
(creature erase+redraw, mess, three stat bars —
`run_creature_screen_budget_combination_check()`,
`headless_main.cpp`). There is real headroom under 8, but an immediate-mode
API driven by a script author has no way to know that, or to stay inside it
on purpose.

## Decision

### Retained mode: the game declares state, Core computes the diff

A new module, `kf/scene.h` / `hakoniwaos/src/scene.cpp`, holds a **retained
scene**: a fixed-size table of objects (sprite, text, or box), plus a
scene-wide background. A caller declares what should be on screen — position,
sprite name, text, colour, visibility, layer — and `kf_scene_commit()`, called
once per frame, diffs this frame's declared state against what was actually
painted last frame, computes the minimal set of dirty rectangles itself, and
repaints only those. The caller never touches a rectangle. No Lua binding
exists yet — this task is Core only, exercised by a headless check with
nothing else calling it, exactly as `KF_HOME_SCREEN` and the rest of the plan
require for a task that must not move a single rendered pixel.

### Handles, not pointers

`kf_scene_id` is a `uint16_t` that increases monotonically for the life of
the program and is **never reused**, not even across `kf_scene_reset()`. A
stale id from before a reset stays an "obviously not found" value forever. A
pointer cannot make that promise: freed storage can be handed back out by a
later allocation and start looking valid again under the old handle. 0 is
reserved and never a valid object, so "did this creation succeed" and "is
this id current" are both a single integer comparison, with nothing to leak
and nothing to double-free.

### Strings are copied, never held by pointer

A sprite name or a text string is copied into a fixed-size field on the
object the moment it is declared: 31 characters plus NUL for a sprite name
(matching the pack format's own 32-byte name field,
`hakoniwaos/src/assets.cpp`), 40 characters plus NUL for text (exactly one
full 240px row at `KF_FONT_CELL_W` = 6). Holding a pointer instead would be
the easy choice and it would be wrong under a Lua binding: a Lua string is
garbage-collected, and the object it was assigned to might not be drawn again
for several frames — by which point that pointer is dangling. A string that
does not fit is truncated and logged once, naming the object and the field;
it never fails silently.

### Two file-static arrays, nothing else

`hakoniwaos/scene.cpp` allocates nothing. The object table
(`KF_SCENE_MAX_OBJECTS` = 64) and the per-frame dirty-candidate scratch space
(`KF_SCENE_MAX_DIRTY_CANDIDATES` = 32) are both fixed-capacity file-static
arrays. `python3 tools/check_no_heap.py .` stays clean, and nothing here uses
`float` or `double` — the same constraints every other file in `hakoniwaos/`
already carries, for the same reason: this code runs identically on the
ESP32's 240MHz core, there is no emulator, and desktop's flat heap and FPU
would lie about both.

64 is not a guess: the entire current home screen (creature, up to four
poops, three stat bars, three stat labels, five care-guide labels) is under
ten objects, so 64 leaves an order of magnitude of headroom.

**Corrected, 2026-08-10.** This section originally said "roughly 40 bytes
per object — a couple of KB of static storage." That was an estimate, and
it was wrong by 5.6x. `xtensa-esp32s3-elf-nm --print-size --size-sort` on
the real ESP32 build measures `g_objects` at `0x3800` = **14,336 bytes —
224 bytes per object**. The estimate ignored what the two design decisions
directly above this one actually cost when multiplied out: strings are
copied rather than held by pointer (73 bytes of name and text buffer per
`RenderState`), and each object holds *two* `RenderState`s, `declared` and
`presented`, so those buffers are paid for twice. That is 90 bytes per
`RenderState`, 180 per object, plus a 32-byte `resolved_name` cache and the
per-object bookkeeping — 224 with the xtensa ABI's padding.

Both decisions are still right; the ADR simply never multiplied them out.
14KB of the ESP32-S3's 512KB internal SRAM is affordable and the headroom
argument survives, but the honest figure is 14KB of static storage, not "a
couple of KB." **Anyone raising `KF_SCENE_MAX_OBJECTS` should price it at
224 bytes a slot** — 128 objects would be 28KB, which is a real decision
rather than a free one.

**The array also spent its first life in `.data` rather than `.bss`, and no
longer does.** Every field in `SceneObject` is zero-initialised except one:
`RenderState::fg` defaulted to `KF_WHITE` (`0xFFFF`). A single non-zero
field anywhere in the array means the array is not all-zero, so the
toolchain cannot leave it to the runtime's boot-time zero-fill — the full
14,336 bytes of initial contents had to be stored in flash and copied into
RAM at boot. `.bss` costs RAM only; `.data` costs the same RAM *plus* an
equal number of bytes of flash, permanently.

The fix was not to change the default colour. `kf_scene_add_text()` takes
no colour arguments, so that initialiser is the entire contract behind
`kf.text("HI")` with no `:color()` call, and zeroing it would have silently
turned every un-coloured label black-on-black. Instead the white is applied
at runtime, by `kf_scene_add_text()` itself, leaving the static array
all-zero. Same rendering, `.bss` placement:

| | Section | `g_objects` | Firmware image |
|---|---|---|---|
| Before | `.data` (`d`) | 14,336 bytes | 683,760 |
| After | `.bss` (`b`) | 14,336 bytes | 669,408 |

**14,352 bytes of flash recovered, RAM unchanged**, verified by
`xtensa-esp32s3-elf-nm` on `kamiframe-firmware.elf` from a real `idf.py
build` either side of the change. The behaviour that was at risk is now
pinned by `run_scene_check()`'s check 5
(`simulator/src/headless/headless_main.cpp`), which memcmps a default text
object against `kf_text_draw(..., KF_WHITE, KF_BLACK)`; it was confirmed to
fail against the naive "just default `fg` to `KF_BLACK`" version, and no
other check in the suite — including both golden checksums — noticed that
change at all.

**What happens when a game declares more items than the scene holds.**
`kf_scene_add_sprite()` / `_add_text()` / `_add_box()` return `0` — never a
valid id — once all 64 slots are full, and log once naming the limit. They do
not panic, and they do not silently hand back a slot that does not exist.
This is the defined, non-crashing behaviour the task's constraints require:
`0` is a value every caller can check without a second "is this valid" call,
and it gives a future Lua binding (Task 3) exactly what it needs to turn an
overflow into a catchable script error (`luaL_error` naming
`KF_SCENE_MAX_OBJECTS`) instead of a crash or a `nil` a script goes on to
dereference.

### The coalescing rule, and why it beats the framebuffer's own collapse

`kf_scene_commit()` knows every candidate dirty rectangle for this frame
before it marks any of them — something the old call-and-draw-immediately
path never had. While the candidate count exceeds `KF_MAX_DIRTY_RECTS`, it
repeatedly merges the pair whose union costs the least: `area(union(a, b)) -
area(a) - area(b)`. That cost is zero or negative for two rectangles that
already touch or overlap — exactly `kf_fb_mark_dirty()`'s own
`touches_or_overlaps()` rule (`framebuffer.cpp:43`, a deliberate 1px
expansion) — so an overlapping pair is always merged before a distant one.
The scene's coalescer is built to **exploit** that merge rule rather than
duplicate or fight it: the framebuffer still does its own pass when each
final rectangle is marked, and any two of the scene's own final rectangles
that happen to touch collapse further there for free.

Two file-static candidate buffers exist for a reason: the working set is
capped at `KF_SCENE_MAX_DIRTY_CANDIDATES` (32) rather than sized to the
worst case (every one of 64 objects changing in the same frame, old rect and
new rect each, is 128 raw candidates). Past 32, `kf_scene_commit()` merges
the cheapest pair eagerly, before appending the next candidate, so the buffer
never grows and nothing is ever unbounded.

**Every final rectangle is marked with `kf_fb_mark_dirty()` before any
painting happens.** This ordering matters: each draw call issued afterwards
(`kf_fill_rect`, `kf_blit_frame`) marks its own, tighter rectangle dirty as a
side effect of drawing (`kf/blit.h`'s own contract) — and because that
tighter rectangle always lands inside a final rectangle already marked, it
merges into the existing slot instead of adding a new one. Marking after
painting would let 12 independently-moving objects hand the framebuffer 24
separate, mostly non-touching marks before its own merge logic ever got a
turn — past 8, straight into the exact full-screen collapse this module
exists to avoid. Measured, not asserted: `run_scene_check()`'s check 4 moves
12 objects spread across the panel specifically so none of their rectangles
touch for free, and the coalescer still lands at **8** dirty rectangles
covering **11,200 of 153,600** framebuffer bytes — proof the coalescer beat
the fallback, not merely avoided it by the test's own layout being lucky.

### The whole-object overdraw, and the clipped-blit follow-up not built here

An object whose bounds only partially fall inside a final dirty rectangle
still gets its **entire** bounds redrawn: `kf_blit_frame()` clips to the
screen edge, never to an arbitrary rectangle, and there is no cheaper way to
draw part of a sprite with the primitives `kf/blit.h` provides today. This is
real, deliberate overdraw, not a bug: the extra pixels it writes are either
inside the same final rectangle (nothing to worry about) or outside it but
identical to what was already there — an unchanged object redrawn in place —
never a pixel that needed to reach the panel but was left unmarked. The cost
is pixels, not correctness, and it does not grow the rectangle count.

The clean fix is a `kf_clip_push()` / `kf_clip_pop()` pair that every blit
call site would consult. It is not built here, on purpose: it would touch
every existing drawing call in `hakoniwaos/` to buy something nothing
currently measures a cost against. If a later measurement finds the overdraw
is not free, this is where the fix belongs.

### A sprite background forces a full-screen repaint; a colour background does not

The scene's background is either a colour or a sprite. A colour background
clips to any final rectangle for free — `kf_fill_rect()` takes the rectangle
as an argument. A sprite background cannot: the same "no arbitrary clip"
limitation above applies, and drawing it once per final rectangle,
unclipped, would risk one rectangle's background repaint overwriting pixels a
different rectangle had already finished painting earlier in the same
commit. So `kf_scene_commit()` detects a sprite background and, whenever
anything at all is dirty, collapses the candidate list to exactly one
full-screen rectangle before marking or painting. This is a real, named cost
— a moving object in front of a static sprite background repaints the whole
screen every time it moves — not an oversight, and it is the other
half of the reason the clipped-blit follow-up above is worth doing later
rather than never.

### Draw order: layer ascending, ties by creation order, and why that has to be stable

Objects paint in ascending `layer`, and within a layer, in the order they
were created — `kf_scene_id` is handed out strictly increasing, so id order
already *is* creation order. This has to be a **stable** rule: if two
overlapping objects on the same layer could paint in a different order from
one commit to the next, the one on top would flicker between frames for no
reason a developer using this API could see or reproduce from their own
script. Sorting by `(layer, id)` guarantees the same two objects always paint
in the same relative order for as long as both exist.

### `kf_scene_reset()` forces a full repaint, on purpose

A reset discards every declared object and the background, and sets a flag
that forces the *next* `kf_scene_commit()` to repaint the entire screen
unconditionally, bypassing the ordinary diff-against-presented path. Without
this, an object that existed before the reset but has no counterpart in
whatever gets declared next would never contribute its old bounds as a dirty
candidate again — its last-painted pixels would sit on screen forever. This
is also what makes the very first commit after program start behave
correctly with no special-casing: the flag starts `true` before any reset is
ever called.

## The proof

`run_scene_check()` (`simulator/src/headless/headless_main.cpp`, flag
`--verify-scene`) asserts, in order:

1. A committed frame with no further changes marks zero dirty rectangles and
   draws zero pixels (`kf_draw_counters_get()` reports 0/0,
   `kf_fb_dirty_rects().count == 0`).
2. Moving one object into an overlapping position marks exactly one merged
   dirty rectangle covering both the old and the new bounds.
3. **The real proof.** A scene committed through `kf_scene_commit()` — two
   objects at different layers, so paint order is actually exercised, not
   just final coverage — is `memcmp`-identical to the same picture drawn by
   hand, in the same order, with `kf_fill_rect()` and `kf_blit_frame()`
   called directly. No golden constant: two independently-produced results
   have to agree byte for byte.
4. Twelve independently-moving objects, spread so that none of their
   rectangles touch or overlap for free, coalesce to at most
   `KF_MAX_DIRTY_RECTS` rectangles covering well under the full framebuffer.
   Measured at 8 rectangles, 11,200 of 153,600 bytes.

A fifth thing the task asked for — that this check fails if
`kf_scene_commit()`'s body is deleted — is not code in the repository. It was
verified by hand once: the body was deleted, the check was rebuilt and run,
three of the four assertions above went red (checks 2, 3, and 4 — check 1
alone cannot distinguish "nothing changed" from "nothing runs"), and the
change was reverted. A check cannot prove its own vacuity by asserting about
itself, and this project has already shipped that mistake twice
(`2026-08-09-creature-on-screen.md`).

`ctest --test-dir build` is 42/42 (baseline 40, Task 1 added
`lua_embed_check` for 41, this task adds `scene_check` for 42). The ESP-IDF
cross-compile (`-DKF_PANEL=ili9341`) is clean.

## Not verified

No scene declared through this API has rendered on real hardware. Every
figure above — the 8-rectangle, 11,200-byte result for the 12-object check,
the memcmp proof — was measured on the desktop build against the desktop
HAL backend. The desktop build is the real firmware against a desktop
backend of the same HAL the ESP32 build uses (`CLAUDE.md`'s architecture
non-negotiable #1), and `hakoniwaos/scene.cpp` runs unmodified on both
targets — but "compiles and passes on the ESP-IDF toolchain" is not the same
claim as "has been flashed and watched." Nothing in this task calls
`kf_scene_commit()` from anywhere but its own headless check: Task 3 is the
Lua binding, Task 4 is the C++ creature screen rebuilt on top of this module,
and hardware verification belongs to whichever of those tasks first puts a
declared scene in the frame loop.
