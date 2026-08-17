# ADR 0061: A screen that is not showing holds no scene objects

**Status:** Accepted
**Date:** 2026-08-17

## Context

`KF_SCENE_MAX_OBJECTS` is 64 (`kf/scene.h`). Until this ADR, that was a
budget shared between **every screen a cartridge declares**, for the whole
life of the process: `kf.screen(name)` groups (ADR 0044) created their
objects when the script's top-level code ran, and a screen that was not
showing merely had its objects set invisible on the way out. Nothing was
ever released.

The demo cartridge had grown into that ceiling and out the other side:

| Screen | Objects |
|---|---|
| Home | 28 |
| Info | 8 |
| Settings | 24 |
| Play picker | 1 |
| **Total held, always** | **61 of 64** |

Three slots left for everything else — with the player looking at exactly
one screen, whose own share was 28. Two things had already been cut to fit
in what remained:

- **`nibble.lua` shipped with no score.** The first minigame wanted a round
  counter, a score readout and a result line. It got three objects total —
  a creature, a piece of food and a target — and pushed round/score/result
  out through `kf.log()` instead. A scoring game that never shows the
  player a number is not a finished game; it was a symptom of the budget,
  not a design choice, and its own source comment said so.
- **The play picker is one cramped line** (`L:QUICKPLAY R:NIBBLE B12 S3`)
  rather than two readable options, for the same reason.

The overflow was also found the hard way, twice — once at six objects and
again at four — because it surfaces as `kf.box()` raising *"the scene
already holds 64 objects"* partway through a script's top-level code,
taking the **whole cartridge** down at load rather than the one screen that
asked for too much.

Raising `KF_SCENE_MAX_OBJECTS` was the obvious alternative and was
rejected: 64 slots cost 14.5 KB of the ESP32-S3's **internal** RAM (that
header's own measurement — `sizeof(SceneObject)` is 232 bytes, and the
array is `.bss` in the scarce 512 KB pool, not PSRAM), a device running the
demo reports around 86 KB of internal heap free, and it does not scale:
every new screen would permanently spend more of it, in a platform whose
entire point is third parties shipping games it has never seen.

## Decision

**A `kf.screen()` group's objects exist only while that screen is
showing.** Navigating away releases them and frees their slots; navigating
back re-creates them. `KF_SCENE_MAX_OBJECTS` becomes a **per-screen**
budget — the largest single screen — rather than one shared between every
screen in the cartridge.

The demo cartridge went from **61 of 64 held permanently** to **30 while
Home is showing** (28 Home + 2 ungrouped error banners), and `nibble.lua`
got its status and result lines back.

### It costs no RAM because the declaration already lives in the script

The binding has always kept a full shadow copy of every object's declared
state in the object's own userdata (`LuaSceneObject`, ADR 0041) — it had
to, because Core's scene API is write-only past `kf_scene_bounds()`, so
the no-arg reads of the jQuery-style accessors had nowhere else to read
from. That shadow is exactly what survives a release:

- **Release** calls `kf_scene_remove()` on each of the group's objects and
  sets the userdata's `id` to 0. Zero is already the value every
  `kf_scene_*` setter treats as a safe no-op, so a script that keeps
  poking a released screen's objects updates their shadow harmlessly and
  sees it applied for real when that screen comes back.
- **Restore** re-adds each object from its shadow, in declaration order,
  replaying *every* field — position, layer, visibility, colours, frame,
  mirroring — not just the constructor arguments, and writes the new id
  back into the same userdata.

So a script's own handles keep working across navigation and **no script
had to change**. The state moves from Core's `.bss` (scarce internal RAM)
into the Lua arena (PSRAM, the pool with room), which is the right
direction.

### Declaration order is preserved on restore, deliberately

Creation order is `kf_scene_commit()`'s paint-order tie-break between
objects sharing a layer (`kf/scene.h`). Restoring out of order would
silently change which of two overlapping objects wins, so `restore_group()`
walks the group's object table front to back.

### `kf_scene_discard_removed()`

`kf_scene_remove()` deliberately does **not** free a slot until the next
commit — the commit still needs the object's `presented` rectangle to know
what area to erase. That is right everywhere except here: a screen switch
releases the outgoing screens and restores the incoming one in the same
call, with no commit in between, so releasing 28 slots and immediately
asking for 30 would hit the ceiling against slots that are removed but not
yet reclaimed, and the incoming screen would come up with objects missing.

`kf_scene_discard_removed()` frees them on the spot and, because nothing
will now ever erase those pixels, forces the next commit to repaint the
whole screen — which a screen switch was doing anyway
(`kf_scene_force_repaint()`). It does nothing at all when nothing is
removed, so a switch that released nothing does not pay for a full repaint.

Order in `set_active_group()` is therefore: **release every outgoing group
→ `kf_scene_discard_removed()` → restore the incoming one.**

### A screen that has never been shown has not taken a slot yet

Releasing on navigation alone would still leave one peak: a cartridge
declares every screen in its top-level code, so the moment that code
finishes, *all* of them are live and the peak is the sum again — 82
objects across four screens simply could not load.

So `screen:sprite()`/`:text()`/`:box()` on a group that is **not the active
screen** builds the userdata and its shadow but does not call
`kf_scene_add_*()` at all. The object materialises the first time its
screen is shown. Only the showing screen's declarations take slots as they
run, and the peak becomes the largest single screen rather than the sum.

