# 2026-08-12 sleep-stack audit

Reviews the fifteen commits `4a92313..719380b` (sleep in Core, sleep on
screen, the attention signal, the home-screen clock, seven futon sprites,
the ten-minute drowsy window and tuck-in bonus, overnight floors/dirtiness
cap/poop suppression, and four debug-clock changes) against ADR 0048, 0051,
0052, 0053 and 0054. This is a review only — no design decisions were made,
and nothing in `hakoniwaos/`, `simulator/`, `sdk/`, `examples/` or `ports/`
was left changed by this audit (one deliberate break-test on `pet.cpp` was
made and reverted; `git diff` on that file is empty).

## Suite status

`ctest --test-dir build`: **51/51**, run five times across this session (three
before an unrelated, concurrently-landing audio change transiently broke the
shared build target — see "Note on a concurrent build break" below — and two
after it landed cleanly). No flakiness observed. The suite's global default
wall-clock pin (`kf_host_time_set_wall_fixed(1767225600)`, main() in
`simulator/src/headless/headless_main.cpp`, predates this project's sleep
work entirely — added in the very first headless-backend commit) sits at
2026-01-01T00:00:00Z, which is itself inside the night window; every check
that cares about day-vs-night re-pins its own clock explicitly rather than
relying on that default, and I did not find one that doesn't (see "Checked
and found correct" below).

## Findings, ranked by severity

### 1. (Medium-High) The overnight floor (ADR 0053) was never re-verified against the CHILD/TEEN care-average, and `hokorimaru_check` cannot see the interaction

**What's wrong.** `apply_stage_segment()`'s care-integral accumulation
(`hakoniwaos/src/pet.cpp:738-740`, `care_integral_mp_seconds += average_before_mp * segment`)
runs *before* the sleep/floor block later in the same function, using
`hunger_before_mp`/etc. captured at the very top of the call — so a floor
rolled and applied *within* a given segment cannot inflate that same
segment's own contribution to the care integral. But the floor clamp does
persist into `state->hunger_mp` at the end of a segment that ends "still
asleep" (the `else` branch around `pet.cpp:1016-1036`), and the *next*
segment's `hunger_before_mp` is simply read fresh off `state` at that next
call's top. For live play — the realistic "device left on overnight" case
this whole feature was built for — the pet session flushes roughly every 30
seconds (`KF_PET_SESSION_FLUSH_SECONDS`), so a sleeping, completely
unattended creature gets its needs re-clamped to at least the overnight
floor on *every one* of those ~30-second segments, all night, every night —
and each of those floor-protected values is fed straight into the CHILD
stage's `care_integral_mp_seconds`, the exact number
`advance_to_next_stage()` later divides to decide the Hokorimaru
(dust-form) branch.

