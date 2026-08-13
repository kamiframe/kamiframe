# ADR 0052: The drowsy window becomes ten minutes, and tuck-in gets a real payoff

**Status:** Accepted
**Date:** 2026-08-11

## Context

Chris, after testing on hardware and in the simulator, 2026-08-11:

> "extend the 'drowsy' timeframe to start 10 minutes before actual bedtime.
> During this drowsy time, the pet moves a little bit then makes the drowsy
> face animation with zzz appearing briefly, then it moves a bit more, and
> repeats until it's bedtime. If you notice while it's drowsy and hit the b
> button, the command tucks it in for a boost in care needs the next day
> when it wakes up (meaning it's more fullfilled when it wakes up compared
> to if it puts itself to bed automatically)."

Three separate changes follow from this, and they land at different
altitudes:

1. **Drowsy shrinks from a whole hour to the ten minutes before bedtime**,
   and stops being a `creature.lua` literal (`local kDrowsyHour = 21`,
   duplicating `hakoniwaos/src/pet.cpp`'s own `kNightStartHour`) — a real
   Core query, because the tuck-in gate below needs the exact same answer
   Core uses, not a second, independently-hardcoded one.
2. **The nodding-off loop is presentation only** — reuses the existing
   `*_sleeping_*` art, no new art, no PixelLab call.
3. **The tuck-in bonus is Core, and it changes the save format** — the
   payoff is deferred to next morning, has to survive the device being
   switched off overnight, and has to work through the offline fast-forward
   path, not just live play. None of that can live in Lua or in session
   memory.

## Decision

### 1. `kf_pet_drowsy()` — a pure query, same shape as `kf_pet_wants()`

```c
bool kf_pet_drowsy(const kf_pet_state *state);
```

False while dead, asleep, an egg (eggs do not sleep — ADR 0048 — so they
have no bedtime to be drowsy before), or with no clock established yet.
Otherwise true for exactly `kDrowsyWindowSeconds` (600, ten minutes)
immediately before `kNightStartHour`. Both constants live in
`hakoniwaos/src/pet.cpp`'s anonymous namespace, next to each other — the
window is derived from `kNightStartHour`, not a second hardcoded `22`.

Like `asleep`, this is **not a new saved sub-state**: it is recomputed on
demand from `state->last_advanced`, exactly the "previous value in, fresh
value out" purity `kf_pet_wants()` already established. Unlike `asleep`
(hour-granularity via `kf_clock_seconds_in_daily_window()`), the ten-minute
window needs minute precision, so `kf_pet_drowsy()` reads `state->
last_advanced` through `kf_civil_from_epoch()` and does its own half-open
interval test — generalised for a midnight wrap even though the current
constants (`kNightStartHour=22`, ten minutes) never actually need it, the
same "handle the wrap, do not special-case the values that happen not to
need it" discipline `kf_clock_seconds_in_daily_window()` itself follows.

`creature.lua`'s old `local kDrowsyHour = 21` is gone. `pet.drowsy()`
(`sdk/lua/kf_lua_port.cpp`) wraps `kf_pet_drowsy()` directly.

### 2. The nodding-off loop — dt_ms-driven, no new state in Core

While drowsy (and not yet tucked in): wander for `kNodWanderMs` (10s), then
freeze in place and show `<stage><branch>_sleeping_s` with a ZZZ for
`kNodPoseMs` (4s), repeat. Both are feel, picked to read well against a
600-second real window (roughly 42 cycles) — not tuned against anything.

Driven by `dt_ms` (real frame time), not any pet/game-time clock — a
deliberate choice with a real consequence, noted rather than hidden: with
the debug speed multiplier turned up, Core's own ten-minute window can pass
in a couple of real seconds, and this loop will show one nod, a partial
one, or none at all. That is the multiplier working as designed against a
loop that is intentionally anchored to real wall-clock pacing, not a bug in
either.

The frozen position is captured once, on the frame the pose phase begins
(`nod_x, nod_y = creature.x(), creature.y()`) — the underlying wander in
`kf_creature_presenter.cpp` keeps running every frame regardless (it only
freezes on Core's own `asleep`, per that file's own header comment), so
without freezing the drawn position separately the creature would appear to
slide while "stopped".

### 3. The tuck-in bonus — Core, saved, and paid at the asleep → awake edge

```c
void kf_pet_tuck_in(kf_pet_state *state);   /* no-op unless kf_pet_drowsy() */
```

A new saved field, `kf_pet_state::tucked_in` (bool), set by
`kf_pet_tuck_in()` — itself a thin wrapper: `if (!kf_pet_drowsy(state))
return;` is the entire gate, so a dead, sleeping, wide-awake, or egg-stage
creature can never be tucked in, without repeating any of those checks
locally.

A new config field, `kf_pet_config::tuck_in_wake_bonus_mp` — **10000
(10%), FEEL NOT ENGINEERING, flagged for Chris to tune on the board.**
Sized against `wake_happiness_cost_mp` (5000): double that cost, applied to
all three needs instead of one, so the bonus reads as clearly worth the one
extra button press without dwarfing a full night's ordinary decay.

**Where the bonus is paid, and the bug this caught.** The first
implementation detected the transition as "was `state->asleep` true before
this `apply_stage_segment()` call, and false after" — which works for the
hour-at-a-time segments `run_pet_sleep_check`'s own case 1 already uses,
but `kf_pet_advance()`'s segments are bounded by **stage** transitions, not
by night-window crossings (see `hakoniwaos/src/pet.cpp`'s own header
comment), so a single call can span an entire night start to finish —
exactly what a real offline fast-forward does. A segment like that starts
awake and ends awake with a whole night in between; "awake before, still
awake after" looks identical to a segment that was never asleep at all, and
the transition — and the bonus — were silently missed. Caught by trying it:
a single ~17-hour jump (evening tuck-in to the next afternoon) failed to
pay the bonus at all under the before/after comparison.

The fix: find the next `kNightEndHour:00:00` at or after the segment's own
start, and check whether it falls at or before the segment's own end — a
direct, closed-form "did a morning happen inside this segment" test,
independent of what `state->asleep` reads at the very end. Only the FIRST
such crossing matters (`apply_tuck_in_bonus_if_due()` clears `tucked_in`
the moment it pays), so a segment spanning several mornings at once (a pet
away for a week) still pays exactly once.

Deliberately **only** the asleep → awake edge, and deliberately **not**
`kf_pet_wake()`'s own deliberate-early-wake edge — see that function's
updated header comment: paying the bonus on every early wake would let a
tucked-in creature be woken again within the same minute for a net-positive
trade (a small happiness cost against a bonus on three needs), which is not
what "wakes up" means in Chris's own framing. A creature woken early keeps
`tucked_in` set until whichever night it actually sees the natural
transition.

### 4. Save format: `kSaveVersion` 9 → 10, `KF_PET_SAVE_BYTES` 92 → 93

One new byte, `tucked_in`, appended after `asleep`. A version-9 save is
**refused**, not defaulted — matching every prior version bump's policy in
this file (a silently-defaulted `false` would happen to be correct for a
creature that was never tucked in and silently wrong for one that was).
**No migration.** Chris was asked directly and declined one for now: "no
save migration needed now during development. I'll let you know if I need
it at some point."

### 5. The debug button

`kf_pet_session_debug_clock_target(KF_PET_DEBUG_CLOCK_DROWSY)` moved from
21:00:05 (inside the old whole-hour window) to 21:50:05 (inside the new
ten-minute one) — same "five seconds past the boundary" convention the
other two points already use. `clock_jump_check`'s "the Drowsy button's own
target lands awake" assertion still passes, and now also asserts
`kf_pet_drowsy()` is true there — the old whole-hour window made "awake"
alone enough to trust the button; the new ten-minute window does not.

## Non-vacuity

Every new assertion was broken and watched fail before being trusted (see
the implementation report for the exact failure messages); two are worth
recording here because they caught real bugs in this work itself, not just
in a hypothetical regression:

- **The offline-transition bug above** was found this way, by the very
  test meant to prove the offline path works, before the fix landed.
- **The "cannot be claimed twice" test was itself vacuous on the first
  attempt.** It advanced both a tucked-in pet and a self-slept control by
  a second 10-hour block, expecting the gap between them to stay fixed.
  Disabling the flag-clear in `apply_tuck_in_bonus_if_due()` left it
  green anyway — the second 10-hour block started at 07:50 (the first
  night's own wake time) and only reached 17:50 the same day, never
  crossing a second night at all, so nothing was ever re-paid regardless
  of the bug. Fixed by making the second block 24 hours, which genuinely
  crosses a following night's own morning — only then did disabling the
  flag-clear make the test fail.

## Consequences

- Every place that used to say "the drowsy hour" or "21:00" for this
  feature now says "the ten-minute drowsy window" or "21:50" —
  `kf_pet_session.h`/`.cpp`, `sdl_debug_window.cpp`, `headless_main.cpp`'s
  comments, and the screens/clock/sleep plan.
- A version-9 save on a developer's machine resets to a fresh pet the next
  time this build runs — the same accepted cost every prior version bump
  already took.
- The tuck-in bonus size (10%) is explicitly flagged as a first guess, the
  same status every other illustrative number in `kf_pet_default_config()`
  already carries.
