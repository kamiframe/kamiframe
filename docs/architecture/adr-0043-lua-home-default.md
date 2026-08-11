# ADR 0043: `kf_scene_force_repaint()`, and `KF_HOME_SCREEN` defaults to `lua`

**Status:** Accepted
**Date:** 2026-08-12

## Context

Task 5 of the Lua game-layer plan
(`docs/superpowers/plans/2026-08-12-lua-game-layer.md`) proved
`examples/creature_demo/creature.lua` draws Home byte-identically to
`kf_creature_screen.cpp` across 250 frames (`run_lua_vs_cpp_screen_check()`,
ADR 0042), but left one gap open rather than patching it: `screen_nav_check`
fails under `KF_HOME_SCREEN=lua` because `kf/scene.h` has no way to force a
full repaint on re-entry to Home without also destroying every scene id
`creature.lua`'s top-level code created and is still holding. Reproduced
directly before this task changed anything:

```
[E] headless FAILED: row 280 (inside the reserved y=[260,320) stats band)
    still held Info's leftover pixels after switching back to Home --
    kf_creature_screen_enter() must repaint the whole panel, not just kField
```

Flipping `KF_HOME_SCREEN`'s default to `lua` on top of that gap would ship
broken Home→Info→Home navigation as the out-of-the-box experience, so the
plan's own end note reordered the repaint fix ahead of the flip and made
them one unit. This ADR records both.

## Decision

### `kf_scene_force_repaint()`: the same flag `kf_scene_reset()` already had, exposed without the teardown

`kf_scene_reset()` already forces the next `kf_scene_commit()` to repaint the
whole panel — that is how `kf_creature_screen_enter()` solves the identical
black-trail problem (ADR 0017) for the cpp screen, which re-declares every
object from scratch on every entry. `creature.lua` does not re-declare: its
`kf.sprite()`/`kf.text()`/`kf.box()` calls run once, at script load, and the
script holds those ids for the life of the process. Calling
`kf_scene_reset()` on re-entry would invalidate all of them out from under
the running script.

The fix is one function, because the underlying mechanism already existed
and only needed a doorway that skips the destructive half:

```c
void kf_scene_force_repaint(void) {
    g_force_full_redraw = true;
}
```

That is the entire body. `kf_scene_reset()` sets the identical flag alongside
wiping `g_objects`, `g_bg_declared`/`g_bg_presented` and the candidate
buffer; `kf_scene_force_repaint()` sets only the flag. Every object keeps its
id, its position, its layer — everything — and the next commit repaints the
whole screen using exactly that unchanged declared state. This is deliberately
**engine capability, not a special case for Home**: any game with more than
one screen hits the same problem the moment something else (another retained
scene, an LVGL screen) has painted over the panel since this screen was last
active, and `kf/scene.h` is where every other scene primitive already lives.

**Why this cannot become "repaint everything every frame by accident."**
There is exactly one way to reach a full repaint through this API: a caller
must name the call. Nothing in `kf_scene_commit()`'s ordinary per-frame path
sets `g_force_full_redraw` — only `kf_scene_reset()` and this new function
do, and both are one-shot flags cleared the moment the next commit consumes
them (`kf_scene_commit()`'s existing `g_force_full_redraw = false;` at its
own end, unchanged). A script or a screen driver has to explicitly decide
"I am becoming active again" to pay this cost; there is no path that spends
it silently on a frame nobody asked for one.

### The caller: `kf_lua_home_screen_enter()`, wired into `kf_screen_nav.cpp`'s `load()`

`kf_screen_nav.cpp`'s `load()` already called `kf_creature_screen_enter()`
synchronously, inline, the moment navigation switches to Home under
`KF_HOME_SCREEN=cpp` — not on the next frame tick, because
`screen_nav_check` (and, on real hardware, a player mashing the menu button)
inspects the panel immediately after the switch, before any per-frame update
has run. The Lua counterpart, `kf_lua_home_screen_enter()`
(`simulator/src/lvgl/kf_lua_home_screen.cpp`), has to match that timing: it
calls `kf_scene_force_repaint()` and then commits immediately, guarded by
`kf_lua_scene_declared_anything()` — the same guard
`kf_lua_home_screen_frame()` already uses so a script that declared nothing
never forces a commit against an empty scene.

