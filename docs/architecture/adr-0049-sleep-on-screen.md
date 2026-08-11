# ADR 0049: Sleep on screen — the Lua binding, the wander freeze, the
# tuck-in interaction, and the futon

**Status:** Accepted
**Date:** 2026-08-11

## Context

Task 7 of the screens/clock/sleep plan
(`docs/superpowers/plans/2026-08-13-screens-clock-sleep.md`) is the game's
half of sleep. Task 6 (ADR 0048) landed `kf_pet_state::asleep`,
`kf_creature_pose_for()` returning `KF_CREATURE_POSE_SLEEPING`, and the
save-format bump — entirely Core-side, nothing exposed to Lua. This task
binds that state to the SDK surface third-party developers read
(`pet.asleep()`), makes the demo creature actually show it (the sleeping
pose, a frozen wander, a deliberate-wake interaction), and adds the
optional tuck-in decoration the care-loop spec names: *"while drowsy, the
creature can be brought to bedding and tucked in."*

**A scope change landed mid-task.** The original brief said "there is no
bedding art — draw it with scene boxes and label it clearly as placeholder
art, do NOT generate art." Partway through implementation, Chris authorized
art generation for a single generic futon sprite and specified its
behaviour (a subtle wiggle, a slow ZZZ blink) via the operator. That
authorization is recorded here because it reverses an explicit instruction
this ADR would otherwise have to explain away. The plain-box design was
built, verified working, then replaced before landing — see "What was
built and discarded" below.

## Decision

### 1. `pet.asleep()` — a plain boolean, no richer sub-state