`hokorimaru_check` (`run_hokorimaru_check()`,
`simulator/src/headless/headless_main.cpp:1581`) never establishes a wall
clock on any of its pets (`kf_pet_init()` leaves `last_advanced.valid =
false`, and nothing in the check sets it), so `have_clock` is `false`
throughout and the sleep/floor mechanism is completely inert for it — the
check only ever exercises the *pre-sleep* code path. ADR 0048's own
verification of this ("Hokorimaru needed no code change ... `hokorimaru_
check` was run, unmodified, and passes") was true and correctly checked
*on the day it was written*, before the floor mechanism (ADR 0053) existed.
Once ADR 0053 landed, this task's own instructions explicitly called out
"the dirtiness cap and poop suppression against `hokorimaru_check`" as
something to check — but the interaction was never re-run, and the
pre-existing check structurally cannot catch it, since it never turns the
clock on at all.

**The evidence** (a command and its output, not an argument — built directly
from the real, unmodified `hakoniwaos/src/pet.cpp`/`rng.cpp`/`clock.cpp`,
same functions the game ships):

```
$ g++ -std=c++17 -I hakoniwaos/include -I simulator/src -I simulator/src/host \
    probe5.cpp probe_stubs.cpp simulator/src/host/host_log.cpp \
    hakoniwaos/src/pet.cpp hakoniwaos/src/rng.cpp hakoniwaos/src/clock.cpp \
    -o probe5 && ./probe5
WITH_CLOCK teen_form=4 last_avg_mp=13691.7 (13.692%) elapsed=172770
NO_CLOCK   teen_form=4 last_avg_mp=4810.6 (4.811%) elapsed=172770
dust_care_average_mp threshold = 20000 mp (20.0%)
KF_PET_TEEN_FORM_DUST = 4
```

Same seed (12345), same completely-untouched pet (zero care actions ever),
same default `kf_pet_default_config()` decay rates and CHILD duration,
death disabled only so the comparison can resolve to a branch decision at
all (with death enabled — the real default — a fully-neglected pet dies
around 96,240s regardless of clock, well before the branch point, per
`advance_to_next_stage()`'s own CHILD-case comment: this specific
comparison needed a synthetic "somehow survives" pet to observe the
average; the RESULT still applies to any real pet that receives just
enough care to avoid the death threshold). The two runs differ *only* in
whether a wall clock is ever established. Result: the identical neglect
pattern scores **~2.85x higher** on the care average with the clock on than
with it off — 13.7% vs 4.8%, both currently still under the 20% dust
threshold for this particular extreme-decay setup, but a large, systematic,
one-directional bias that grows with how many nights fall inside the
CHILD stage. It is entirely plausible a genuinely marginal "kept barely
alive, nothing more" pet (`hokorimaru_check`'s own `barely` case, `pet.cpp`
line ~1621: needs pinned at 15%, deliberately below the 20% dust threshold)
would cross into a real family purely by sleeping through enough nights on
a device that stays powered, never touched.

**What it costs a player.** The Hokorimaru dust-form character — "neglect
made visible," per the character bible and `hokorimaru_check`'s own
comments — is the intended, deliberate outcome for a creature that was
"kept alive and nothing more." If a player leaves the device powered on
overnight (the ordinary, expected way to own one of these), the creature's
own sleep is now silently doing some of the "care" that was supposed to be
the player's job, making the dust form measurably harder to reach than the
design intends — undermining a specific, named story beat without anyone
deciding that on purpose.

**Suggested fix.** Not a one-line fix — this is a real design interaction,
not an obvious bug, and belongs with Chris:
- **Option A (accept it):** sleep is deliberately forgiving; a creature
  that merely survives via the floor, rather than never being fed at all,
  arguably *was* cared for enough. If so, say so explicitly somewhere near
  `dust_care_average_mp`'s own comment, so the next reader doesn't
  rediscover this as a surprise.
- **Option B (exclude the floor from the integral):** when a segment's
  starting need value came from a floor clamp applied at a previous
  segment's end, don't let that clamp count toward `care_integral_mp_seconds`
  — e.g. track the pre-floor decayed value separately for the integral's
  own weighting. More invasive; touches an accumulator that has been
  untouched by every sleep task so far.
- **Minimum, regardless of which way this goes:** extend `hokorimaru_check`
  (or add a sibling check) with a with-clock, multi-night variant of the
  `barely` case, so this interaction is monitored going forward instead of
  silently exercising only the code path that is now atypical in
  production.

### 2. (Low-Medium, worth a decision, not a code defect) Care actions are never gated on `asleep`

`kf_pet_feed()`/`kf_pet_play()`/`kf_pet_rest()`/`kf_pet_bath()` (`pet.cpp`)
have no `state->asleep` check anywhere, and neither does
`kf_pet_session_feed()`/`_play()`/`_rest()`/`_bath()`
(`simulator/src/pet/kf_pet_session.cpp:259-283`) — a sleeping pet can be fed,
played with, rested and bathed exactly as if it were awake, full effect,
full reaction roll, full `care_actions_taken` increment. No ADR claims this
is blocked, and `examples/creature_demo/creature.lua`'s Home screen doesn't
currently wire any of the four care buttons up at all (only wake/tuck-in are
reachable from Home), so this may not be reachable from the shipped demo
today — but the Core-level and session-level surface is open, and nothing
prevents a future screen (or a different cartridge) from calling
`pet.feed()` etc. on a sleeping creature. Given ADR 0053's whole premise is
that sleep protects a pet *without* requiring active care, a future screen
that lets a player rack up free care credit on a sleeping pet at 3am would
quietly undercut that. Worth a one-line confirmation from Chris on whether
this is intended; not fixed here since it's a scope/design call, not a
defect against any stated requirement.

## Checked and found correct

- **Save format v11**: `pack()`/`unpack()` symmetry and `KF_PET_SAVE_BYTES`.
  Summed every `put_*`/`get_*` call in `pack()`/`unpack()` by hand: 1
  (version) + 4+4+4 (needs) + 1 (poop_count) + 4 (seconds_until_next_poop) +
  4 (dirtiness) + 4 (neglect_seconds) + 1+1 (sick, dead) + 1+8
  (last_advanced) + 1+1+1 (stage, teen_form, adult_branch) +
  8+8+8+8+8 (the five whole-life u64 accumulators) + 4 (care_recency_window)
  + 1 (base_trait) + 4 (care_actions_taken) + 1+1 (last_reaction,
  last_care_action) + 1+1 (asleep, tucked_in) + 4+4+4+4 (the four new v11
  fields) = **109**, matching `KF_PET_SAVE_BYTES` in `kf/pet.h` exactly.
  Field order in `pack()`/`unpack()` matches one-for-one. All four new v11
  fields (`hunger_floor_mp`, `happiness_floor_mp`, `energy_floor_mp`,
  `dirtiness_cap_mp`) are present in both `pack()` and `unpack()`, last in
  the layout, consistent with the version-bump comment.

- **"Nothing new can kill a sleeping pet" (ADR 0053 section 5)**. Verified,
  not assumed: `grep -n "dead = true" hakoniwaos/src/pet.cpp` finds exactly
  one site (`pet.cpp:1190`), inside the neglect/sickness block that runs
  once per `apply_stage_segment()` call regardless of asleep status, driven
  by the segment's *aggregate* `neglect_seconds` vs. `effective_death_seconds`
  — not a moment-by-moment trace. The one documented exception (death from
  neglect accrued during the *awake* portion of a segment that later also
  touches a night) is real and is exactly what the code does; no other path
  sets `dead`.

- **Dirtiness cap vs. the dirtiness neglect threshold.** The highest
  overnight dirtiness cap band (`kOvernightDirtinessCapUnwellNotTuckedInMp`,
  60000mp/60%) stays below `KF_PET_DIRTY_STINK_MP` (80000mp/80%, what
  `is_neglected()` compares `dirtiness_mp` against) in every band — so a
  properly-capped sleeping pet can never trip `is_neglected()` via the
  dirtiness channel, consistent with "never neglected while asleep" for
  that axis.

- **The four debug-clock changes are mutually consistent.** `DROWSY =
  21:50:05`, `BEDTIME = 22:00:05`, `MORNING = 07:00:05` — single source of
  truth in `kf_pet_session_debug_clock_target()`
  (`simulator/src/pet/kf_pet_session.cpp:380`), consumed identically by the
  desktop debug window (`sdl_debug_window.cpp`) and the ESP32 bridge
  (`ports/esp32/main/kf_dbg_bridge.cpp`); `kf_pet_session.h`'s own enum
  comment (`21:50:05 -- inside the ten-minute...`) matches the `.cpp`. The
  mutating-command-count comments (`kf_dbg_bridge.h`/`.cpp`/`CMakeLists.txt`)
  consistently describe 12 total, with the "ten mutating commands" phrase in
  `CMakeLists.txt` correctly meaning "the other ten, i.e. not counting
  BTN/BTNHOLD" (12 − 2 = 10) rather than a stale total — checked the
  arithmetic explicitly rather than assuming the word "ten" was wrong.

- **`KFDBG STATE`'s buffer-size arithmetic (ADR 0054)**, independently
  recomputed with a small script against the actual format string in
  `ports/esp32/main/kf_dbg_bridge.cpp` (not eyeballed): 33 specifiers
  exactly as claimed (1×`%d`, 18×`%lu`, 4×`%llu`, 3×`%u`, 2×`%zu`, 5×`%s`),
  literal text exactly 463 bytes, worst-case substituted width 4×20 + 24×10
  + 5×5 = 345, plus the trailing NUL = **809**, matching the comment exactly
  and clearing `json[1024]` with 215 bytes of margin as claimed.

- **RNG determinism across today's new `kf_rng_below()` call sites**
  (the overnight floor rolls). Reviewed every place two `kf_pet_advance()`
  sequences are compared in the same test process: `run_pet_check()`'s
  offline-vs-direct equivalence case (reseeds to `0x0FF11E5E` before each of
  the two sequences), `run_pet_sleep_check()`'s `floor_band_trial()` helper
  (reseeds per trial) and its explicit determinism case (g) (reseeds to the
  same value twice, asserts byte-identical floors). Did not find an
  un-reseeded comparison among today's additions — the one shared-stream
  collision the team already found and fixed (documented in ADR 0053) stays
  fixed.

- **Poop suppression while asleep is non-vacuous** — verified by actually
  breaking it, not just reading it. Temporarily changed
  `hakoniwaos/src/pet.cpp`'s poop-generation condition from
  `awake_seconds_in_segment >= state->seconds_until_next_poop` back to
  `segment >= state->seconds_until_next_poop` (i.e., reverted ADR 0053's own
  fix), rebuilt a standalone probe directly against the real `pet.cpp`, and
  ran a pet asleep for a full 9-hour night with a 1-hour poop interval:

  ```
  poop_count after a whole night asleep = 8 (expected 0)
  seconds_until_next_poop = 104 (expected 1800, unchanged)
  FAIL (poop leaked overnight)
  ```

  Reverted immediately after; `git diff hakoniwaos/src/pet.cpp` is empty —
  the file matches `HEAD` exactly, confirmed after the edit.

- **`kf_pet_wants()`'s sleep gate vs. the wake transition.** `state->asleep`
  is always recomputed at the very end of `apply_stage_segment()`, before
  the game layer ever queries `kf_pet_session_wants()`/`pet.wants()` for
  that frame — found no path where a want could be read against a stale
  `asleep` value. Separately, `kf_pet_session_frame()` derives its elapsed
  time from `kf_time_mono_us()` (a real monotonic clock), never from
  `kf_time_wall()` — so a backward jump from the new "Sync Clock" button
  cannot produce negative elapsed time in live play; `kf_pet_load_and_advance()`
  and `kf_pet_session_debug_set_clock()` handle a backward wall-clock move
  by direct-assignment / explicit clamp-to-zero respectively, not by
  computing a negative delta anywhere.

- **"the drowsy hour" phrasing** survives in a few comments
  (`creature.lua:474`, two spots in `headless_main.cpp`) as informal
  shorthand for the drowsy window, not a technical claim of a full hour —
  read each in context; none assert a duration or make an assertion that
  would be wrong. The genuinely load-bearing stale references (ADR 0049,
  the plan doc) are already annotated with a pointer to ADR 0052 rather than
  silently rewritten, per `CLAUDE.md`'s own rule — checked, present.

## Not independently re-verified

Everything under "Not verified" in ADR 0048/0051/0052/0053/0054 themselves
(anything needing real hardware) is unchanged by this audit — this was a
desktop/headless review only, per the task brief.

## Note on a concurrent build break

Partway through this audit, `git status` showed uncommitted changes
appearing in the shared tree (`hakoniwaos/src/app.cpp`,
`simulator/CMakeLists.txt`, new `kf/hal/audio.h` and friends) — another
agent's audio work landing live. For a few minutes this left the shared
`kamiframe-headless`/`kamiframe-sim` build target broken (`kf_audio_init`/
`kf_audio_shutdown` referenced but not yet linked). This is unrelated to the
sleep-stack commits under review and not something this audit touched or
fixed — noted here only so a "why did the build fail once" question doesn't
get attributed to the sleep work. The break-test above was done by compiling
`pet.cpp` directly against a small standalone harness specifically to avoid
depending on that shared, moving target. By the time of the final `ctest`
runs (5x total, last two after the audio change landed cleanly), the shared
build was green again and stayed green.

## Summary

- **11 items checked**, of which **2 are findings** (one Medium-High, one
  Low-Medium/advisory) and **9 are verified correct** (including two
  non-vacuity break-tests performed directly, not merely read).
- **Most important finding**: the overnight floor (ADR 0053) measurably
  inflates the CHILD-stage care average for a completely unattended,
  sleeping pet in live play — nearly 3x higher than the same neglect
  pattern scores with no wall clock established — and `hokorimaru_check`
  cannot see this because it never establishes a wall clock at all. This is
  a real, reproducible interaction between two already-shipped mechanisms,
  not a hypothetical; it needs a decision from Chris (accept, or change how
  the integral weights a floor-clamped value), not a mechanical fix.
- **Suite status: genuinely green.** 51/51, run 5 times across the session,
  no flakiness, no host-wall-clock dependency found in the sleep-related
  checks.
- No fixes were made to the commits under review — findings are reported,
  not patched, per the task's "fix only what is small, obviously correct,
  and clearly a defect" instruction; neither finding here is that.
