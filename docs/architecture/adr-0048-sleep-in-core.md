# ADR 0048: Sleep in Core — the asleep flag, the waking-fraction constant,
# the save-format bump, and the egg decision

**Status:** Accepted
**Date:** 2026-08-11

## Context

Task 6 of the screens/clock/sleep plan
(`docs/superpowers/plans/2026-08-13-screens-clock-sleep.md`) implements sleep
as the care-loop spec settles it
(`docs/superpowers/specs/2026-08-09-core-care-loop-design.md`'s "Sleep,
settled"). The design was already decided before this task started — Chris's
answer that the creature falls asleep entirely by itself, live and offline
following the same rule, is what makes an otherwise-hard offline problem
tractable. This ADR records how that design became code: the mechanism, the
one named constant the spec calls for, the save-format bump, and the one
decision the spec left open (the egg).

The plan's own "What is true today" table names the largest hidden
dependency in the whole plan: `kf_pet_advance()` never touched
`state->last_advanced`, only `kf_pet_load_and_advance()` did, at load. Sleep's
night-window test needs Core to know what time it is *during live play*, not
only immediately after a reload — so that dependency is this task's first
requirement and the foundation everything else sits on.

## Decision

### 1. `kf_pet_advance()` carries `last_advanced` forward

`hakoniwaos/src/pet.cpp`'s `kf_pet_advance()` now maintains a local `cursor`
that mirrors `state->last_advanced.epoch_seconds`, snapshotting
`have_clock = state->last_advanced.valid` once at entry. Every time the
function's internal stage loop processes a segment (whether or not that
segment crosses a stage boundary), `cursor` advances by exactly that
segment's length and `state->last_advanced.epoch_seconds` is written back
immediately — not just once at the end of the call. This still makes no HAL
call anywhere in this function, matching this file's own header comment: the
value being carried forward was already sitting in `state`, handed in by
whoever last called `kf_pet_load_and_advance()` (the only place a real wall-
clock reading enters Core). A live session that flushes many small
`kf_pet_advance()` calls, one per frame-batch
(`simulator/src/pet/kf_pet_session.cpp`'s `kf_pet_session_frame()`), now
keeps `last_advanced` tracking real elapsed time continuously, not only at
the next reload.

When `have_clock` is false — which is true for a freshly-`kf_pet_init()`ed
state and for **every existing check in this codebase that pokes a bare
`kf_pet_state` directly**, including `hokorimaru_check`, the sickness/death
checks, and `apply_stage_segment_for_test()`'s own callers — `last_advanced`
is simply left alone. There is no baseline epoch to carry forward from, and
this is not new behaviour: it is the same "invalid until the first successful
[load-based] advance" contract `kf/pet.h` already documented, generalised to
cover live play too rather than only the load path.

**A pet that dies mid-segment** stops exactly where it died — the early
`return` inside `kf_pet_advance()`'s stage loop already existed for this
reason (a creature must not be walked through a stage transition on a care
average it did not live to see); the `last_advanced` bump for the segment
that killed it has already happened by the time that `return` fires, and
nothing after it runs. The wall clock genuinely did reach that instant before
the creature died; whatever time was left in the caller's `elapsed_seconds`
is simply never applied to anything again.

### 2. `bool asleep` joins `kf_pet_state` — nothing else does

The spec's "whatever the drowsy state needs" turned out to be nothing beyond
the one boolean. **There is no persisted "drowsy" sub-state in Core.**
Settling the creature into bed is the game layer's decoration
(`docs/superpowers/plans/2026-08-13-screens-clock-sleep.md`'s Task 7,
declared in `creature.lua`), not a mechanism Core depends on — the spec is
explicit that removing the drowsy/settle interaction from the *mechanism*
(rather than solving its offline-meaninglessness problem) is what unblocked
this task in the first place. `state->asleep` is computed fresh inside
`apply_stage_segment()` every time the wall clock is known, as a single
point-in-time membership query against `kf_clock_seconds_in_daily_window()`
(a one-second probe starting at the instant the segment just finished at) —
never a second, hand-rolled "is this hour a night hour" check. Core does not
re-derive the window, including for a yes/no test, not only for the seconds-
accounting below.

There was no reason to consider a richer drowsy state for a future task
either: waking a creature deliberately (allowed, costs happiness — Task 7/8's
concern) simply sets `asleep` false directly through whatever call Task 7
adds; nothing about the mechanism above requires knowing *why* the creature
is awake, only *that* it is.

### 3. Night is 22:00–07:00 local, via the one sanctioned function

Two fixed constants, private to `pet.cpp`'s anonymous namespace —
`kNightStartHour = 22`, `kNightEndHour = 7` — passed to
`kf_clock_seconds_in_daily_window()` (`kf/clock.h`, ADR 0046) everywhere a
night-window question is asked. Night is not `kf_pet_config`: the spec is
explicit that a per-pet bedtime was never asked for, and the window function
itself is designed exactly for this call shape (analytic, no stepping,
correct at the edges) — see ADR 0046 for why that module exists and how its
arithmetic works. This task's whole risk-3 mitigation (offline sleep needing
analytic arithmetic, never a loop) was retired by *building* that function in
Task 3; this task only had to *call* it, twice: once for the point-in-time
"is it asleep now" query, once for the seconds-overlap query below.

