# ADR 0021: Life stages and evolution

**Status:** Accepted, 2026-08-05
**Reversal cost:** Medium. The core mechanic (`kf/pet.h`/`pet.cpp`) is about
150 new lines behind a save-format version bump -- deleting it means
reverting those files and accepting that any save written under v2 resets
to a fresh pet on the next build, the same well-defined fallback ADR 0015
already established for any incompatible version. The Lua binding
(`pet.stage()`/`pet.teen_form()`/`pet.adult_branch()`) and the pet screen's
placeholder blob are each small, independent additions on top and can be
reverted separately without touching the core mechanic.

## Requirement

ADR 0015 named life stages and evolution as an explicit, deferred design
surface: real content this project would need before it could honestly call
itself "the quintessential v-pet experience." With the hardware HAL
scaffolding (ADR 0020) built and parts on order, this was the next slice --
and, unlike every earlier slice in this project, one where Chris asked to be
directly involved in the gameplay/creative decisions rather than have a
sensible default picked for him: *"I also want to be more heavily involved
in decision making on how the evolution/life stage work looks as I imagine
this is tying all into my demo software with my trademarked characters,"*
and, more broadly, *"involved in more gameplay decisions other than
evolution etc."* That is now a standing instruction for this project, not a
one-time ask for this slice alone.

Accordingly, this slice did not start with code. It started with a design
options document (`11-life-stages-evolution-design-options.md`, delivered
separately) walking through the genre's real option space -- Tamagotchi-
style (a few fixed stages, one branch point near the end), Digimon-style (a
branch at every stage, a real tree), and no-evolution (continuous growth,
no distinct forms) -- and asking Chris to choose, rather than picking one
and presenting it as done. His answers, verbatim from that exchange, are
what this ADR implements:

- **Structure:** classic Tamagotchi-style. Egg -> Baby -> Child -> Teen ->
  Adult. *"This is the quintessential v-pet experience that started it
  all, and needs to be nailed first."*
- **Branching:** 3 possible teen types (chosen by care quality during
  Child), each branching to 2 adult forms (chosen by care quality during
  Teen) -- 6 adult forms total.
- **Egg behaviour:** "just a timer, no care needed (classic)" -- no need
  decay at all while an egg.
- **Care-quality metric:** "average over the whole stage (truer to intent,
  more to build)" -- explicitly not a snapshot taken at the moment of
  transition, and explicitly accepted that this needs new persisted state
  and a save-format version bump.
- **Stage timing:** "I'll decide exact numbers later -- just make it
  configurable." `kf_pet_default_config()`'s values are illustrative, not a
  tuning recommendation.
- **Character art:** "simplistic blobs for now to get the systems
  working" -- real character work (names, backstories, art) waits for his
  designer; nothing here invents any of it.

## Decision: the mechanism is generic, the meaning is not this file's job

`kf/pet.h`'s own header comment states the boundary this slice was built
to hold: *"WHAT A STAGE OR FORM NUMBER MEANS IS NOT THIS FILE'S BUSINESS."*
`stage` is a life-cycle position; `teen_form` and `adult_branch` are plain
0-based indices identifying which branch was taken, nothing more. Core does
not know or care what a "teen_form == 1" pet looks like, is called, or acts
like -- that is real creative content, and it lives in the Lua cartridge
layer (`kf_lua_port.cpp`'s `pet.*` binding) or above. This is the same
core-generic / Lua-cartridge split this codebase already uses for hunger,
happiness, and energy, extended to cover the whole life-cycle question, and
it is what lets Chris's actual character work start later without touching
this file again -- and what keeps his trademarked IP out of the Apache-2.0
generic engine entirely.

**Stage progression is a closed-form, bounded loop, never a per-second
simulation.** `kf_pet_advance()` already computed decay as one calculation
per call rather than simulating elapsed time second by second (ADR 0015) --
critical for correctly handling multi-day or multi-week offline
fast-forward in roughly constant work relative to elapsed time. Adding
stages could easily have broken that property by turning "how long was the
device off" into "how many seconds do I need to individually step through
to find every stage boundary." Instead, `kf_pet_advance()`'s loop is bounded
by the number of *remaining life stages* -- at most 4 (Egg, Baby, Child,
Teen; Adult is terminal and handled after the loop) -- never by
`elapsed_seconds` itself. An offline gap of three days or three years,
advancing through every remaining stage in one call, costs the same
handful of internal steps. Each iteration computes how much of the current
stage's duration is left, applies decay for exactly that segment (or the
remainder of `elapsed_seconds`, whichever is smaller) via the existing
`apply_stage_segment()`, and only calls `advance_to_next_stage()` once the
stage's full duration has actually been credited -- so a single call
spanning egg-through-adult decides every branch from the care actually
accumulated during that stage's real duration, never blurred across
stages. This is verified directly: `pet_stage_evolution_check`'s check 5
crosses all four real transitions in one `kf_pet_advance()` call and
confirms the correct stage, both branch indices, and the correct leftover
time credited to the terminal Adult stage.

