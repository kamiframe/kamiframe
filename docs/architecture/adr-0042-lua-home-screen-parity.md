# ADR 0042: The Lua home screen, and the parity check that judges it

## Context

Task 5 of the Lua game-layer plan (`docs/superpowers/plans/2026-08-12-lua-
game-layer.md`) makes `examples/creature_demo/creature.lua` declare the
whole home screen — background, creature with pose and facing, mess, three
stat bars, the care guide — behind a build flag, `KF_HOME_SCREEN=cpp|lua`,
defaulting to `cpp`. The point of the task is not the flag; it is
`run_lua_vs_cpp_screen_check()`, which drives a fresh pet through an
identical frame-by-frame timeline through each implementation in turn and
asserts the two paint byte-identical framebuffers on every single frame.
That check is what this ADR records, because it did its job: it found two
real bugs before it ever passed, and one architectural gap it cannot fix
that this ADR names instead of hiding.

## Decision

### The wander stays in C++, shared through a new presenter module

The plan is explicit that the wander (`kf_creature_update`, pose/sprite-
name resolution) moves to Lua in Task 6, not this one — moving it while the
renderer is also new would make an A/B diff impossible to attribute. For
Task 5, that means both screen implementations need the *identical* wander
result on any given frame, computed once. `kf_creature_screen.cpp` used to
own that computation privately; it is now `simulator/src/pet/kf_creature_
presenter.{h,cpp}`, extracted verbatim (not rewritten) so the extraction
itself carries no behaviour risk, with `kf_creature_screen.cpp` and the new
`kf_lua_home_screen.cpp` both calling into it. The five hardware care
buttons (Task 6 of the pet-screen plan) made the identical move, into
`kf_home_screen_input.h` — without it, `KF_HOME_SCREEN=lua` would silently
turn Feed/Play/Rest/Bath/Flush into dead buttons, since they were coupled
to `kf_creature_screen.cpp`'s own per-frame function before this task.

### One script, two build modes: a runtime flag, not two scripts

`creature.lua` has to behave differently depending on whether IT is
drawing Home (`KF_HOME_SCREEN=lua`) or the C++ screen is (`cpp`, narration
only) — declaring scene objects under `cpp` would collide with the C++
screen's own objects in the one shared scene. `kf.home_screen_active()`
gates the script's own top-level declarations and its per-frame updates.
It is a **runtime** flag (`kf_lua_port_set_home_screen_active()`), seeded
from the build's `KF_HOME_SCREEN_LUA` compile define but overridable at
runtime — deliberately, because the parity check needs ONE compiled binary
to drive *both* halves of the comparison regardless of which default that
binary's own `KF_HOME_SCREEN` build setting picked. `kf_lua_port_init()`
does **not** reset this flag on every call (an earlier version did, and it
broke the check — see "Found by the check" below): the flag has to survive
from whenever it was last set through to the script's own top-level chunk
running, and `kf_lua_port_init()` runs that chunk as part of the same call.

### `kf_scene_set_frame()` gets a Lua binding: `obj:frame([n])`

Task 2 added `kf_scene_set_frame()` to Core; Task 3's binding never wired a
Lua method to it. Found while building this task's demo screen, which
needs to declare a multi-frame idle pose's animation cursor and had no way
to. Sprite-only, no-arg-read/arg-write, the same convention as every other
object property.

### The error banner is scoped to the Lua-owns-Home case only

Task 5's brief asks for a one-line banner when `on_frame` has been disabled
by an error. It is **not** wired into `kf_creature_screen.cpp`: that
screen's own correctness never depends on Lua (the demo script only
narrates under `KF_HOME_SCREEN=cpp`), so a narration error there leaves
nothing on the *picture* broken to announce. When Lua owns Home, a disabled
`on_frame` freezes the scene with no other indication, which is what the
banner is for. `kf_lua_port_last_error()` keeps a copy of the message
`kf_lua_port_frame()`'s own `KF_LOGE` already logs, so the panel and the
log say the same thing.

## Found by the check

`run_lua_vs_cpp_screen_check()` (`--verify-screen-parity`, 250 frames:
egg bob, wander, mess appearing and flushed, a button-triggered reaction,
death and the shrine, revival clearing it) diverged twice before it ever
passed.

**Frame 3: the animation accumulator, wiped by its own "fresh pose" reset.**
`kf_creature_presenter_advance()`'s "did the resolved sprite name just
change" check fires on the very first call regardless of `dt_ms`, and
resets the animation accumulator as part of "treat this as a fresh pose."
`kf_creature_screen_enter()` (the C++ path) makes that first call with
`dt_ms == 0` — a free, harmless reset before any real time has elapsed. The
Lua path's first-ever call was its first *real* `dt_ms == 33` frame, so the
reset wiped out the 33ms it had just accumulated, a one-time timing offset
the C++ path never has. Fixed by giving `kf_lua_home_screen_init()` the
identical `dt_ms == 0` priming call.

