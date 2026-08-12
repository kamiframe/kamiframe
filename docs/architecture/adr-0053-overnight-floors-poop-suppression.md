# ADR 0053: Overnight floors, a dirtiness cap, and no poop while asleep

**Status:** Accepted
**Date:** 2026-08-11

## Context

Chris, after testing sleep and tuck-in in the simulator, 2026-08-11:

> "While the pet is sleeping, their needs decay, and poop still builds up.
> Poop at the very least should not be building up... while it's sleeping it
> should only ever decay to whatever that total level would be in the
> morning, which also depends on if you tucked it in manually, or it did it
> itself... The pets needs can never drop to absolute 0 while it's asleep,
> not even when it's sick... the pet WILL NOT poop while it's asleep."

Sleep (ADR 0048) and the drowsy/tuck-in window (ADR 0052) already existed.
This extension does not touch either mechanism's own logic; it adds a
protective ceiling and floor on top of what they already compute.

## Decision

### 1. Three overnight floors, rolled once at the awake -> asleep transition

`kf_pet_state` gains three new fields: `hunger_floor_mp`, `happiness_floor_mp`,
`energy_floor_mp`. Each is rolled independently -- **three separate
`kf_rng_below()` calls**, never one value copied into all three -- the moment
`apply_stage_segment()` (`hakoniwaos/src/pet.cpp`) detects the creature
falling asleep, from a band selected by two booleans read AT THAT INSTANT:

|              | tucked in    | not tucked in |
|--------------|--------------|----------------|
| well         | 60-70%       | 50-60%         |
| unwell       | 40-50%       | 20-30%         |

`kf_rng_below()`, not the entropy HAL -- the same game-visible,
save/replay-deterministic RNG `base_trait` already uses (`kf/rng.h`), so a
pinned seed reproduces the exact same night twice.

**"Unwell" is this task's own inference, not a spec quote.** Chris gave two
examples ("sick or not doing well") and left the definition open. It is
implemented as: `state->sick` (as it stood entering this segment) **OR** any
of the three needs below `kUnwellNeedThresholdMp` (25%, `pet.cpp`'s anonymous
namespace) **at the moment the creature falls asleep**. Flagged here as a
starting point, open to tuning -- there is no evidence beyond "a reasonable
guess" behind 25% specifically.

**SET-POINT, NOT A PURE FLOOR -- a decision, made and flagged rather than
buried.** Applied as `max(decayed_value, floor)`: a pet that falls asleep at
10% hunger *wakes* somewhere in its band, not stuck at 10%. Chris's own
phrasing ("its needs *would be* a random value between 20 and 30%") reads
this way, and the stated goal ("never able to get to zero") is most directly
satisfied by a set-point. The consequence is real and worth naming plainly:
**sleep becomes a small heal**, so a neglected pet is partially rescued by
morning rather than merely protected from getting worse. Reversing this to a
pure floor is a one-line change by design -- swap the `>` comparison for
`max(naive_value, floor)` against `naive_value` alone stays the same shape,
only the clamp direction's *consequence* differs, not the code path.

**Compile-time constants, not `kf_pet_config`.** Unlike most of this file's
tunable numbers (`tuck_in_wake_bonus_mp`, `wake_happiness_cost_mp`), the four
bands live in `pet.cpp`'s anonymous namespace next to `kNightStartHour` and
`kDrowsyWindowSeconds`. Flagged as a judgment call: these bands are closer in
kind to the night window itself (a fixed global rule about what a night does)
than to a per-pet care number a cartridge author is expected to retune. Moving
them into `kf_pet_config` later, if Chris wants per-pet tuning, is small and
mechanical, not a redesign.

**Cleared on waking.** The moment `apply_stage_segment()` detects the
asleep -> awake transition, all four fields (the three floors and the
dirtiness cap below) reset to 0 -- never a real floor/cap value (every band's
minimum is >= 20000), so this doubles as "not currently active" without a
separate flag.

**Saved** (`kSaveVersion` 10 -> 11, `KF_PET_SAVE_BYTES` 93 -> 109, four new
`uint32_t` fields) for the same reason `tucked_in` is: the device being
switched off overnight is the whole case that matters, and a floor that
evaporated on reload would protect only a live session. A version-10 save is
**refused**, not defaulted -- matching every prior bump in this file. Chris
was not re-asked about migration for this bump; the "no migration during
development" answer from the version-10 bump (ADR 0052) is treated as still
standing.

