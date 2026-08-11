# ADR 0044: `kf.screen()` — named object groups over one shared scene, and a registry that learns names

**Status:** Accepted
**Date:** 2026-08-13

## Context

Task 1 of `docs/superpowers/plans/2026-08-13-screens-clock-sleep.md` is the
groundwork for a Lua-drawn Info screen and a new Settings screen (Tasks 2 and
4): it changes no pixels on its own, and every later task depends on the
mechanism it introduces. Two things this plan established shape the design:

- **There is one retained scene, not one per screen.** `KF_SCENE_MAX_OBJECTS`
  is 64 (`kf/scene.h`), and `hakoniwaos/src/scene.cpp` holds a single
  file-static table for the whole process. `examples/creature_demo/
  creature.lua` declares 24 objects for Home, plus 1 for the error banner
  (`kf_error_banner_create()`) = 25; Info as text objects is budgeted at 8;
  Settings at 14. That is 47 of 64 — real headroom, but not obviously so,
  which is why this task's check asserts a live-object count against a named
  constant (`kf_scene_live_object_count()`, added below) rather than assuming
  one. Raising `KF_SCENE_MAX_OBJECTS` was explicitly ruled out: the object
  array lands in `.data`, so 64→96 would cost roughly 7 KB of flash *and*
  7 KB of RAM.
- **Navigation stays in C++.** Three things read the old two-entry registry
  that cannot read a Lua table: `sdl_debug_window.cpp`'s "Next Screen"
  button, its screen-name readout, and `kf_screen_nav_wants_lvgl()`, which
  both desktop backends use to decide whether to pump LVGL at all. Moving
  screen switching into the script would also put it behind the
  `lua_pcall` boundary that exists specifically to stop a bad script from
  breaking the device (ADR 0014).

Given both constraints, screens had to become a Lua-facing *grouping* over the
one shared scene, not a scene of their own, with the actual "which screen is
showing" decision staying in `kf_screen_nav.cpp`.

## Decision

### `kf.screen(name)` returns a named group; the registry learns names

`kf_screen_nav.h`'s `kScreenCount = 2` and its two hardcoded `g_screens[]`
assignments become a fixed-capacity table
(`KF_SCREEN_NAV_MAX_SCREENS = 8` — a handheld with one MENU button, not a
desktop with a taskbar) filled by:

```c
int kf_screen_nav_register(const char *name, void (*update)(uint32_t dt_ms));
const char *kf_screen_nav_name(int index);   /* "?" out of range */
int kf_screen_nav_count(void);
void kf_screen_nav_show(int index);
```

