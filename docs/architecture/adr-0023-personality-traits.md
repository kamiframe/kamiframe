# ADR 0023: Personality traits

**Status:** Accepted, 2026-08-06
**Reversal cost:** Medium, same shape as ADR 0021's. The core mechanic
(`kf/pet.h`/`pet.cpp`) is behind a save-format version bump (v2 -> v3) --
deleting it means reverting those files and accepting that any save
written under v3 resets to a fresh pet on the next build, the same
well-defined fallback ADR 0015 and ADR 0021 already established for any
incompatible version. The Lua binding (`pet.base_trait()`/`pet.dominant_
care_trait()`) and the Info screen's new "Personality" section are each
small, independent additions on top and can be reverted separately without
touching the core mechanic.

## Requirement

ADR 0021 named personality traits as one of three remaining deferred design
surfaces (alongside care-mistake tracking and the random event scheduler).
With life stages and evolution shipped, this was the next slice Chris
picked: *"Nice, let's go with personality traits system next."*

Same standing process as ADR 0021 (Chris: *"I also want to be more heavily
involved in decision making"*), so this did not start with code either. It
started with a design-options document
(`15-personality-traits-design-options.md`) walking through the genre's
real option space -- assigned-at-birth-and-mostly-flavour, care-history-
derived, mechanically-active, or no-new-state narration -- followed by
Chris's answers, then a concrete-plan document
(`16-personality-traits-concrete-plan.md`) proposing the actual mechanism
before any implementation began.

Chris's answers, restated:

- **Two layers.** A small set of random "base" traits, rolled once, mostly
  flavour. On top of that, "major" traits derived from care history -- the
  real behaviour driver: *"there is a base set of simple traits that are
  random every time, but the major swingers in how the pet behaves are due
  to how they are cared for."*
- **Care-derived, from per-need averages, not action counts (yet).**
  *"I would say track each need average separately to determine traits. If
  more differentiation is needed for later evolutions, then start also
  accounting for total care actions on top of that."* Action counts are
  explicitly a later addition, not built in this slice.
- **Fully separate from evolution.** *"I think the traits should affect
  more of how they speak/act personality wise and not so much affect their
  evolution."* No effect on `teen_form`/`adult_branch`.
- **Whole-life, but recency-weighted.** *"Whole-life should count for
  something, but recent care should be weighted high enough that it can
  swing things later if needed."*
- **The base-trait table is a fine starting point.** *"Those look good as
  starting traits. I assume we can add more or change these later?"* --
  yes: see "Where this lives" below for why that is a one-file Lua edit,
  not a Core change.

One thing worth flagging plainly, separate from the mechanism itself:
Chris also pasted a character bible
(`14-character-bible-v1.md`) in the same message, referencing it as "naming
I've worked on in another conversation." That document is real and
valuable, but it names EVOLUTION forms (the tsukumogami roster -- Ippomaru,
Chokimaru, and so on, for `teen_form`/`adult_branch`), not personality
traits. It does not contain trait names, and this slice does not use it for
that reason -- the placeholder names below (Chatty/Quiet/Curious/... and
Foodie/Playful/Chill) are still exactly what doc 16 proposed and Chris
approved. Separately, the character bible's own roster shape -- four verb
families (Cut/Hold/Mark/Go) with a variable 1-3 adults each, plus an
11th orphan branch (Hokorimaru) reachable only through total neglect --
does not match what ADR 0021 actually shipped (`KF_PET_TEEN_FORM_COUNT` =
3, `KF_PET_ADULT_BRANCH_COUNT` = 2, both compile-time constants, uniform
2-per-family, care-quality-only branching). That mismatch is real,
substantial, separate follow-up work -- reconciling the evolution tree's
shape with the character bible -- and is deliberately NOT part of this
slice, which only touches personality.

## Decision: a periodic-halving accumulator, not a hard reset or a true
continuous EMA

The mechanism doc 16 proposed and this slice built:

**Base traits: one random pick from a small fixed table, rolled once at
`kf_pet_init()`.** Via `kf_rng_below()` (`kf/rng.h`), not `kf_entropy()`
directly -- `kf/rng.h`'s own header comment names it as exactly the tool
for this: "the game-visible random number generator... anything the pet or
a Lua game observes comes from here," seeded once from the entropy HAL at
boot (`kf/app.cpp`) and otherwise pure Core, no HAL calls, keeping
`kf_pet_init()`'s existing trivially-unit-testable contract intact. This is
a small correction versus doc 16's own wording ("the same RNG path
`kf_entropy()` already provides") -- `kf_rng_below()` is the actual
existing tool for game-visible randomness; `kf_entropy()` seeds it once and
is never called again. Same determinism story as everything else `kf_rng`
touches: headless tests pin it with a fixed seed and get a repeatable
`base_trait`, proven directly in `run_pet_personality_check()`'s first
check. Stored as `uint8_t base_trait`, `[0, KF_PET_BASE_TRAIT_COUNT)`,
fixed for the pet's whole life, never touched again after roll -- proven by
the same check advancing a month and feeding the pet, then confirming
`base_trait` didn't move.

**Care-derived traits: three whole-life, periodic-halving accumulators,
never reset at a stage transition.** `hunger_integral_mp_seconds`,
`happiness_integral_mp_seconds`, `energy_integral_mp_seconds` -- each works
exactly like `care_integral_mp_seconds` already does (need value weighted
by elapsed seconds, weighted by the value BEFORE that segment's own decay,
summed), except three separate running totals per need instead of one
blended average, and accumulated whenever `stage != EGG` (starting at
Baby, not gated to Child/Teen the way the evolution accumulator is).

The actual recency-weighting mechanism is what doc 16 flagged as the one
open technical question, and where this slice made a real design call: a
true continuous exponential decay needs either floating point (which this
project avoids everywhere, for the drift reasons `kf/pet.h`'s own header
comment gives) or a per-second loop (which defeats the entire point of
`kf_pet_advance()`'s closed-form, bounded-by-stages-not-by-elapsed-seconds
design -- see this file's and `pet.cpp`'s own header comments). Instead:
a single shared `care_recency_window_seconds` counter tracks how many
seconds have accumulated toward the next halving; every time it reaches
`kf_pet_config`'s new `personality_recency_half_life_seconds`, all three
accumulators are right-shifted once (halved) before the triggering
segment's own contribution is added at full weight, and the leftover
remainder carries forward. This is one division and at most one bit-shift
per `kf_pet_advance()` segment -- closed-form regardless of how large the
elapsed time is (a three-day offline gap costs the same handful of
operations as three years, capped defensively at a 63-bit shift, which
already flushes any accumulator to 0, the mathematically correct answer for
"so much time passed that nothing old survives"). This satisfies Chris's
"whole-life should count for something, but recent care should be weighted
high enough that it can swing things later" without floating point or a
per-second loop -- proven directly in `run_pet_personality_check()`'s
boundary-crossing check (999s of a 1000s half-life accumulates at full
weight; the 1000th second halves the prior total before adding its own
contribution).

**Which need is "dominant" is a pure query, not stored.**
`kf_pet_dominant_care_trait()` compares the three accumulators directly
(valid because all three accumulate over the identical span of time, one
call at a time, via the same `accumulate_personality()` -- there is no
per-need normalisation to do), returning 0/1/2 for hunger/happiness/energy.
Ties -- including a fresh pet whose accumulators are all still zero,
e.g. an egg -- resolve to 0 (hunger), a defined, boring default rather than
an unspecified one.

## Where this lives

Same Core-numeric/Lua-named split ADR 0021 already established for
`teen_form`/`adult_branch`, extended to personality: `base_trait` and
`kf_pet_dominant_care_trait()`'s return value are opaque 0-based indices,
nothing more. `pet.base_trait()`/`pet.dominant_care_trait()`
(`kf_lua_port.cpp`) hand those indices to the cartridge layer; a Lua script
maps index -> name -> actual line. This is directly what makes Chris's "I
assume we can add more or change these later?" true without a Core
rebuild: today's six placeholder base traits (Chatty/Quiet/Curious/
Sleepyhead/Bold/Shy) and three placeholder care-derived labels (Foodie/
Playful/Chill) are one array in one Lua file, exactly like `teen_form`'s
real names will be once Chris's character work lands there instead.