### 4. Falling asleep is automatic; live and offline are the same rule

There is exactly one code path that decides `asleep`:
`apply_stage_segment()`. It runs identically whether it is invoked from a
live frame-batch flush or from a single multi-week offline jump inside
`kf_pet_load_and_advance()` — the same "closed-form, bounded by remaining
life stages, never by elapsed time" discipline this file already applied to
decay, mess and neglect before sleep existed. There is no separate "am I
online" branch anywhere in this mechanism. Waking is entirely a function of
the wall clock crossing 07:00 — nothing in Core can leave a creature asleep
forever, because nothing needs to *do* anything for it to wake; the next
segment that crosses the boundary simply computes `asleep = false`.

### 5. The neglect pause: an overlap subtraction, not a rewrite of the crossing math

`neglect_seconds`' three-way "how much of this segment counts as neglect"
logic (already-neglected / crossed-mid-segment / mess-only-at-the-end) is
**untouched**. What changed is what happens to the `neglected_for` seconds it
produces, before they ever reach the accumulator:

```
neglected_for_awake = neglected_for
    - (seconds of the *neglected sub-range* that fall inside the night window)
```

The key fact that makes this a small, local change rather than a redesign:
**in every one of the three existing cases, the neglected range is always the
LAST `neglected_for` seconds of the segment** — case 1 (already neglected)
covers the whole segment; case 2 (crossed mid-segment) covers
`[need_neglect_at, segment)`, a suffix by definition; case 3's
`segment / 2` estimate is, by the same "the second half" framing, also a
suffix. That means the neglected range's own start offset is already sitting
in the existing code as `cared_for` (`= segment - neglected_for`) — reused
directly rather than recomputed, and it is what turns "the exact seconds this
segment was neglected AND asleep" into one more call to
`kf_clock_seconds_in_daily_window()` over
`[segment_start_epoch + cared_for, segment_start_epoch + segment)`.

`cared_for` itself — the genuinely-cared-for seconds that already reduce
`neglect_seconds` — is untouched by sleep. A creature that is fine at night
was already contributing nothing to the accumulator either way; sleep does
not need to freeze a delta that is already zero.

### 6. The waking-fraction constant: 15/24, applied to the thresholds

```cpp
constexpr uint32_t kWakingFractionNumerator = 15u;
constexpr uint32_t kWakingFractionDenominator = 24u;
```