**Frame 230 (the revive event): the presenter kept walking while dead.**
`kf_creature_screen_frame()`'s death branch returns before ever calling
`kf_creature_presenter_advance()`. An early version of the Lua wiring
advanced the presenter every frame regardless of `pet->dead`, so during the
20 dead frames in the check's own timeline, the Lua half's creature kept
wandering (and consuming `kf_rng` draws) somewhere the screen never shows,
while the C++ half's stayed frozen. By the revive event the two had walked
to different places and drawn different numbers of RNG values from the
same stream, and nothing after that point could agree again. Fixed by
adding the identical death gate to `kf_lua_home_screen_frame()` — and,
because this is exactly the kind of behaviour that belongs to a *specific*
screen driver rather than to Lua glue in general, the presenter-advance
call was moved out of `kf_lua_port_frame()` (generic, shared by every
script this codebase loads, including proof scripts with no pet session at
all) and into `kf_lua_home_screen.cpp` itself, which is the file that
actually knows a pet session exists.

**Proof the check can fail, not just that it can pass.** The Lua screen's
creature was moved one pixel off the C++ position (`creature.x() + 1`),
the header regenerated, and the check re-run: it failed at frame 0, naming
the exact hash mismatch. Reverted before committing.

## Known gap: Home re-entry under `KF_HOME_SCREEN=lua`

`kf_creature_screen_enter()` forces a full-panel repaint on every entry to
Home via `kf_scene_reset()`, which is what stops Info's LVGL pixels from
showing through rows the C++ screen's own diff would otherwise consider
unchanged (the black-trail bug, ADR 0017). `kf_scene_reset()` also
discards every scene object and invalidates every id — fine for the C++
screen, which re-declares everything from scratch on every entry, but
`creature.lua` declares its objects **once**, at script load, and holds
their handles for the life of the process. There is no `kf/scene.h`
primitive that forces a full repaint *without* destroying object identity.
Confirmed directly: `screen_nav_check` (built with `KF_HOME_SCREEN=lua`)
fails with row 280 still holding Info's leftover pixels after a Home ->
Info -> Home cycle. Does not affect the parity check (which never touches
Info) or a fresh boot (Home is active before anything else has painted a
pixel), and does not affect the default `KF_HOME_SCREEN=cpp` build at all —
`screen_nav_check` passes there.

**Closed by ADR 0043**, ahead of Task 7 rather than inside it: the plan's
own end note reordered this fix to run before the `KF_HOME_SCREEN` default
flip, since flipping the default on top of this gap would have shipped
broken Home navigation as the out-of-the-box build. `kf/scene.h` gained
`kf_scene_force_repaint()` — a full repaint that keeps every object's id,
unlike `kf_scene_reset()` — and `kf_screen_nav.cpp`'s `load()` now calls it
on every re-entry to Lua-drawn Home. Task 7's `kf.screen()`/`screen:show()`
is still the right home for *navigation itself* moving into the script; it
no longer also has to be the place this particular repaint gap gets fixed.

## Firmware size

ESP32-S3, `-DKF_PANEL=ili9341`, both `KF_HOME_SCREEN` values build clean,
zero warnings. See the Task 5 report
(`.superpowers/sdd/lua-task-5-report.md`) for the measured sizes on this
worktree's ESP-IDF checkout.

## Not verified

No `KF_HOME_SCREEN=lua` build has rendered on real hardware — the ESP-IDF
cross-compile is clean in both flag states but "compiles, links, reports a
plausible size" is not "has been flashed and watched," the same
distinction ADR 0040 and ADR 0041 already draw. The Home re-entry gap
above is confirmed only on desktop (`screen_nav_check`); its effect on a
real device's KFDBG-driven MENU/B navigation has not been observed
directly.

## Superseded in part

**"`KF_HOME_SCREEN=cpp|lua`, defaulting to `cpp`"** (Context, above) is
superseded by ADR 0043: `KF_HOME_SCREEN` now defaults to `lua` on both
build systems, closed by ADR 0043 exactly as this document's own "Closed by
ADR 0043" note (under "Known gap") anticipated. References elsewhere in
this ADR to "the default `KF_HOME_SCREEN=cpp` build" describe the state as
it stood before that flip, which is when this ADR's own findings apply, and
are left as written.