`kf_screen_nav_register()` is **create-or-fetch by name**, not create-only:
the first registration of a name wins its `update` callback, every later
call under the same name is a pure fetch that returns the existing index
unchanged. This is what lets `kf_screen_nav_init()` register `"home"` with
its real per-frame update function (`kf_lua_home_screen_frame` or
`kf_creature_screen_frame`, matching this build's `KF_HOME_SCREEN`)
*before* `creature.lua` ever runs, and `creature.lua`'s own
`local home = kf.screen("home")` safely fetch that exact same index moments
later — with `update = nullptr` — without silently blanking out what
`kf_screen_nav_init()` already set. Home registers first (still index 0), so
`B`-jumps-home and `kf_screen_nav_debug_home()` keep meaning what they mean.

`kf.screen(name)` mirrors this on the Lua side: it searches a fixed-capacity
array of `LuaScreenGroup` (`sdk/lua/kf_lua_scene.cpp` — **an array, not a
Lua table**, the same reasoning `kf_lua_scene.cpp` already applies to scene
objects, capped at `KF_SCENE_MAX_OBJECTS` for a single group's own id list
so it can never be the binding that overflows first), and either returns a
handle to an existing group or creates one, registering it with the C++
registry on creation. Calling `kf.screen("home")` twice in one script is
therefore not an error and does not make a second screen.

### The single-owner rule, and the ADR 0042/0043 bug it exists to prevent

`kf_screen_nav_show(int index)` is **the one function** that switches which
screen is showing. Both the MENU/B edge inside `kf_screen_nav_frame()` and
`screen:show()` (the Lua method) call it and nothing else —
`screen:show()`'s entire body is `kf_screen_nav_show(group.registry_index);`.

This matters because ADR 0042 documented, and ADR 0043 fixed, exactly the
failure shape two independent switching paths produce: stale pixels from
the previous screen surviving under the new one, on some transition orders
and not others, because two code paths each partly decide "what's showing"
and disagree about ordering. `kf_screen_nav_show()` does three things, in
this order, for *any* valid index:

1. `kf_lua_scene_activate_screen(index)` — hides every OTHER `kf.screen()`
   group's objects, shows this index's group (if one is registered under
   it), re-applies its stored background colour if it ever set one, then
   `kf_scene_force_repaint()` and an immediate `kf_scene_commit()` if
   anything has ever been declared. A no-op, changing nothing, if no group
   is registered under `index` — Info, still LVGL as of this task.
2. If the screen has an LVGL root: `lv_obj_invalidate()` + `lv_screen_load()`
   (unchanged from before this task).
3. Otherwise, if `index == 0` (Home): Home's own entry hook
   (`kf_lua_home_screen_enter()` or `kf_creature_screen_enter()`, matching
   `KF_HOME_SCREEN`) — for whatever it does beyond scene-group bookkeeping
   (the error banner, the creature presenter's animation cursor). Any other
   non-LVGL screen needs nothing further: step 1 already repainted it.

`run_screen_group_check()`'s step 5 is the check this design exists to pass:
it drives the identical A→B and B→A transitions once through `screen:show()`
and once through `kf_screen_nav_debug_advance()`/`_debug_home()` (the same
edges a real MENU/B press produces) and asserts byte-identical framebuffers.
If the two paths ever disagreed, this is where it would show up.

### Why the registry can't just `#include` the Lua binding (and vice versa)

`simulator/CMakeLists.txt` links `kamiframe_lvgl_port` (which holds
`kf_screen_nav.cpp`) against `kamiframe_lua_port` (which holds
`kf_lua_scene.cpp`) — not the other way around, on purpose, stated
explicitly in that file's own comment. `kf_screen_nav.cpp` including
`kf_lua_scene.h` is therefore fine (the allowed direction); the reverse
would create the exact link cycle that comment forbids.

But `kf.screen(name)` and `screen:show()` — both defined in
`kf_lua_scene.cpp` — need to call `kf_screen_nav_register()` and
`kf_screen_nav_show()`, which live on the other side of that same boundary.
The fix is a function-pointer boundary instead of a header dependency:

```c
/* kf_lua_scene.h -- declared independently, matching the two functions'
 * shapes without naming them or the library that defines them. */
typedef int (*kf_screen_nav_register_fn)(const char *, void (*)(uint32_t));
typedef void (*kf_screen_nav_show_fn)(int);
void kf_lua_scene_set_screen_nav(kf_screen_nav_register_fn, kf_screen_nav_show_fn);
```

`kf_screen_nav_install_lua_hooks()` (`kf_screen_nav.cpp`) wires the two real
functions in — called once, as `kf_screen_nav_init()`'s first step for the
interactive build, and once from `headless_main.cpp`'s `main()` before every
headless check, since several existing checks (`run_lua_creature_check()`,
`run_lua_vs_cpp_screen_check()`'s Lua half) load `creature.lua` without ever
calling `kf_screen_nav_init()` — and `creature.lua`'s `kf.screen("home")`
call needs the hooks regardless of which check loads it. Idempotent, so
installing it twice (once globally, once again inside a check that *does*
call `kf_screen_nav_init()`) is harmless. Before the hooks are installed,
`kf.screen()`/`screen:show()` raise a named Lua error rather than calling
through a null pointer.

### Ungrouped objects, and why the error banner must stay one

`kf.sprite()`/`kf.text()`/`kf.box()` (bare, no screen) are unchanged: they
create an object belonging to no group, which `kf_lua_scene_activate_screen()`
never touches regardless of which screen is showing. This is deliberate, not
an oversight — `kf_error_banner_create()` calls `kf_scene_add_text()`
directly from C++, with no screen at all, and a banner that vanished the
moment a script switched screens would hide the exact error that caused the
switch to look wrong in the first place.

### Per-screen backgrounds

`kf_scene_set_background_color()` is scene-wide and singular — there is only
one background slot, shared by every screen. `screen:background(color)`
therefore does two things: it applies the colour **immediately**, exactly
like the pre-existing bare `kf.background()` (this is what makes Home's
background correct at boot, before `kf_screen_nav_show()` is ever explicitly
called for it — `g_active` defaults to 0, matching Home's index, the moment
`kf.screen("home"):background(bg)` runs during script load); and it records
the colour on the group, so `kf_lua_scene_activate_screen()` can **re-apply**
it the next time navigation switches back to that screen, after some other
screen's own `:background()` call has overwritten the one shared slot in
between. Both writes set the exact same value, so there is nothing for the
two call sites to disagree about.

A screen that never calls `screen:background()` inherits whatever colour is
currently set — documented rather than defaulted, because a silently-black
Settings screen and a silently-inherited one are both defensible and only
one of them is what the script's author meant.

### `kf_scene_live_object_count()`

Added to `kf/scene.h`/`hakoniwaos/src/scene.cpp` — a small O(64) scan over
`g_objects` counting entries that are `in_use && !removed`. A debug/test
accessor only (no interactive caller needs it); it exists so
`run_screen_group_check()` can pin the object count multiple `kf.screen()`
groups declare over the one shared scene against a named constant, per the
plan's risk 5 ("the 65th `kf.text()` returns 0 and raises — at script load,
so it fails loudly, but nobody had measured the count before this").

## Consequences

- `sdl_debug_window.cpp`'s `screen_name()` (a hardcoded `0 = Home, 1 = Info,
  "?"` switch) is deleted. The readout now calls `kf_screen_nav_name()`
  directly, correct by construction for any number of registered screens
  rather than by remembering to extend a switch.
- `examples/creature_demo/creature.lua`'s Home block is a receiver change
  only: `kf.sprite/text/box/background()` become
  `home:sprite/text/box/background()` where `local home =
  kf.screen("home")`. Same layout, same object count, same declaration
  order — `screen_nav_check`'s golden checksum (`ac44bb9819809bea`) and
  `screen_parity_check` both stayed green unchanged, which is the whole
  point: this task changes no pixels.
- `kf_screen_nav.h`'s stale header comment (*"Home (kf_pet_screen.cpp)"*,
  wrong since Task 4 of the Lua game-layer plan and contradicted by the same
  file's own later paragraph) is corrected.
- New desktop test: `run_screen_group_check()` /
  `--verify-screen-groups` / ctest `screen_group_check`. Desktop suite:
  46/46 (45 baseline — 44 plus `clock_check`, landed concurrently by
  another task on this same branch — plus this one).
- ESP32 (`-DKF_PANEL=ili9341`): builds clean, zero warnings.

## Not verified

No Lua screen group has rendered on real hardware. This task's checks are
desktop/headless only — `run_screen_group_check()` proves the mechanism in
isolation (two screens, no pet, no LVGL), and `screen_nav_check`/
`screen_parity_check` prove Home's existing behaviour is unchanged under the
new mechanism, but nothing here has run on the ESP32-S3 target beyond a
clean cross-compile.