### 2. The dirtiness cap: a second, explicitly-flagged inference

Chris named hunger, happiness and energy. Dirtiness is a fourth axis, and it
**rises** rather than falls, so the floors above do nothing for it -- left
alone, a pet can wake filthy enough to be pushed toward sickness, undercutting
the entire point of this change. `dirtiness_cap_mp` (a fourth new saved
field) mirrors the needs bands' shape -- same two booleans, same rolling
instant -- but as a **ceiling** (`min(risen_value, cap)`), and with a single
fixed value per band rather than a rolled range:

|              | tucked in | not tucked in |
|--------------|-----------|----------------|
| well         | 25%       | 40%            |
| unwell       | 45%       | 60%            |

Picked, not derived -- flagged as an inference from intent, same status as
the "unwell" threshold. A well, tucked-in pet caps at the lowest ceiling,
matching Chris's own "wakes least dirty" framing.

### 3. No new poop while asleep, and the counter pauses rather than resets

`apply_stage_segment()`'s poop-generation block (`hakoniwaos/src/pet.cpp`,
was around lines 611-633 before this change) now counts against
`awake_seconds_in_segment` -- `segment` minus however many of this segment's
seconds fall inside the night window
(`kf_clock_seconds_in_daily_window()`, the identical closed-form building
block the neglect-pause already uses) -- instead of `segment` itself, for
**every** use: the "has enough time passed for a poop" comparison, the
extra-poops division, and the plain countdown subtraction.

Because a plain countdown only cares how many awake seconds have elapsed, not
*when* within the segment they fell, this is **exact, not an approximation**,
and -- unlike the floors and the tuck-in bonus -- it is correct for a segment
spanning **any number of nights**, not only the first. `seconds_until_next_poop`
is simply frozen for every asleep second: a pet that had, say, 1800 of its
3600-second interval left when it fell asleep still has exactly 1800 left the
moment it wakes, whether that gap was one night or three. **This is what
stops a whole night's suppressed interval from dumping a pile of poops the
instant the creature wakes** -- the counter never accumulated debt to begin
with, so there is nothing to release.

**Existing poop's effect is what changes, not its presence.** Poop already
waiting when the creature falls asleep is untouched by the night (nothing
clears `poop_count`); what changes is `dirtiness_rise_per_hour`, split into
two rates -- `dirtiness_rise_mp_per_hour` alone while asleep,
`+ dirtiness_rise_per_poop_mp_per_hour * poop_count` while awake -- weighted
by the same asleep/awake second split the poop counter uses. This is a
genuine **rate reduction**, not a set-point, so (like the poop counter) it is
correct for any number of nights in one segment, not just the first: existing
poop still dirties the pet a little overnight (the base rate, unchanged) but
stops accelerating it until the creature wakes, at which point the per-poop
term resumes at full strength immediately.

### 4. The floor/cap apply at the wake instant, not the segment's end