**Where 15/24 comes from.** The night window is fixed at 9 hours
(22:00–07:00), so the fraction of any 24-hour day still available to accrue
neglect is `(24 - 9) / 24 = 15/24`. It is a property of the *window*, not a
tuning value — there is no config surface for it, matching the window itself
not being config (decision 3 above).

**Why the thresholds, never the accrual rate.** `sickness_onset_seconds` and
`sickness_death_seconds` are read through a local, scaled copy
(`effective_onset_seconds`, `effective_death_seconds` — `raw * 15 / 24` with
a `uint64_t` intermediate, the same overflow discipline every other rate
calculation in this file already uses) computed fresh inside
`apply_stage_segment()`; **`config->sickness_onset_seconds` and
`config->sickness_death_seconds` themselves are never written to.** The spec
gives two reasons and the second is load-bearing: `neglect_seconds` is a
save-format field read directly by the debug timeline, so it should keep
meaning literal awake-and-neglected seconds, not a pre-scaled figure whose
meaning depends on whether the clock happened to be known; and neglect
*recovers* as well as accrues (`cared_for` subtracts it back down), so
scaling only the accrual side would have made a cared-for creature's recovery
rate disagree with its accrual rate, harsher in a direction nobody asked for.
Moving the *comparison* instead scales both directions identically without
touching what the accumulator itself means.

**The zero-sentinel guard reads the raw config, never the scaled copy.**
`sickness_death_seconds == 0` means "cannot die" (the same sentinel shape
`poop_interval_seconds == 0` already uses for "no mess"); checking that
against `effective_death_seconds` instead would let a misconfigured-but-
nonzero raw value round down to an accidental zero under `* 15 / 24` integer
division and silently flip the sentinel's meaning. The guard is
`if (config->sickness_death_seconds > 0u)`, unchanged from before this task.

**Inert without a clock, for the identical reason the pause is.** Both the
overlap subtraction and the threshold compression are gated on the same
`have_clock` this whole mechanism is gated on throughout. This is what keeps
every check in this codebase that never establishes a wall clock completely
unaffected — not approximately unaffected, structurally unaffected, since the
compression code path is simply never reached for them.

### 7. Hokorimaru needed no code change, and none was made