Unlike the cpp path's matching re-entry call
(`kf_creature_presenter_force_anim_restart()`), nothing here touches the
animation cursor. The cpp path needs that call because its scene object is
freshly recreated on every entry and its animation state has to be told "this
is not a continuation." The Lua path's creature sprite object is the *same*
object it always was — `kf_scene_force_repaint()` does not touch it — so its
animation cursor is already exactly where the wander left it. There is
nothing to reset.

### The check that proves the timing matters: `screen_nav_check` had to learn to boot Lua first

Adding `kf_scene_force_repaint()` alone did not turn `screen_nav_check`
green. The check builds its own headless environment by calling
`kf_screen_nav_init()` directly — it never called `kf_lua_port_init()`,
because there was never a reason to before Home could be Lua-drawn. Under
`KF_HOME_SCREEN=lua` that left the check exercising an empty scene (Home's
error banner only) with no background ever declared, so the row-280
assertion failed for an unrelated reason: no script had run, not a repaint
bug. `run_screen_nav_check()` now boots the Lua VM with the real demo script
in the same relative order `sdl_main.cpp`/`app_main.cpp` already use
(`kf_screen_nav_init()` — which creates the error banner and sets
`kf.home_screen_active()` — **then** `kf_lua_port_init()`, which runs
`creature.lua`'s top-level code and is what actually declares the
background), under `#ifdef KF_HOME_SCREEN_LUA` so the `cpp` build's version
of this check is untouched. Only after both fixes together did the check
turn green — each was independently necessary, confirmed by watching the
check fail for a different, correct reason with only one of the two applied.

### `KF_HOME_SCREEN` now defaults to `lua`

Both CMake entry points (`simulator/CMakeLists.txt`,
`ports/esp32/CMakeLists.txt`) flip their `set(KF_HOME_SCREEN "lua" CACHE
STRING ...)` default. `cpp` stays fully buildable —
`idf.py -DKF_HOME_SCREEN=cpp build` on the device, `-DKF_HOME_SCREEN=cpp` on
desktop — as the parity reference and fallback the plan calls for. Verified
the default itself, not just an explicit override: `cmake -UKF_HOME_SCREEN
build` (dropping the cached value, forcing the `CACHE STRING` default to
apply on the next configure) lands on `KF_HOME_SCREEN:STRING=lua` with no
flag passed at all.

### What did NOT move: the wander stays in C++, shared through `kf_creature_presenter`

The plan's own "What moves and what stays" table always meant the wander
(`kf_creature_update`, `choose_target`, the dwell) and pose/sprite-name
selection to end up owned by the script. This task does not do that, and the
decision not to is deliberate, not an oversight — the size of the remaining
work and its risk profile are different in kind from the repaint fix and the
flip:

- **Bit-exact determinism is the hard requirement, and it is easy to get
  subtly wrong.** `run_lua_vs_cpp_screen_check()` reseeds `kf_rng` to the
  same value before each half and compares 250 frames of hashed
  framebuffers. Keeping that check meaningful after moving the wander to
  Lua means a from-scratch Lua reimplementation of `hakoniwaos/src/
  creature.cpp`'s fixed-point arithmetic (1/16px sub-pixel positions,
  integer division, the dwell timer) that draws from `kf/rng.h` in *exactly*
  the same call count and order the C++ path does, every frame, including
  every RNG-consuming event (arrival at a target draws a dwell duration and
  a new target: three draws, in a fixed order). ADR 0042 already shows this
  category of bug is real and expensive even for logic Task 5 only
  *shared*, not reimplemented: two genuine divergences (frame 3's animation
  accumulator, the revive event's presenter-kept-walking bug), each found
  only because the parity check named the exact frame, and each costing a
  real debugging pass. A second independent implementation of the RNG-
  consuming half is a strictly harder version of the same problem.
- **A wrong migration is worse than no migration, and this project's own
  rule says so twice.** "A check that passes with the drawing deleted
  proves nothing" and "do not relax the comparison to a tolerance" (this
  plan's own text) both cut against attempting this under time pressure: a
  loosened check would hide exactly the class of bug it exists to catch,
  and a correct-looking but subtly-off port (a rounding difference, a
  missed RNG draw on one branch) could ship undetected if the check were
  weakened to accommodate it.
- **Nothing the owner asked to see requires it.** Lua already drives every
  pixel decision for Home under the new default: which sprite, where, which
  pose, the stat bars, the mess, the care guide, and all five care buttons
  all flow through `creature.lua`'s own declarations every frame, read
  through `creature.x()`/`.y()`/`.sprite()`/`.mirrored()`/`.frame()` — the
  read-only accessor boundary Task 5 built specifically so this kind of
  phased migration is safe to defer. The wander's fixed-point math is the
  one remaining piece still computed in C++ and merely *read* by the
  script, not authored by it.

This is exactly the shape of decision `hakoniwaos/src/creature.cpp` and
`kf_creature_presenter.h`'s own comments now say explicitly, so a future
reader does not find a comment claiming this already happened: genuinely
moving the wander into the script is correctly still open work, most
naturally landing alongside Task 7's `kf.screen()`/`screen:show()` (where
per-screen state ownership is already moving fully into Lua for navigation)
rather than as a rider on this task.