`kf_pet_advance()`'s segments are bounded by stage transitions, not by
night-window crossings (this file's own header comment) -- a single call can
span an entire night start to finish, exactly what a real offline
fast-forward does. This is precisely the bug ADR 0052 documents finding for
the tuck-in bonus, and the fix generalises here: a shared
`next_epoch_at_hour()` helper finds the next `kNightStartHour:00:00` and
`kNightEndHour:00:00` at or after a segment's own start, giving closed-form
`falls_asleep_this_segment` / `wakes_this_segment` booleans independent of
what `state->asleep` merely reads at the very end.

When a segment carries the creature **past** the wake instant, the floor/cap
is computed and applied **at that exact instant** (`apply_decay()` from the
segment's own starting values, for exactly the seconds up to wake), then
ordinary, **unprotected** decay/rise continues for whatever of the segment
remains afterward. Clamping the segment's *final* value instead would
incorrectly protect hours of ordinary daytime decay that happened after the
creature woke -- the identical reasoning ADR 0052 gives for why the tuck-in
bonus could not use a before/after comparison either. When a segment ends
**still asleep**, no split is needed: the naive whole-segment value already
*is* "now," so the floor/cap clamps it in place directly.

> **AMENDED 2026-08-12 -- the previous paragraph's last sentence described a
> real defect, not a design choice.** "The floor/cap clamps it in place
> directly" turned out to mean re-applying `max(decayed, floor)` on **every
> single segment** that ends still asleep, not once. In live play,
> `kf_pet_session_frame()` flushes roughly every `KF_PET_SESSION_FLUSH_
> SECONDS` (30s), so a real overnight session hit this branch on the order of
> 1,080 times a night -- and because `care_integral_mp_seconds` accumulates
> each segment's *start-of-segment* value (section on the care integral,
> `pet.cpp` line ~739), every one of those re-clamps fed a floor-level number
> into the very next segment's own contribution. The floor became a level
> held continuously through the night rather than the wake-instant set-point
> this ADR itself already describes it as ("a pet that falls asleep at 10%
> hunger *wakes* somewhere in its band" -- a MORNING value, per Chris's own
> "would be a random value... by next morning"). See "Amendment: the
> per-segment reclamp defect" below for the fix, the measured consequence,
> and the fix's own new test coverage. Do not read the sentence above as
> still describing current behaviour for the "still asleep" case -- it only
> ever should have applied at the wake instant, which is the one case this
> paragraph's FIRST half (the `wakes_this_segment` split) already covers
> correctly and unchanged.

**Only the FIRST night-crossing inside a segment is resolved this precisely**
-- the same accepted limitation ADR 0052 already documents for the tuck-in
bonus, for the identical reason (a segment spanning several nights at once,
e.g. a pet away for a week, only rolls/pays once). This is a real, documented
gap for a multi-night-in-one-segment span; it is not a gap for the case that
actually matters (the device switched off overnight, one segment, one
night) -- see the non-vacuity section for exactly what was proven.

`next_epoch_at_hour()` itself uses a strict `<` (not `<=`) when deciding
whether to push its result a day forward: if the segment's own start already
sits exactly on the target hour, that IS the crossing being asked about. Every
caller that reaches this boundary through genuine continuous simulation never
actually lands exactly on it (the segment that reaches a boundary already
updates `state->asleep` for that instant, so the next segment's own start
reads the already-updated flag) -- this only matters for a state built by
hand, which several of this task's own tests do.

### 5. Nothing new can kill a sleeping pet

Verified, not assumed, per CLAUDE.md's own rule about subagent claims of
"impossible": `state->dead` is set in exactly one place
(`apply_stage_segment()`'s neglect/sickness block), from
`state->neglect_seconds >= effective_death_seconds`, and that check fires on
**every** call -- so a live (non-dead) pet can never carry a neglect total at
or past the death threshold into a later call; the instant it reaches that
level, `dead` is set in the same call. Nothing this task adds touches
`neglect_seconds` accrual or the death check itself. The floors/cap protect
`hunger_mp`/`happiness_mp`/`energy_mp`/`dirtiness_mp` specifically, which
death does not read directly.

One case is real and worth naming plainly, not a bug: if a pet accumulates
enough neglect to cross the death threshold **during the AWAKE portion** of a
segment that also happens to include some sleep later in the same call, it
still dies -- the death check evaluates the segment's aggregate
`neglect_seconds`, not a moment-by-moment trace. That is death from genuine
daytime neglect that happened to be bundled into a segment that later touches
a night, not death *during* sleep in the sense Chris's request describes;
`run_pet_sleep_check`'s case 11(c) and `pet_offline_ageing_check`'s case
8(c) both prove the actual "fell asleep already at absolute zero, sick" case
survives to morning with every need strictly above zero.

## Non-vacuity

Every new assertion was broken and watched fail, then restored -- see the
implementation report for exact failure messages. Two are worth recording
here because they caught real bugs, not just hypothetical regressions:

- **A shared-process RNG collision.** `pet_offline_ageing_check`'s existing
  offline-vs-direct-advance equivalence case (`run_pet_check()`'s case 7)
  compares two independent `kf_pet_advance()` sequences built from the
  identical starting state. Both now roll an overnight floor via
  `kf_rng_below()`, drawing from the SAME process-global stream
  (`kf/rng.h`) -- and the two sequences run at different points in that
  stream, so without reseeding between them they draw different floors and
  the comparison fails for a reason that has nothing to do with either
  mechanism actually disagreeing. Fixed by reseeding to an identical value
  immediately before each sequence -- exactly "results are identical across
  two runs with the same seed," this task's own required non-vacuity case,
  applied to two sequences within one test rather than two process runs.
- **An unpinned host wall clock.** `lua_pet_binding_check`'s mess stage
  (`run_lua_pet_check()`) never established a wall clock explicitly and
  relied on whatever the real host clock happened to read. It failed while
  this task was being built because the host clock was, at that moment,
  genuinely inside the 22:00-07:00 night window -- poop suppression (this
  ADR) correctly suppressed every poop for a session that was, by
  coincidence of when the test happened to run, asleep the whole time. Fixed
  by pinning the wall clock to a known daytime instant before the session is
  ever created, the same discipline this file already applies everywhere
  else a test's outcome depends on the clock.
- **The boundary bug `next_epoch_at_hour()`'s strict `<` fixes** was found
  by the very test meant to prove "never zero, even sick, from absolute
  zero" (case 11(c)): landing a hand-built state exactly at 22:00:00 with
  `asleep` still false meant the original `<=` pushed the bedtime crossing a
  full day forward, silently skipping the floor roll for the entire
  intervening night.

## Consequences

- `docs/superpowers/plans/2026-08-13-screens-clock-sleep.md`'s Task 6 "the
  neglect clock pauses while asleep, but the needs do not" language, and
  ADR 0048's identical framing, describe the *neglect accumulator*
  specifically and remain true unchanged -- but a reader could previously
  infer from them that needs decay **all the way down** overnight with
  nothing to stop them. That is no longer the whole picture: the three
  needs now have a floor, mess (poop generation) now pauses too. Both
  documents are annotated with a pointer to this ADR rather than rewritten,
  per CLAUDE.md's own instruction to annotate a superseded line rather than
  silently rewrite it.
- ADR 0052's tuck-in bonus is now one of TWO overnight mechanisms that stack
  for a tucked-in creature: the higher tucked-in floor band AND the wake
  bonus both apply, in that order (floor/cap first, bonus paid on top) --
  Chris's "needs less care in the morning if you tucked it in" is now
  satisfied twice over for a tucked-in pet, not merely once.
- `run_pet_sleep_check()`'s case 8 (the tuck-in-bonus LOAD-BEARING
  comparison) and case 10 (its offline counterpart) both had their starting
  needs raised from 60000 to 85000mp, comfortably above every overnight
  floor band's ceiling (70000, well+tucked-in) -- otherwise the NEW floor
  mechanism could add its own random bump on top of the bonus for the
  tucked-in pet specifically (60000 sits inside the well+tucked-in band's
  own range), breaking the "ahead by EXACTLY `tuck_in_wake_bonus_mp`"
  assertion non-deterministically. Case 10 was also switched from real decay
  to a zeroed-decay config, for the identical reason -- isolating the bonus
  proof from this new mechanism rather than conflating the two.

## Amendment (2026-08-12): the per-segment reclamp defect

`docs/reviews/2026-08-12-sleep-stack-audit.md`'s finding 1 caught, and Chris
confirmed, that section 4's "still asleep" branch above was applying the
floor/cap on **every segment** a live session's needs ended asleep in, not
once at the wake instant the rest of this ADR describes. Section 4 above
carries an inline annotation pointing here rather than being silently
rewritten, per `CLAUDE.md`'s own rule; this section is the fix's full record.

**The bug.** `apply_stage_segment()`'s "still asleep at the end of this
segment" branch (`pet.cpp`, was around lines 1016-1036) read `state->hunger_mp
= max(state->hunger_mp, state->hunger_floor_mp)` (and the equivalent for
happiness, energy, and `dirtiness_cap_mp`) unconditionally, every call. A live
session (`kf_pet_session_frame()`) flushes every `KF_PET_SESSION_FLUSH_
SECONDS` (30s), so an unattended, powered-on overnight session hit this
branch roughly 1,080 times a night, re-clamping the needs up to the floor on
every single one. `care_integral_mp_seconds` (this file's own header comment;
`pet.cpp` line ~739) accumulates each segment's **start-of-segment** value --
so every one of those re-clamps fed an artificially high, floor-level number
into the CHILD-stage care average on the very next segment. Offline (the
device switched off overnight, one segment, one low pre-sleep value) was
never affected -- there is no interior segment boundary for the bug to repeat
across in a single `kf_pet_advance()` call. The wake-instant clamp in the
`wakes_this_segment` branch (the first half of section 4 above) was, and
remains, correct and untouched.

**Measured consequence (before the fix, `docs/reviews/2026-08-12-sleep-
stack-audit.md`'s own numbers, independently reproduced during this fix):**
an identical, completely unattended CHILD-stage pet (same seed, same
never-touched neglect pattern, `dust_care_average_mp` = 20000mp/20%) scored
**13.7%** on the care average with a wall clock established (the bug active)
versus **4.8%** with no wall clock at all (the mechanism entirely inert) --
a **~2.85x** inflation purely from whether a wall clock happened to exist,
with nothing to do with how the pet was actually treated. Because that
average selects the Hokorimaru ("dust") branch at the CHILD -> TEEN
transition, **leaving the device powered on overnight changed which adult
family the pet could reach**, silently working against the specific,
named "neglect made visible" story beat the dust form exists for.

**Chris's decision:** "the overnight floor, yes 100% make it clamp at wake" --
confirming the reading this ADR's own section 1 already commits to ("SET-
POINT, NOT A PURE FLOOR... the stated goal is most directly satisfied by a
set-point... it wakes somewhere in its band"), and rejecting the alternative
this ADR flagged (a floor held continuously). The fix is a deletion: the
"still asleep" branch's clamp is removed outright, for the three needs AND
`dirtiness_cap_mp` (no principled reason found to treat the cap differently
from the floors here -- Chris did not distinguish them, and the cap is the
identical mechanism on a different axis). The needs and dirtiness now decay/
rise completely **unprotected** for the whole night once the floor/cap is
rolled at bedtime; the ONLY clamp left is the pre-existing, correct
wake-instant one.

**Re-measured after the fix, same scenario, same seed:** **4.996%** with a
wall clock versus **4.811%** without -- roughly **+3.9%**, not **+185%**.
Both numbers stay well under the 20% dust threshold in this particular
extreme-neglect scenario, matching the audit's own observation, but the
*bias* the bug introduced is now small and explainable (a few hours of
floor-protected time right at the very end of the stage, not an entire
night's worth of artificially-elevated segments) rather than large and
systematic.

**A real, visible consequence, not hidden:** with the per-segment clamp
gone, the needs bars now **visibly drain during the night** on a device left
running and displaying its home screen, then **jump back up** to the rolled
floor/cap only at the moment the creature wakes (07:00 by default). Verified
directly against `pet.cpp` (not assumed): a pet asleep from 21:55 with
default CHILD decay rates reaches 0% hunger by roughly 00:00, stays at 0%
for the rest of the night, then jumps to its rolled floor (56.1% in the
probe run) in the single 30-second segment that crosses 07:00:00. Chris had
already noticed bars draining overnight before this fix and suspected a bug
-- this IS the intended behaviour now: the floor is what the creature wakes
up with, not a level it is held at throughout the night.

**Non-vacuity.** `pet_offline_ageing_check` (`run_pet_check()`'s new case 9,
`headless_main.cpp`) compares many `KF_PET_SESSION_FLUSH_SECONDS`-sized
`kf_pet_advance()` calls across an 11-hour night against one direct advance
of the identical total elapsed time, using deliberately slow, flat decay
rates so a coarse single-sample "large advance" (this file's own documented
left-Riemann-sum approximation) stays a fair reference rather than diverging
from segmentation granularity alone. With the fix: diff ~1.7% (well inside a
5% tolerance chosen to sit comfortably above both the accepted live-path
truncation loss and the left-Riemann approximation, but far below the bug's
own ~185% scale). With the old clamp temporarily restored: `FAILED: many
small (1320 x 30s) advances across a night produced care_integral_mp_
seconds=2116720800, vs 1504800000 for one single 39600s advance of the
identical elapsed time -- diff 611920800 exceeds the 75240000 (5%)
tolerance` -- a diff of ~40.7%, unambiguously outside tolerance. Reverted
immediately after.

**Evaluated, not implemented (Chris's call):** `neglect_seconds` already
pauses while the creature is asleep (ADR 0048) -- the principled complement
would be for `care_integral_mp_seconds` to pause too, since sleep is not
care time. Measured what a naive version of that (weighting each segment's
contribution by `awake_seconds_in_segment` instead of `segment`, keeping the
existing `stage_elapsed_seconds` divisor unchanged) would do to the 11-hour
slow-decay scenario above: the reported CHILD care average dropped from
37.3% to 6.9%, a ~5.4x reduction -- because dividing by the FULL elapsed
time (including every asleep hour) while only accumulating the AWAKE hours'
contribution systematically understates the average for every pet that
sleeps normally, not just a neglected one. A dimensionally correct version
would need a second, new accumulator (awake-only elapsed seconds) to divide
by instead of `stage_elapsed_seconds` -- exactly the "more invasive; touches
an accumulator untouched by every sleep task so far" cost the audit's own
Option B already flagged. Not implemented here; the numbers above are for
Chris's decision.