`KF_PET_BASE_TRAIT_COUNT` (6) is a compile-time constant, not config -- the
same "shape vs. tuning value" distinction `KF_PET_TEEN_FORM_COUNT`/
`KF_PET_ADULT_BRANCH_COUNT` already draw in `kf/pet.h`: how many trait
slots exist is structural (the save format and the Lua-side name table
change together), unlike `personality_recency_half_life_seconds`, which is
an ordinary tunable `kf_pet_config` field defaulting to 86400 (24h),
exactly as illustrative as every other number in that function.

## Info screen surfacing

A new "Personality" section on Info (`kf_pet_info_screen.cpp`), the spot
Chris's own doc-16 answer named as natural. `base_trait` is shown
immediately -- unlike `teen_form`/`adult_branch`, it is meaningful from the
moment a pet exists, no "blank before it means anything" case needed. The
dominant care trait follows the existing blank-until-meaningful convention
`teen_form`/`adult_branch` already use, since it reads as a boring 0
default before any real care has accumulated (still an egg).

## Verified

- Full clean rebuild (target `clean`, every object file recompiled from
  source), GCC 13, the same strict warning set as every prior slice: clean.
- `tools/check_no_heap.py`: clean.
- All 13 `ctest` targets pass, including a new `pet_personality_check`
  (`run_pet_personality_check()` in `headless_main.cpp`) covering: base
  trait range and repeatability under a pinned RNG seed; base trait
  unchanged by a month of elapsed time and a care action; the personality
  accumulators carrying FORWARD across a stage transition rather than
  resetting (the direct contrast with `care_integral_mp_seconds`); the
  periodic-halving boundary crossing, checked against exact arithmetic;
  `kf_pet_dominant_care_trait()`'s band selection for each of the three
  needs plus the documented tie-break default; and the v3 save format
  round-tripping every new field byte for byte with the wall clock pinned
  so nothing perturbs the values under test.