`screen:background()` is deferred the same way and for the same reason:
there is one scene-wide background slot, and a screen declaring itself in
the background would otherwise paint its colour over the one the player is
actually looking at until the next navigation put it back.

### `kf.active_screen()` now reports the navigation registry's name

Previously the active-screen name was written only when a switch matched a
registered `kf.screen()` group. Under `KF_HOME_SCREEN=cpp` Home is a real
registered screen with **no Lua group**, so that name could never be
`"home"` in that build — it kept reporting whichever *other* screen was
last activated, the entire time the player was looking at Home.

That was not cosmetic. The play picker opens on
`kf.active_screen() == "home"`, so under `KF_HOME_SCREEN=cpp` pressing UP
on Home did nothing at all and **PLAY was a dead button** — a regression
introduced when Task 5 of the Nibble plan moved UP out of
`kf_home_screen_input.cpp` and into the script.

`kf_lua_scene_activate_screen()` now takes the registry's own name
alongside the index and records it unconditionally, before the "is there a
group here" early return. `kf_screen_nav_init()` records `"home"` the same
way at boot, so the answer is right from the first frame in both builds
rather than only after the first navigation. The alternative considered was
an `#ifndef` around the picker's UP gate; it would have left
`kf.active_screen()` lying in one of the two builds, which is a worse thing
to leave behind than a dead button.

**`KF_HOME_SCREEN=cpp` had not compiled at all since ADR 0045**, which is
how the dead button went unnoticed: nobody could build the binary it was
dead in. `kf_screen_nav.cpp` included `kf_lua_home_screen.h` only under
`#ifdef KF_HOME_SCREEN_LUA`, but that header also declares
`kf_lua_info_screen_frame()`, which `kf_screen_nav_init()` registers for
Info in **both** builds — Info has been Lua-declared regardless of which
Home is compiled in since ADR 0045. The definition was always linked in
(`kf_lua_home_screen.cpp` is in the source list unconditionally); only the
declaration was hidden, so the flag died with
`'kf_lua_info_screen_frame': undeclared identifier`. Moving that one
include out of the guard is the whole fix, and the flag builds and links
again.

No CI job builds `KF_HOME_SCREEN=cpp` (`.github/workflows/ci.yml` builds
the default only), which is why a build-breaking change survived. Whether
that flag earns a CI job — or whether the C++ Home has outlived its purpose
now that ADR 0043 made `lua` the default and ADR 0045 moved Info off LVGL —
is a scope question left open here rather than answered.

The release pass also runs when the incoming index has no Lua group — under
`cpp`, navigating back to Home still has to make Info and Settings let go.

### What is *not* released

Objects created through the bare `kf.sprite()`/`kf.text()`/`kf.box()` calls
belong to no screen and are never released. That is deliberate and the two
error banners are why: a banner created by `kf_lua_home_screen_init()` /
`kf_lua_settings_screen_init()` that vanished on navigation would take the
error with it. `run_settings_screen_check()` asserts this split explicitly —
2 ungrouped objects before any script runs, 30 once Home has declared — so
the distinction stays visible rather than living only in prose.

## Consequences

- `KF_SCENE_MAX_OBJECTS` is a per-screen budget. A cartridge can declare
  far more than 64 objects in total; one screen still cannot exceed 64.
- Overflow moved from load time to activation, and from fatal to logged:
  `restore_group()` names the screen and how many of its objects it could
  not restore, instead of `kf.box()` raising and taking the cartridge down.
  A screen that genuinely wants more than 64 now fails visibly on that
  screen rather than preventing the game from starting.
- Object ids churn on every navigation (`kf_scene_id` is a `uint16_t`
  monotonic counter). ADR 0040's "wrapping needs thousands of add/remove
  cycles" is now closer than it was — roughly 2,300 Home entries rather
  than the effectively-never it described. It remains harmless in practice:
  the binding writes the new id into the same userdata on every restore, so
  no stale id is ever held on the Lua side, and `:remove()` is guarded by
  the userdata's own `removed` flag rather than by id comparison. Worth
  revisiting if a reason to widen the type appears; not worth widening on
  its own today.
- The play picker's cramped single line is no longer a budget constraint.
  It is left as it is because nobody has redesigned the picker, not because
  there is no room.

## Proof

- `screen_group_check` declares **82 objects across four screens** in a
  64-slot scene. The script loading at all is the headline assertion; a
  build where screens still shared one budget cannot get through its
  top-level code. It then asserts the live object count is **1** after
  load, **40** while each of the two deliberately-oversized screens is
  showing (two screens whose objects could never coexist), and **1** again
  on return.
- The same check asserts screen `"a"` comes back **byte-for-byte
  identical** — an FNV-1a hash of the whole framebuffer, equal to the very
  first time it was shown — after its objects have been released and
  re-created twice over. That is what proves the shadow copy is a
  *complete* record of an object's declared state rather than nearly
  complete; the object count alone would not.
- `settings_screen_check` asserts 2 live objects before any script runs
  (the ungrouped banners) and 30 once Home has declared, and prints the
  number it actually got on a mismatch so the constant can be re-measured
  rather than guessed.
- Full suite on Linux GCC and on Windows MSVC with
  `WARNINGS_AS_ERRORS=ON` — see the commit for the counts.
  `check_no_heap` / `check_no_float` unaffected:
  `kf_scene_discard_removed()` allocates nothing, and the group bookkeeping
  moved *out* of `.bss` into Lua's arena.