`sdk/lua/kf_lua_port.cpp` adds `pet.asleep()` to the existing `pet.*` table,
returning `kf_pet_session_state()->asleep` directly — the same shape as
`pet.sick()`/`pet.dead()` next to it. ADR 0048 already decided Core carries
no drowsy sub-state ("settling the creature into bed is the game layer's
decoration"), so there is exactly one question a script can ask, matching
the audience constraint: a WordPress-developer-turned-pet-scripter reads
one boolean, not an enum they have to look up.

### 2. `pet.wake()` and `kf_pet_wake()` — waking costs happiness, kept small, no-op twice over

`kf/pet.h` gains `kf_pet_wake(kf_pet_state *state, const kf_pet_config
*config)`, alongside feed/play/rest/bath/flush, with the identical
`if (state->dead) return;` shape those four already share, plus a second
guard those four do not need: `if (!state->asleep) return;` — waking an
already-awake creature costs nothing, because nothing happened. It
subtracts a new config field, `wake_happiness_cost_mp` (default 5000 —
5%, illustrative like every other figure in `kf_pet_default_config()`),
using the same underflow-safe floor-at-zero subtraction `apply_decay()`
already uses, then sets `asleep = false`.

**Deliberately does not touch `care_actions_taken`.** Unlike feed/play/
rest/bath, waking is not something the creature has an opinion about —
`kf_pet_reaction_to()` is never consulted, so there is no reaction to
record and no reason to start the presenter's reaction-hold window
(`kf_creature_presenter.cpp`'s `care_actions_taken` watch). Pose precedence
already puts `asleep` above the held reaction (ADR 0048, decision 10) for
the identical reason: nothing about waking should flash a happy/objecting
pose the creature never performed.

`kf_pet_session_wake()` (`simulator/src/pet/kf_pet_session.{h,cpp}`) wraps
it against the one live session, matching `kf_pet_session_flush()`'s shape
exactly. `pet.wake()` (`sdk/lua/kf_lua_port.cpp`) binds it with no argument
and no return value, same as `pet.flush()`.

### 3. The wander freezes while asleep — in the presenter, not Core

The plan's own warning: *"A sleeping creature that keeps walking is the
obvious bug and the parity check will not catch it."* `screen_parity_check`
hashes one committed frame; it has no notion of "did this stay put across
many frames", which is exactly the shape of this bug.

`kf_creature_update()` (`hakoniwaos/src/creature.cpp`) owns wander
position/target/dwell and has no knowledge of `kf_pet_state` at all — it
takes a `kf_creature*` and a field rect, nothing else. The freeze therefore
lives one level up, in `kf_creature_presenter_advance()`
(`simulator/src/pet/kf_creature_presenter.cpp`), which already branches on
`pet->stage == KF_PET_STAGE_EGG` to choose "bob in place" over "wander" —
sleep gets a third branch, `else if (pet->asleep)`, that calls
`kf_creature_tick_anim()` only and skips `kf_creature_update()` entirely.
**Skipped, not called with `dt_ms = 0`**: `dwell_ms`/`reaction_hold_ms` must
not tick down either, so a creature that falls asleep mid-dwell or
mid-reaction resumes exactly there on waking, not part-way through a
countdown that kept running while nothing was drawn.

This is simulator-layer code (`simulator/src/pet/`), not
`hakoniwaos/` — matching CLAUDE.md's "pet simulation stays in Core, how
sleep looks is the game's" split. The wander itself still lives in C++
(ADR 0043's own decision, restated in `kf_creature_presenter.h`'s header
comment — moving it into Lua needs a bit-exact `kf/rng.h` reimplementation
that is real, separate work), so freezing it lives next to it rather than
in `creature.lua`.

### 4. `kf.button(name)` — a new, minimal, scoped input binding

Nothing before this task let a Lua script read a raw button press.
Home's five existing actions (feed/play/rest/bath/flush) are wired
entirely in C++ (`kf_home_screen_input.cpp`), by design — Settings' own
header comment explains why a *shared cross-screen* button registry would
be a mistake (a screen's buttons firing while a different screen is
active). That reasoning is about sharing, not about reading raw state per
se: `kf_app_buttons_pressed()` is already read directly, scoped to
whichever screen's own per-frame function is running, by every C++ input
handler in this codebase. `kf.button(name)` does the identical thing,
just reachable from Lua now, and only from inside whichever screen's own
`on_*_frame()` happens to be executing — there is no shared registry to
get wrong.

Lowercase string names (`"up"`/`"down"`/`"left"`/`"right"`/`"a"`/`"b"`/
`"menu"`), matching `pet.stage()`'s own lowercase-string convention rather
than exposing `kf_button`'s integer bitmask — the audience constraint
again: a third-party author should never need to know this project has an
enum for buttons. An unrecognised name raises via `luaL_error()` (a script
bug, caught at the call site) rather than silently returning false.

This is the one piece of this task that is not sleep-specific: it is a
generically useful addition (any custom interaction a third-party game
wants, that does not fit the five built-in care actions) that sleep
happened to need first, for the tuck-in interaction below.

### 5. Care buttons are inert against a sleeping creature

`kf_home_screen_input.cpp`'s `kf_home_screen_handle_care_buttons()` gains
one guard at its top: `if (pet->asleep) { return; }`. Feed/play/rest/
bath/flush on a sleeping creature is not a scenario the spec describes and
reads as an obvious bug once named — a script that presses A to feed a
sleeping creature should not also silently wake it via a side effect wired
somewhere else. Waking is its own explicit interaction (decision 2 above),
read separately, in Lua, from the same button.

### 6. The tuck-in interaction — decorative only, Core's timing untouched

The spec: *"Being put to bed is the optional interaction. The drowsy state
is the signal that this is available: while drowsy, the creature can be
brought to bedding and tucked in. It is a nicety, not a duty."* ADR 0048
already decided Core carries no drowsy sub-state and no "settle early"
mechanism — *"removing the drowsy/settle interaction from the mechanism
... is what unblocked this task in the first place."* So tucking in changes
nothing about **when** the creature actually falls asleep; it only changes
what the screen shows in the meantime.

`creature.lua` tracks this entirely as local closure state — `tucked_in`
(bool), `bed_x`/`bed_y` (the position captured at the moment of tucking
in), `was_asleep` (to notice the wake edge). `drowsy` is computed fresh
every frame: `kf.clock_set() and kf.hour() == 21 and not asleep and not
tucked_in`. 21 (9 PM, the hour before Core's own 22:00 night start) is a
literal in the script, not read from Core — this is display-only and never
feeds back into the actual sleep decision, so duplicating it is honest
rather than risky; `kf/clock.h`'s night-window function is not part of the
Lua surface and should not become one just to save one integer literal.

Pressing B while drowsy captures the creature's current position as
`bed_x`/`bed_y` and sets `tucked_in = true`. **B, not a new physical
button.** All seven buttons are already claimed on Home except B: A/UP/
DOWN/LEFT/RIGHT are the five care actions, MENU cycles screens globally,
and B is `kf_screen_nav.cpp`'s global "go home" — a true no-op when Home is
already active (`go_home()`'s own `if (g_active != 0u)` guard), so reusing
it here on Home costs nothing and collides with nothing.

**Waking puts the bedding away.** The spec's own words: *"it wakes, gets
itself out of bed, and puts the bedding away."* `creature.lua` watches for
the `was_asleep && !asleep` edge (Core's own wake, whether natural morning
or the deliberate `pet.wake()` from decision 2) and resets
`tucked_in = false` there — never as a player action, matching *"the
player is never required to do anything to end sleep."*

**Rendering collapses to two states, not three.** Whether a creature is
wide awake or asleep-but-not-tucked-in (fell asleep on its own, "plopped
down where it stands"), `creature.sprite()` already resolves the correct
pose — `kf_creature_pose_for()` returns `KF_CREATURE_POSE_SLEEPING`
whenever `pet.asleep()` is true, with no help needed from the script. The
only branch `creature.lua` needs is `tucked_in` vs not: tucked in shows
the futon and hides the body; not tucked in shows the body with whatever
pose Core/the presenter already resolved, asleep or awake alike.

### 7. The futon: one generic sprite, looked up by literal name, animated in Lua

**Corrected mid-task**: the pack entry name is `futon_idle_s`, not the
bare `futon` first proposed — matching the existing precedent for
non-creature scenery, `shrine_idle_s`. `tools/character_manifest.toml`
gains a `[stages.futon]` table modelled on `[stages.shrine]` (`kind =
"scenery"`, one state, one direction) and the PNG lands at
`examples/creature_demo/sprites/futon_idle_s_01.png`, both landed
separately from this task's own commit (see "Not verified" below) — this
task only writes the code that references the name.

**One sprite for every stage, looked up by literal name**, exactly like
`shrine_idle_s` in the same file — `home:sprite("futon_idle_s")`, never
through `kf_creature_sprite_name()`'s stage/pose/direction resolver.
Scenery has no pose or direction to resolve; routing it through that
resolver would be inventing variants that do not exist.

**The wiggle** mirrors `kf_creature_presenter.cpp`'s `egg_bob_offset_y()`
— the same integer triangle wave (period 3000 ms, amplitude ±2 px) —
duplicated in `creature.lua` rather than shared, because that C++ helper
is `static` to its own translation unit and Lua cannot see it. Kept
integer-only (`//` floor division, matching the rest of this script's
existing arithmetic idioms) in the same spirit as `hakoniwaos/`'s own
no-float rule, even though `check_no_heap.py` does not enforce that on
Lua and nothing else in the toolchain would catch a stray `float` here.

**The ZZZ blink** is font text, not art — `Z` is a real glyph at `0x5A`
(`hakoniwaos/src/font_data.h`), so `"ZZZ"` renders directly; the font's
uppercase-only limitation was already priced into every other screen this
plan touches. Chosen period, named per the operator's own instruction to
report the exact value: **visible 2000 ms of every 3000 ms cycle** (`
kZzzCyclePeriodMs = 3000`, `kZzzVisibleMs = 2000`) — mostly on, a brief
pause, never a fast flicker, on the same 3000 ms cadence as the wiggle for
a single coherent "breathing" period rather than two unrelated timers.
Both numbers are feel, to be judged on the board, same as every other
"Chris judges this" figure in this plan.

The same `zzz` text object is reused for the drowsy cue (static, shown
while drowsy-not-tucked) and the tucked-in blink (animated) — one scene
object doing both jobs rather than two, since they are never shown at the
same time.

### 8. Adults, and any stage with no sleeping sprite: unchanged from ADR 0048, restated here

No new decision — ADR 0048 already made both calls and this task changes
neither:

- **The egg does not sleep** (`apply_stage_segment()`'s early return for
  `KF_PET_STAGE_EGG`, ADR 0048 decision 9). `creature.lua` needs no
  egg-specific sleep branch because `pet.asleep()` is simply always false
  for an egg-stage pet; nothing here re-decides that.
- **Adults have no sleeping art** (18 shipped sprites cover baby/child/
  teen0-3 only, per the plan's finding 1). `kf_creature_sprite_name()`
  does not special-case ADULT the way it special-cases EGG — an adult that
  falls asleep resolves a name like `adult00_sleeping_s` that the pack
  does not contain, and `resolve_and_declare()`'s existing missing-sprite
  fallback (`kf/scene.h`'s placeholder box, `KF_RGB(255, 0, 128)`,
  logged once per name) is what shows. **This is the explicit, commented
  decision for this gap**, not a silent fallthrough: it is the same
  fallback every other missing sprite in this pack already gets (verified
  live below, for `futon_idle_s` itself, since the art had not landed at
  the time this task's own tests were run) rather than a special adult-
  sleep rule invented here. Building real adult sleeping art, or an
  adult-specific fallback, is an art decision for Chris, not a code
  decision this task should make silently.

### 9. Dirty-rect budget: measured, not assumed

`run_sleep_screen_check()` (`simulator/src/headless/headless_main.cpp`)
measures the worst dirty-rect count over 300 real-cadence frames in two
shapes:

| Shape | Worst-case dirty rects | `KF_MAX_DIRTY_RECTS` |
|---|---|---|
| Asleep, not tucked in (frozen, single-frame sprite) | **0** | 8 |
| Tucked in (futon wiggle + ZZZ blink, body hidden) | **1** | 8 |

The plain-asleep case measuring exactly 0 is expected, not a bug: nothing
in that scene changes at all once the creature has settled and the fill
bars are held constant by this check's own setup (it never calls
`kf_pet_session_frame()`, so needs do not decay during the measurement).
The tucked-in case measuring 1, not 2, matches the same "erase and draw
overlap and get unioned" reasoning `kf_creature_screen_check`'s own header
comment gives for the ordinary wander: the futon and the ZZZ text sit
close enough together (`zzz` is drawn at `bed_x + 30, bed_y - 10`, well
within the futon's own 48×48 footprint) that `kf_fb_mark_dirty()` unions
their two rectangles into one. **Both figures are comfortably under
budget** — nowhere near the point where `KF_MAX_DIRTY_RECTS` (8) would
collapse the frame to a single full-screen box.

## What was built and discarded

The original brief's placeholder-box design (two `kf.box()` calls forming
a simple two-tone bed, positioned under the creature every frame) was
built, its own version of this task's tests passed, and its own worst-case
dirty-rect count was measured before Chris authorized real art mid-task
and the operator relayed the futon's exact spec. Recorded here rather than
silently deleted from history: the box version measured comparably (low
single-digit worst-case dirty rects), so the art swap was a pure asset
change, not a design change — the scene-object shape (one sprite/box for
the bed, one text object for ZZZ), the tuck-in interaction, the wiggle and
the blink all carried over unchanged. The final code references
`futon_idle_s` throughout; no trace of the box version remains in the
shipped script.

## The proof

- `ctest --test-dir build` — **48/48** (the prior 47 plus the new
  `sleep_screen_check`).
- `python3 tools/check_no_heap.py .` — `core is heap-free (36 files
  scanned)` — unchanged file count, since this task added no new
  `hakoniwaos/` source files (only new functions inside existing
  `pet.cpp`/`pet.h`).
- No `float`/`double` in any `hakoniwaos/` file this task touched
  (`pet.cpp`, `pet.h`, `creature.cpp`), checked by hand.
- The ESP-IDF cross-compile (`-DKF_PANEL=ili9341`, incremental, every file
  this task touched recompiled) is clean — zero warnings anywhere in
  project code. Firmware image: 519,888 bytes (67% of the app partition
  free; ADR 0048 recorded 513,536 bytes before this task, so sleep-on-
  screen's own code costs roughly 6.3 KB).

### Non-vacuity: every new assertion broken and watched fail, then restored

Per this project's own rule that a passing test is not evidence it tests
anything, every new assertion in `run_sleep_screen_check()` was proven
non-vacuous by breaking the mechanism it depends on, rebuilding, observing
the specific failure, then restoring and reconfirming green. Two of these
(marked below) were caught being genuinely vacuous on the FIRST breakage
attempt and had to be rewritten before they proved anything:

| Assertion(s) | Breakage introduced | Failure observed |
|---|---|---|
| `pet.asleep()` reads Core's state (2 assertions) | `lua_pet_asleep()` hardcoded to always push `false` | `FAILED: pet.asleep() reads true while state->asleep is true` (plus cascading failures below, since every later assertion in this check depends on the binding being honest) |
| `kf_pet_wake()`'s no-op guards (2 assertions) | Disabled the `if (state->dead \|\| !state->asleep) return;` guard entirely | `FAILED: waking an already-awake creature is a no-op -- no happiness cost for a wake that never happened` and `FAILED: waking a dead creature is a no-op, same as every other care action against a dead pet` |
| `kf_pet_wake()`'s happiness cost | Hardcoded the subtracted `cost` to `0u` | `FAILED: kf_pet_wake() costs exactly wake_happiness_cost_mp happiness, nothing more` (and the button-wired equivalent below, since both read the same function) |
| The wander freezes while asleep (**the plan's own named risk**) | Disabled the presenter's `else if (pet->asleep)` branch (`false && pet->asleep`) | `FAILED: asleep: the creature does not wander -- settled exactly where it stood the instant it fell asleep, even across 300 more frames of real elapsed time` |
| The freeze is specific to `asleep`, not a permanent stop | Forced the presenter to ALWAYS take the frozen branch (`else if (true)`) | `FAILED: once woken, the wander resumes -- the freeze was specific to being asleep, not a permanent stop` (this breakage is deliberately different from the row above, to prove the two assertions are not accidentally testing the same thing) |
| The resolved sprite is the sleeping pose | Disabled `kf_creature_pose_for()`'s `if (pet->asleep) return KF_CREATURE_POSE_SLEEPING;` | `FAILED: asleep: creature.sprite() resolves to a *_sleeping_* pose sprite` |
| Care buttons are inert while asleep | Disabled `kf_home_screen_handle_care_buttons()`'s asleep guard | `FAILED: asleep: flush (and every other care button) does nothing -- kf_home_screen_input.cpp's own asleep gate` |
| Pressing A while asleep wakes the creature, through the real script | Disabled `creature.lua`'s `if asleep and kf.button("a") then pet.wake() ... end` | `FAILED: pressing A while asleep wakes the creature, through creature.lua's own kf.button("a")/pet.wake() call` (plus the happiness-cost and wander-resumes assertions, cascading) |
| `kf.button()` itself | Hardcoded the binding to always push `false`, ignoring the bitmask read | Same cascading failures as the row above -- correctly indistinguishable from creature.lua's own wiring being broken, since kf.button() IS that wiring's only input |
| The futon shows at the tuck-in position, **and** the body is genuinely hidden (2 sub-assertions) | **Caught vacuous on the first attempt.** The original single assertion sampled a pixel at `bed_x+24, bed_y+24` -- exactly where the creature's own (still-visible, in the broken run) body sprite was ALSO being drawn, since `bed_x/bed_y` are captured from the same live position the body follows. With `kf.button()` broken (tuck-in never fires), this single assertion stayed GREEN. Rewritten into two: sample the body's own new position after 60 more frames of invisible wander and require background there, AND sample the original `bed_x/bed_y` and require it stays non-background. Re-broke `kf.button()` and confirmed BOTH now fail: `FAILED: tucked in: the creature's own body sprite is NOT drawn at its live, invisible wander position -- body:hide() is real` and `FAILED: tucked in: the futon is still showing at the FIXED bed position, unmoved by the body's own hidden wandering` |
| The wiggle/ZZZ blink are live, not frozen | Same `kf.button()` breakage (tuck-in never fires, so nothing ever animates) | `FAILED: tucked in: a step that changes nothing else still dirties something -- the futon wiggle / ZZZ blink are live, not frozen decoration` |
| Waking puts the bedding away (`tucked_in` resets) | **Also caught vacuous on the first attempt**, same root cause as the futon-position row: the original assertion compared `kf_creature_presenter_x()/y()` before and after a second tuck-in attempt, but that raw position drifts every frame regardless of `tucked_in`'s value (it is gated on Core's `asleep`, not this decorative flag), so it read "different" whether or not the reset actually happened. Disabling `creature.lua`'s `if was_asleep and not asleep then tucked_in = false end` left this comparison GREEN. Rewritten to sample the PIXEL at the ORIGINAL `bed_x/bed_y` instead: re-broke the reset line and confirmed `FAILED: waking put the bedding away -- the ORIGINAL bed position is vacated, which only happens if tucked_in actually reset and the futon moved to a fresh spot` |

The two vacuous-on-first-attempt rows are the reason this ADR states them
explicitly rather than only listing the final, correct assertions: both
were caught specifically because they were deliberately broken and
re-verified as this rule requires, not because a review noticed the flaw
by inspection.

## Not verified

**The futon's own art had not landed when this task's tests were run.**
`kf_assets_get("futon_idle_s")` returns `null` against the shipped pack as
of this commit, and the check output confirms the expected fallback:

```
[E] scene  scene object 5: sprite 'futon_idle_s' not found in the mounted
    pack -- drawing a placeholder
```

`resolve_sprite()`'s existing missing-sprite handling (`hakoniwaos/src/
scene.cpp`) draws the same `KF_RGB(255, 0, 128)` placeholder box every
other missing sprite in this pack gets — no crash, no blank screen, logged
once. `run_sleep_screen_check()`'s pixel assertions were written to be
robust to this: they check "differs from background" / "equals
background", never an exact colour, so they remain valid unchanged once
the real art lands. **What has not been re-verified**: that the real
`futon_idle_s_01.png`, once packed, actually looks like a futon at 48×48
under colour-key transparency, and that `[stages.futon]`'s manifest entry
(landing separately, per this task's own division of labour with the
operator) round-trips through `tools/kf_character_manifest.py` without
tripping the three hardcoded stage-key tuples that file's own review
found. Re-run `ctest --test-dir build -R sleep_screen_check` after the art
and manifest entry land; a green result there is not itself proof the art
LOOKS right, only that nothing broke structurally — Chris judging it on
the board (the plan's own requirement for this task) is what closes that.

**Nothing about sleep-on-screen has run on hardware.** Per the plan's own
"Who has to be at the bench" table: no board needed to build this task,
but Chris needs to judge bedtime feel — the drowsy hour, the tuck-in
button, the wiggle amplitude, and the ZZZ blink cadence are all feel
questions this task built a specific, named, reportable value for and
none of them derived from anything but the operator's stated intent.

**The attention signal (Task 8) is unstarted** and, per its own
requirement, must never fire while asleep — `pet.asleep()` landing here is
exactly what that task will read to enforce it.