- The existing `lua_pet_binding_check` was extended (not replaced) to
  cover `pet.base_trait()`/`pet.dominant_care_trait()`, widening the stage/
  evolution proof script's packed-integer encoding to fit the two new
  fields alongside `pet.stage()`/`pet.teen_form()`/`pet.adult_branch()`,
  checked against the identical packing computed directly from
  `kf_pet_session_state()`/`kf_pet_dominant_care_trait()` in C++.
- `screen_nav_check`'s golden checksum was updated for Info's new,
  intentional "Personality" content -- confirmed deterministic across
  repeated runs before being locked in, same discipline as every earlier
  checksum in this project. This check bypasses `kf_app_init()`, so
  `kf_rng` is never reseeded from the entropy HAL in this path and a fresh
  pet's `base_trait` is identical run to run under the fixed default seed.
- A real interactive pass under `Xvfb`: clicking the debug window's
  existing "Next Screen" button (ADR 0022) reached Info directly, showing
  "Personality" / "Base trait 5" on a fresh egg; clicking "Skip 1 Hour"
  hatched the pet into Baby, and Info then showed "Base trait 5, care
  trait 2" -- 2 (energy) correctly reflects that `kf_pet_default_config()`
  decays energy slowest of the three needs, so with no care actions taken
  energy was genuinely the highest-accumulating need, an actual end-to-end
  proof that the mechanism produces the intuitively correct reading, not
  just a value that round-trips.

## Accepted cost

`personality_recency_half_life_seconds` (86400s / 24h) and the base-trait/
care-trait-label tables are illustrative, per the same "I'll decide exact
numbers later" spirit ADR 0021's stage durations and decay rates already
carry. Action-count-based differentiation, named by Chris as a possible
later addition "if more differentiation is needed," is not built --
per-need averages are the whole mechanism this slice ships, exactly as
scoped. Food/toy/clothing preferences, named by Chris as something
personality should eventually drive, are explicitly out of scope: there is
no item, inventory, or wardrobe system in the codebase at all yet, so
there is nothing to prefer against. What this slice does guarantee is that
`base_trait`/`dominant_care_trait` are already plain, queryable numbers, so
whichever preference system gets built later reads them the same way any
other script would -- no redesign needed then.

## Later

- Action-count-based differentiation on top of the per-need averages, if
  the feel calls for it once pets are actually being raised (Chris's own
  stated fallback).
- Food/toy/clothing preferences, once an item/inventory system exists to
  hang them on.
- The evolution-roster shape mismatch between the character bible
  (`14-character-bible-v1.md`) and ADR 0021's shipped implementation --
  real, separate, not-yet-scoped follow-up work, flagged above and
  explicitly not part of this slice.
- Real trait names and flavour text, once Chris's naming pass (or a fresh
  one specifically for personality, since the character bible's naming
  covers evolution, not this) is ready -- a one-file Lua edit whenever
  that happens, per "Where this lives" above.