## The proof

- `scene_check` gained a sixth assertion block: declare a box, commit,
  simulate "another screen painted over the panel" with a raw
  `kf_fill_rect()` bypassing the scene entirely, then prove **first** that
  an ordinary commit with nothing declared as changed leaves those pixels
  alone (anti-vacuity: the repaint call below is not riding a diff that
  would have fired anyway), **then** that `kf_scene_force_repaint()` +
  commit restores the box's own pixels, **then** that the same object id
  still responds to an ordinary `kf_scene_set_pos()` afterward — object
  identity survived. Verified non-vacuous directly: commenting out
  `kf_scene_force_repaint()`'s one line and re-running turned this
  assertion red (`"kf_scene_force_repaint() repaints the box's area even
  though nothing about the box itself changed"`), then the line was
  restored.
- `screen_nav_check` reproduced failing under `KF_HOME_SCREEN=lua` before
  any code changed (confirming the gap is exactly what ADR 0042 described,
  not something else), and passes after both fixes — the repaint primitive
  and `run_screen_nav_check()`'s own Lua-boot addition — landed together.
  Neither fix alone was sufficient; each was checked in isolation.
- `ctest --test-dir build` is **44/44** in both `KF_HOME_SCREEN` states,
  including a genuinely fresh-configured default (`cmake -UKF_HOME_SCREEN`).
  `python3 tools/check_no_heap.py .` stays clean — the one line
  `kf_scene_force_repaint()` adds is a single boolean assignment.
- The two golden checksums, `headless_determinism` and `headless_fullscreen`,
  are unmoved — neither touches `simulator/src/pet/*` or
  `simulator/src/lvgl/*`, per ADR 0040/0042's own reasoning, which this task
  does not change.
- `run_creature_screen_budget_combination_check()`'s worst-frame figure is
  unchanged at **3 of 8** dirty rectangles (bound 5, analytical ceiling 6) --
  this task adds no new per-frame drawing, only a one-shot, explicitly-
  invoked full repaint on screen entry, which is outside that check's own
  steady-state measurement.
- The ESP-IDF cross-compile (`-DKF_PANEL=ili9341`) is clean, zero warnings,
  in both flag states: `KF_HOME_SCREEN=lua` (the new default) produces a
  671,872-byte firmware image (57% of the app partition free);
  `KF_HOME_SCREEN=cpp` produces 672,672 bytes (also 57% free). The ~800-byte
  difference is link-time dead-code elimination on whichever
  `kf_screen_nav.cpp` branch is not compiled in for a given `KF_HOME_SCREEN_
  LUA` setting; both screens are always compiled into either image
  regardless (unchanged from Task 5 — see `simulator/CMakeLists.txt`'s own
  comment on why).

## Not verified

No `KF_HOME_SCREEN=lua` build has been flashed and watched navigate Home →
Info → Home on real hardware — this fix is confirmed only by
`screen_nav_check` on the desktop build, the same distinction ADR 0040,
0041 and 0042 already draw for everything before them in this plan. The
wander staying in C++ is unverified in the sense that it was never
attempted, not in the sense that an attempt is known to be unsafe; ADR 0042
and this ADR are the record for whoever picks that work up next.