The spec proved by reading `pet.cpp` that the dust branch (`select_branch()`
on `care_integral_mp_seconds / stage_elapsed_seconds`, an average of need
levels across the whole child stage) needs no sleep-awareness: needs decay
identically whether asleep or not (decision 5's whole point), and the stage
clock (`stage_elapsed_seconds`) is driven by elapsed seconds, not by
`neglect_seconds`. Nothing about sleep touches either input to that average.
`hokorimaru_check` was run, unmodified, and passes — see "The proof" below
for the exact command.

### 8. Offline stays analytic — the whole point of building ADR 0046 first

A multi-week offline gap is still one call into `apply_stage_segment()` per
*stage* (never per night, never per day) — the loop bound is "remaining life
stages" (at most 4), unchanged from before this task. Within that one call,
the night-seconds accounting is one `kf_clock_seconds_in_daily_window()` call
over however many days the segment spans; that function was already proven
in ADR 0046 to do "whole windows plus two partials" in closed form regardless
of span length. Sleep adds no loop of its own anywhere.

### 9. The egg does not sleep

**Decision: eggs do not sleep**, not "eggs sleep and keep showing
`egg_idle_*`". `apply_stage_segment()`'s existing early return for
`KF_PET_STAGE_EGG` (already there for "no care needed as an egg" — the needs
themselves do not move either) now also means `state->asleep` is never
touched during EGG, so it stays at whatever `kf_pet_init()` left it (`false`)
for the whole stage. This was the cheaper of the two options stated in the
requirement and, more importantly, the more honest one: there is no
`egg_sleeping` art in the shipped pack (18 sleeping sprites cover
baby/child/teen0-3 only — see the plan's finding 1), and an egg has nothing
to be tired *from* in the first place — the same "no care needed as an egg"
reasoning `kf/pet.h`'s own header comment already gives for why EGG's decay
is skipped entirely. `kf_creature_sprite_name()`'s existing collapse-to-
`egg_idle_<dir>` for every pose already covers `KF_CREATURE_POSE_SLEEPING`
defensively (a comment was added at that site, but no code changed there) —
belt-and-braces given `kf_creature_pose_for()` should never actually be asked
for SLEEPING on an egg-stage pet in the first place, per the decision above.

### 10. Pose precedence: dead, sick, ASLEEP, held reaction, neutral

`kf_creature_pose_for()` gained one new branch, between `sick` and the held-
reaction check:

```cpp
if (pet->dead)  return KF_CREATURE_POSE_DEAD;
if (pet->sick)  return KF_CREATURE_POSE_SICK;
if (pet->asleep) return KF_CREATURE_POSE_SLEEPING;
if (reaction_hold_ms > 0u) { ... }
return KF_CREATURE_POSE_NEUTRAL;
```

**Above the held reaction:** a creature that is asleep should look asleep
even if `last_reaction`/`reaction_hold_ms` are still coasting on the last
thing that happened before it dropped off — `last_reaction` is sticky by
design (`kf/creature.h`'s own comment on `reaction_hold_ms`), so without this
ordering a creature that fell asleep moments after a liked feed would show
"happy" instead of "asleep" for the rest of the reaction window. In practice
waking a creature deliberately (the game layer's job, costing happiness)
already clears `asleep` before any *new* reaction could fire, so this
ordering mostly protects the tail of a reaction that happened just before
bedtime, but that is exactly the case worth getting right rather than leaving
to luck.

**Below sick:** an ill creature stays legibly ill even overnight. Sickness is
the state that most needs the player's attention, and hiding it behind a
sleeping pose for roughly a third of every day (the 9-hour night window)
would work directly against that.

The header comment this replaces (`kf/creature.h:47-49` before this task)
said outright *"Sleeping is never returned yet: nothing in Core can say the
creature is asleep."* That sentence became false the moment `state->asleep`
landed — corrected in the same commit as the code that falsified it, per
`CLAUDE.md`'s own rule about a comment contradicting its code being exactly
the class of defect this project has already paid for six times.

### 11. Save format: version 8 → 9, `KF_PET_SAVE_BYTES` 91 → 92

One new byte, `state->asleep`, packed/unpacked as the very last field
(`put_u8`/`get_u8`, matching every other `bool` in this format). A version-8
save is refused by the existing `in_bytes != KF_PET_SAVE_BYTES` check in
`unpack()` — a genuine v8 save is 91 bytes, the code now expects exactly 92,
so the refusal is a direct, honest consequence of the size no longer
matching, not a synthetic corruption test. This matches the policy every
prior version bump in this file already established (ADR 0021 onward): an
incompatible save falls back to a fresh pet via
`kf_pet_load_and_advance()`'s existing "unpack failed" branch, not an error.

## The proof

- `ctest --test-dir build` — **47/47** (the prior 46 plus the new
  `pet_sleep_check`). Command run and confirmed at the end of this task.
- `hokorimaru_check` passes **unmodified** — no source line in that check
  changed. Run in isolation:
  `ctest --test-dir build -R "hokorimaru_check|pet_sickness_check|pet_death_check|pet_stage_evolution_check|pet_personality_check|pet_adult_reachability_check"`
  — all six pass, confirming requirement 7 and that the other pre-sleep pet
  checks (none of which ever establish a wall clock) are likewise untouched.
- `python3 tools/check_no_heap.py .` — `core is heap-free (36 files
  scanned)`, a real, non-zero scan count.
- No `float`/`double` anywhere in `hakoniwaos/src/pet.cpp`,
  `hakoniwaos/src/creature.cpp`, or `hakoniwaos/src/clock.cpp` (checked by
  hand with a word-boundary grep, since `check_no_heap.py` does not check
  this).
- The ESP-IDF cross-compile (`-DKF_PANEL=ili9341`, a full clean rebuild) is
  clean — zero warnings anywhere in project code. Firmware image: 513,536
  bytes (67% of the app partition free).

### Non-vacuity: every new assertion broken and watched fail, then restored

Per this project's own rule that a passing test is not evidence it tests
anything, every new assertion below was proven non-vacuous by breaking the
mechanism it depends on and observing the specific failure, then restoring
the code and reconfirming green:

| Assertion(s) | Breakage introduced | Failure observed |
|---|---|---|
| `kf_pet_advance()` carries `last_advanced` forward (2 assertions in `run_pet_check()`'s new case 6) | Removed the `cursor`/`last_advanced.epoch_seconds` update inside the stage-loop segment branch | `FAILED: kf_pet_advance() carries last_advanced forward by exactly the elapsed seconds it was handed` and `FAILED: last_advanced carries forward by the FULL elapsed time even when the call crosses a stage boundary internally` |
| `kf_creature_pose_for()` returns SLEEPING (2 new cases in `creature_pose_check`) | Removed the `if (pet->asleep) return KF_CREATURE_POSE_SLEEPING;` branch | `FAILED: asleep outranks a happy reaction: expected 4, got 1` and `FAILED: asleep outranks a disliked reaction too: expected 4, got 2` |
| `state->asleep` tracks the night window live (`run_pet_sleep_check()` case 1) | Disabled the `state->asleep = kf_clock_seconds_in_daily_window(...)` assignment | `FAILED: asleep the instant the clock reaches 22:00` and `FAILED: still asleep in the middle of the night` |
| Eggs never become asleep (`run_pet_sleep_check()` case 2) | Temporarily let `KF_PET_STAGE_EGG` fall through past its early `return` into the sleep computation, landing the probe instant well inside a night (an earlier version of this test landed the probe exactly on the night's exclusive end and did not catch this — corrected before landing; see "A bug found while proving non-vacuity" below) | `FAILED: an egg never becomes asleep, even landing on an instant well inside a night` |
| Neglect pauses while asleep (`run_pet_sleep_check()` case 3, and the offline night-exclusion assertion in `run_pet_check()`'s new case 7) | Short-circuited the night-overlap subtraction (`if (false && have_clock && neglected_for > 0u)`) | `FAILED: neglect_seconds stays at ZERO across a whole night the creature spent neglected -- the neglect clock pauses while asleep`; and, separately, all three offline spans: `FAILED: starts mid-night: clock-aware neglect_seconds (28800) is not less than the no-clock comparison (28800)` (and the equivalent for the other two spans) |
| Waking-fraction threshold compression (`run_pet_sleep_check()` case 4) | Short-circuited the compression block (`if (false && have_clock)`) | `FAILED: 15000s -- the RAW 24000s threshold compressed by exactly 15/24 -- is enough` |
| `asleep` round-trips through save/load (`run_pet_sleep_check()` case 5) | Forced `pack()` to always write `0` for `asleep` | `FAILED: asleep round-trips through save/load byte for byte` |
| A v8 save is refused (`run_pet_sleep_check()` case 5) | Simultaneously loosened `unpack()`'s size check (accept `KF_PET_SAVE_BYTES - 1` too) *and* its version check (accept `8` too), with the load buffer zero-initialised to avoid reading uninitialised stack memory during the experiment — loosening either guard alone left the other one still correctly refusing the buffer, which is itself a real (and reassuring) finding: the two checks are independent layers, not one check duplicated | `FAILED: a rejected v8 save falls back to a fresh pet, exactly like every earlier version rejection in this file` |
| Offline analytic fast-forward matches one direct `kf_pet_advance()` call across a night (`run_pet_check()`'s new case 7, three spans) | Same breakage as the neglect-pause row above — this assertion group and the "strictly less than no-clock" group share the identical underlying code, and both failed together | `FAILED: starts mid-night: offline path (neglect=0 hunger=100000 sick=0) does not match one direct kf_pet_advance() call (neglect=1800 hunger=0 sick=0)` (this specific message came from an earlier, different bug — see below — but the mechanism-disabled run also failed this same assertion group, just with different numbers) |
| `pet_offline_ageing_check`'s new night-spanning cases, generally | See "A real bug found while writing this task" below — the FIRST failure this task's own test-writing process produced was not a broken production assertion at all, but a bug in the test itself | — |

### A real bug found while writing this task's own tests

While writing the offline night-spanning cases for `run_pet_check()`, the
first run failed with `neglect=0 hunger=100000` for every span — i.e. the
"loaded" pet had not aged at all. The cause was in the **test**, not in
`kf_pet_advance()`/`apply_stage_segment()`: the test called
`kf_pet_load_and_advance()` once to establish a baseline (correctly adopting
the pinned wall clock as `last_advanced`), then jumped the simulated clock
forward and called `kf_pet_load_and_advance()` a second time — without ever
re-saving the baseline in between. The second load read the *original*
saved bytes back off disk, whose `last_advanced` was still invalid (a fresh
`kf_pet_init()`ed pet has never been advanced), so
`kf_pet_load_and_advance()` correctly took its "nothing to fast-forward
FROM" branch and simply adopted the new `now` with zero elapsed time — the
same defensive behaviour `kf/pet.h`'s own header comment documents, working
exactly as designed against a test that had not actually set up the
precondition it claimed to. Fixed by adding the missing `kf_pet_save()` call
between establishing the baseline and jumping the clock, mirroring the
pattern `run_pet_check()`'s existing case 4 already uses for the identical
reason. Left in "The proof" table above as a record that this task's own
early runs were not silently green from the start.

A second, smaller bug of the same shape was found and fixed while proving
non-vacuity for the egg exemption (case 2 of `run_pet_sleep_check()`): the
first version of that test advanced from 02:00 by six hours, landing at
08:00 — *after* the night window's exclusive end — so even with the egg's
sleep exemption completely disabled, the assertion `!egg.asleep` still
passed, for the wrong reason (the clock itself said "awake" by then, not the
exemption). Caught only because this task's own non-vacuity pass disabled
the exemption and the test still went green; fixed by shortening the advance
to one hour so the probe instant (03:00) lands well inside the night, then
reconfirmed the disabled-exemption run genuinely fails.

## Not verified

**Nothing about sleep has run on hardware.** This task is Core-only, per the
plan's own "Who has to be at the bench" table (Task 6: no board, no Chris).
Task 7 (the game-side presentation — the sleeping pose, the drowsy cue, the
bedding, waking it deliberately) and Task 8 (the attention signal, which
explicitly must not fire while asleep) are unstarted; this task builds
exactly what they will read (`state->asleep`,
`KF_CREATURE_POSE_SLEEPING`), nothing about how it looks.

**The waking-fraction figure (15/24) has never been felt, only derived.**
The spec's own words: "whether their combined pressure feels right is a
thing to feel rather than derive." That judgement — alongside the late-night
deficit the spec also describes, which is an emergent property of needs
continuing to decay overnight rather than a separate mechanism this task
built — is explicitly deferred to Task 7/8, where a real device and a real
player are in the loop.

**The offline night-spanning cases use a synthetic, single-need config**
(`night_config`/`only_hunger`/`compress_cfg` in the tests above), not
`kf_pet_default_config()`. This was deliberate — it isolates the mechanism
being proved from every other decay channel that could also push a creature
into `is_neglected()` and blur which seconds are being measured — but it
means the *default* config's actual sleep-adjusted pacing (how many real
hours a default pet can go unattended overnight before falling ill) has not
been measured end to end. That is exactly the "thing to feel" the paragraph
above defers to Task 7/8, not a gap this task should have closed on its own.