**The care-quality average is a left-Riemann-sum integral, not a
mathematically exact one.** Chris asked for "an average over the whole
stage," which needs new persisted state: `stage_elapsed_seconds` (how long
the pet has been in its current stage) and `care_integral_mp_seconds` (a
running numerator, reset at every stage transition). Each segment of
elapsed time is weighted by the need average *at the start* of that
segment, before that segment's own decay is applied --
`care_integral_mp_seconds += average_before_mp * segment` -- then the
branch is picked by dividing by `stage_elapsed_seconds` at the transition.
This is a deliberate, documented approximation, the same category of
accepted trade-off `kf_pet_session.cpp`'s own live-tick batching already
makes (ADR 0016) and for the identical reason: an exact continuous integral
needs either float (which this project avoids everywhere, for exactly the
drift-over-weeks-of-uptime reason `kf/pet.h`'s own header comment gives) or
many small steps (which defeats the point of a closed-form calculation).
It delivers what Chris asked for faithfully; it does not claim lab-grade
numerical precision beyond that. Only Child and Teen accumulate this at
all -- `stage_feeds_a_branch_choice()` -- since Baby always leads to
exactly one Child (nothing to choose) and Egg has no needs to average in
the first place.

**Branch selection is equal-width bands over that average**, the simplest
defensible default: `(average_mp * branch_count) / (KF_PET_MILLIPERCENT_MAX
+ 1)`, clamped, band 0 the neediest/worst-cared-for band and
`branch_count - 1` the best. Not exposed as config yet -- like every other
number in this file, a candidate for later if equal bands turn out to be
the wrong shape once pets are actually being raised, but there is no real
pet history yet to tune it against.

**The tree SHAPE is a compile-time constant, not config.**
`KF_PET_TEEN_FORM_COUNT` (3) and `KF_PET_ADULT_BRANCH_COUNT` (2) are
`#define`s, unlike decay rates and stage durations. Changing how many
branches exist means the save format and the Lua-side branch tables change
together -- a structural decision, not a per-pet tuning value.

**The save format bumps to version 2**, hand-packed byte by byte exactly
like every field before it (`put_u8`/`put_u32`/`put_i64`/`put_u64` and
matching `get_*` -- never a raw struct write, since struct layout is not
something GCC and MSVC, both in this project's CI matrix, are obliged to
agree on). `KF_PET_SAVE_BYTES` grows from 22 to 41: `stage` (1 byte),
`teen_form` (1 byte), `adult_branch` (1 byte), `stage_elapsed_seconds` (8
bytes), `care_integral_mp_seconds` (8 bytes) on top of the original 22. A
version-1 save (from before life stages existed) is refused and falls back
to a fresh pet -- no migration code, an explicit, accepted cost, the exact
behaviour ADR 0015 already established for any unrecognised version.

## A real bug found and fixed

`kf_pet_load_and_advance()`'s original code, from before this slice,
returned `KF_ERR_INVALID` directly when `unpack()` failed, contradicting
its own header comment's documented promise of a fresh-pet fallback on
anything unreadable. That contract had never actually been exercised --
every save this project had ever written was version 1, so `unpack()` had
never failed in practice. Bumping the save version to 2 makes it a live
path: `kf_pet_session_init()` calls `kf_pet_load_and_advance()` and
`KF_ASSERT(result == KF_OK, ...)`'s on the result, so *any* device carrying
a version-1 save built before this slice ships would have hit the broken
branch and crashed on boot the moment it upgraded, rather than starting a
fresh egg as documented. Fixed by changing the `unpack()`-failure branch to
call `kf_pet_init(state)` instead of returning an error, matching what the
header comment always claimed. `pet_stage_evolution_check`'s check 7
exercises this exact path directly -- writing a synthetic version-1 save
and a synthetic wrong-size save via `kf_store_write()`, then confirming
`kf_pet_load_and_advance()` returns `KF_OK` with a fresh egg in both cases,
not an error -- the same discipline ADR 0019 and ADR 0020 established of
surfacing every real bug found during a slice rather than silently patching
it.

## Verified

- Full clean rebuild (`rm -rf build`, reconfigure, rebuild every object
  file including vendored dependencies from source), zero warnings beyond
  the two pre-existing, unrelated ones already accepted before this slice
  (an SDL3 CMake policy notice, and `loslib.c`'s `tmpnam` linker warning).
- `tools/check_no_heap.py`: clean.
- All 11 `ctest` targets pass, 5 repeated full-suite runs in a row with no
  flakiness, including the two pre-existing tests
  (`pet_offline_ageing_check`, `lua_pet_binding_check`) that needed fixing
  once Egg's no-decay behaviour was implemented (see "What changed in
  existing tests" below) and the three new/extended ones this slice adds:
  - **`pet_stage_evolution_check`** (new): the egg's no-decay guarantee
    holding right up to the last second before hatching; a single
    `kf_pet_advance()` call correctly continuing past a stage boundary
    within the same call rather than stopping dead at it; the care
    accumulator resetting cleanly at a stage transition rather than
    leaking Baby's own (branch-irrelevant) care into Child's average;
    `select_branch()`'s equal-band mapping landing in the correct band for
    three known, decay-isolated care levels (10%/50%/90% -> bands
    0/1/2); one call spanning all four real stage transitions at once,
    checked against known bands for both teen_form and adult_branch;
    Adult's terminal behaviour (a further `advance()` call keeps
    accumulating time without picking another stage or branch); the v2
    save format round-tripping every new field with a pinned wall clock
    for determinism; and the version-incompatible/wrong-size save
    fallback bug above, directly.
  - **`lua_pet_binding_check`** (extended): a third stage proving
    `pet.stage()`/`pet.teen_form()`/`pet.adult_branch()`, packing all
    three into one integer the same way the C++ side computes it
    independently from the live session state, and comparing -- proves
    the *string* `pet.stage()` hands back round-trips to the correct enum
    value, not just that some string comes out.
  - **`pet_screen_check`**'s golden checksum, updated twice this slice: once
    for the new blob's rendered pixels, once more after fixing the
    caption label's position (see "What changed in existing tests" below).
- The demo creature script (`kf_lua_creature_check`) exercises the new
  stage-transition announcements for real, not just in isolation: its
  existing 10-day decay-then-recover lifecycle already crosses every
  stage boundary from Egg through Adult, and ran clean with no Lua error
  through all of it, producing exactly the expected sequence of
  transition and branch-decided log lines (confirmed by direct log
  inspection during this slice, not just the pass/fail check).
- A real Xvfb + `xdotool` + ImageMagick `import` screenshot of the running
  simulator (the same tooling ADR 0017 used) confirmed the pet screen's
  blob renders and is legible, and caught the caption-position bug
  described below before it shipped.

### What changed in existing tests

Two pre-existing tests broke as a direct, expected consequence of Egg's new
no-decay behaviour, not a regression in what they were checking:

- **`pet_offline_ageing_check`**: its "decays by exactly the configured
  rate over one hour" check advanced a fresh pet by exactly 3600 seconds --
  which is also `kf_pet_default_config()`'s default `egg_duration_seconds`,
  so the whole elapsed time was now consumed by an egg stage that does not
  decay, leaving nothing to observe. Fixed by setting `state.stage =
  KF_PET_STAGE_BABY` directly before advancing, isolating decay-math
  testing from egg-stage behaviour, which has its own dedicated coverage
  in `pet_stage_evolution_check` now.
- **`lua_pet_binding_check`**'s original decay-proof stage accumulated only
  about 40 simulated seconds (1200 frames at roughly 33ms each) -- far
  short of the 3600-second egg duration, so hunger never moved and the
  "decayed visibly" check failed. Fixed by using a larger synthetic
  per-frame duration for that stage's loop (3500ms/frame, same frame
  count), so cumulative simulated time comfortably clears the egg stage
  with margin, without changing what the stage is actually proving (the
  binding's FFI correctness, not egg timing).

### A real layout bug caught by an actual screenshot, not assumed

The pet screen's blob (see "The pet screen blob" below) was first built
with its caption label positioned via `lv_obj_align_to(label, blob,
LV_ALIGN_OUT_BOTTOM_MID, ...)`. An Xvfb screenshot taken while building
this showed the caption landing far below the blob, not under it --
because that call ran before the blob's first real resize
(`update_blob()`'s `lv_obj_set_size()`), anchoring against LVGL's default
~100px object size instead of the actual small blob. Fixed by giving the
caption a fixed position computed from `kBlobDiameterMax` (the compile-time
known worst case) instead of a live anchor to the blob's current size --
confirmed correct with a second screenshot before locking in the updated
golden checksum. Named here because this is exactly the category of bug
ADR 0017 already flagged screenshots as being necessary to catch (a
correctly-passing checksum test proves *determinism*, not that the layout
looks right) -- this slice hit that same case for real, not hypothetically.

## The pet screen blob

`kf_pet_screen.cpp` now draws a placeholder blob in the screen's top-right
corner: a plain `lv_obj_create` styled with `LV_RADIUS_CIRCLE` (the
standard LVGL way to draw a circle with no custom drawing code or image
asset), sized and coloured from the live `kf_pet_state`. Diameter grows
with life-cycle stage (8px at Egg up to 24px at Adult -- "grows up" is the
simplest visual anyone reading the screen understands with zero
explanation); colour is a fixed placeholder per stage before any branch is
decided, then switches to a colour indexed by `teen_form` (3 colours) or by
`(teen_form, adult_branch)` (6 colours, each staying in the same hue family
as its parent teen colour) once one has been. A caption label under it
spells out the stage name and, once decided, the raw branch indices (e.g.
"teen 1", "adult 2-0") -- numbers, not names, for the same reason
`kf/pet.h`'s header comment gives: naming these branches is real creative
content that does not exist yet.

This is explicitly placeholder, per Chris's own framing ("simplistic blobs
for now to get the systems working"), not a design proposal for what his
actual characters should look like -- the point is proving the
stage/evolution state is visibly distinguishable on screen using existing
widget primitives, nothing more.

## The `pet.*` Lua additions

`pet.stage()` returns the life-cycle position as a lowercase string --
`"egg"`, `"baby"`, `"child"`, `"teen"`, `"adult"` -- matching the existing
convention the demo creature script already uses for its own need bands
(string-keyed, not raw enum integers), rather than leaking Core's `enum
kf_pet_stage` layout into the cartridge layer. `pet.teen_form()` and
`pet.adult_branch()` return the raw 0-based indices Core stores, exactly as
opaque as they are in C++ -- meaningless (always 0) before the branch point
that sets them, so a script checking them should check `pet.stage()` first.

The demo creature script (`kf_lua_demo_creature_script.h`) now reacts to
stage transitions with the same "announce only on the frame it actually
changes" pattern it already used for need bands: a short, generic line per
transition ("the egg cracks open," "growing bigger every day," etc.) plus
the raw branch index once one is decided ("care during childhood settled
into teen form 2"). This is placeholder demo content, the same status as
every need-band message that was already in this file -- not Chris's
actual creature's voice, and it does not invent names for the branches it
announces, only reports the index Core hands it.

## Accepted cost

Stage durations and decay rates are illustrative
(`kf_pet_default_config()`: egg 1 hour, baby 1 day, child 2 days, teen 3
days -- adult by about a week), explicitly per Chris's own "I'll decide
exact numbers later, just make it configurable." Nothing about the
mechanism depends on these specific values; they exist so the systems have
something concrete to run against today.

`select_branch()`'s equal-band mapping is not tunable per pet yet, and
neither is the care-quality metric itself (an average, not Chris's other
named option of time-in-critical-band or explicit mistake-counting, which
remain deferred, real design surfaces of their own -- see ADR 0015's
"deliberately not built" list, now narrowed by exactly this one item).

The pet screen blob's size/colour tables are placeholder and will be
entirely replaced once real character art exists -- nothing about their
current values is meant to survive that.

## Later

- Real character content: names, backstories, and art for the egg through
  every adult form, once Chris's designer is involved. This is the whole
  reason the core/cartridge boundary in this slice was drawn where it was.
- Making stage durations and the branch-selection bands actual `kf_pet_
  config` fields exposed to a settings surface, once there is a real pet
  history to tune them against.
- Time-in-critical-band or explicit care-mistake tracking, if the simple
  average ever turns out to be the wrong feel once pets are actually being
  raised -- both remain real, separate features, not ruled out by this
  slice's choice of the simple average to start.
- A DS3231 RTC on real hardware (ADR 0020's still-open gap): stage timers
  rely on the same wall clock offline fast-forward already depends on, so
  closing that gap benefits life stages the identical way it benefits
  ordinary decay.

## Superseded in part

**"The pet screen blob"** describes `kf_pet_screen.cpp` as the live Home
screen; it no longer is. Since ADR 0021's own later work
(creature-on-screen) and ADR 0044/0045, Home is `kf_creature_screen.cpp` or
a `kf.screen("home")` Lua group, and draws the real sprite this section's
"real character content... once Chris's designer is involved" looked
forward to, not a placeholder blob. `kf_pet_screen.cpp`'s blob still exists
and still compiles under `-DKF_ENABLE_LVGL=ON` as `pet_screen_check`'s
golden-checksum subject, but nothing in a running build shows it.
