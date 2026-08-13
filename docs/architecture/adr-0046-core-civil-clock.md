# ADR 0046: `kf/clock.h` — civil time in Core, integers only, no offset

**Status:** Accepted
**Date:** 2026-08-13

## Context

Task 3 of the screens/clock/sleep plan builds the piece
Task 4 (the Settings screen's 12-hour clock) and Task 6 (sleep's 22:00-07:00
night window) both block on: turning a wall-clock second count into an hour
and minute, and answering "how many seconds of this span fall inside a daily
clock-time window". Both consumers must get the same answer to "what hour is
it" or the clock on screen and the hour the pet falls asleep drift apart from
each other — which is why this lives once, in Core, rather than twice, once
per consumer.

This task started against a brief written before a timezone decision landed.
The brief's original API carried a device-wide UTC offset
(`kf_clock_set_utc_offset_seconds()` / `kf_clock_utc_offset_seconds()`) that
`kf_civil_from_epoch()` / `kf_epoch_from_civil()` would apply. Partway through
this task, Chris settled the question directly (relayed mid-task, recorded in
the plan under "Timezone: settled by Chris"):

> The user sets the clock by hand. The RTC holds LOCAL time directly. No
> timezone database, no stored UTC offset, no conversion layer. [...] "the
> user will be able to change the clock themselves at first. Eventually I do
> want to hook it to the internet so it can tell what local time is. Feature
> for later though."

The API below reflects that decision, not the brief's original draft. No
offset function was ever implemented — the draft was replaced before it had a
body, rather than shipped and removed later, which is exactly what the
decision's own reasoning (below) calls for.

## Decision

### No offset field, anywhere, not even for later

`kf/clock.h` carries no UTC offset, no timezone, and no conversion layer of
any kind. Every function takes and returns a plain `epoch_seconds` that IS
the wall clock's own reckoning — `kf_time_wall()`'s `epoch_seconds`, unmodified.
Three consequences, each one a direct line from Chris's reasoning:

1. **A field nothing sets is a field that is wrong the first time something
   reads it.** There is no UI, no protocol, and no plan to determine an
   offset today — the device has no network and no location. An offset field
   "for later" would default to something (0, almost certainly) that is
   simply incorrect for anyone not at that meridian, silently, forever, until
   a feature that does not exist yet sets it.
2. **A field in a persisted format has to be carried forever.** Nothing in
   this task persists anything (kf/clock.h is stateless — see below), but the
   API shape here is what Task 4 will persist, and an offset baked into that
   shape now would be a save-format commitment made for a feature not yet
   designed.
3. **"Local" can only honestly mean one thing on this device today**: what
   the person holding it told the Settings screen. Modeling that as
   `epoch_seconds` being local directly, rather than as "UTC plus a stored
   correction", is not a simplification of the honest design — it IS the
   honest design. There is no UTC anywhere in this system to be an offset
   *from*.

### Consequently: this module is completely stateless

With no offset to hold, `clock.cpp` has no file-static variable at all —
contrast `kf/rng.h`'s single `g_state`, which exists because a device
genuinely has one running random sequence. Every function here is a pure
conversion over its own arguments: same input, same output, forever, with
nothing to initialise and nothing that could be left uninitialised. That
statelessness is also the answer to Chris's third point (below) about a
sync eventually moving the clock: a stateless function cannot be surprised
by a jump, because it never remembers a previous call to be surprised by.

### Why not `localtime()` — still the right question, mostly the same answer

`localtime()` from `<ctime>` ships on both the host and ESP-IDF and would
compile cleanly on both. It is still wrong here, for reasons that hold
independent of the offset decision:

- **The HAL boundary.** `CLAUDE.md`'s rule is that Core (`hakoniwaos/`) talks
  to the HAL and nothing else. `localtime()` reaches around that into libc's
  own global state.
- **`TZ` is process-global state neither target sets.** Before the timezone
  decision this mattered because it made the same epoch legally produce two
  different answers depending on load order. After the decision it matters
  less for *this specific* concern (there is no timezone to convert), but it
  is still exactly the kind of hidden, load-order-dependent global state this
  project's headless checks exist to eliminate, and reaching for a
  timezone-aware function to do timezone-*free* work is the wrong tool
  regardless.
- **Locale machinery, on a build budgeted in kilobytes.** `kf/budget.h` and
  `tools/check_no_heap.py` both hold `hakoniwaos/` to a much smaller and more
  predictable footprint than libc's calendar code assumes.

### The integer algorithm: Howard Hinnant's `days_from_civil` / `civil_from_days`

`hakoniwaos/src/clock.cpp` implements the well-known constant-time,
branch-free civil-calendar algorithm described at
<http://howardhinnant.github.io/date_algorithms.html> — `days_from_civil()`
and `civil_from_days()`, converting between a `(year, month, day)` triple and
a day count since 1970-01-01. It is reproduced in full in the source (not
pulled in as a dependency) because it is roughly twenty lines each direction,
integer-only, and — the property that matters most here — **proleptic**: it
does not special-case any particular year range, which is what lets
`kf_civil.year` legally be negative (a date before year 0) without the code
needing a second path. That generality is not decoration; it is what makes
"never assume the epoch is a small, recent, positive number" true by
construction rather than by convention.

`kf_civil_from_epoch()` and `kf_epoch_from_civil()` are thin wrappers: split
`epoch_seconds` into a day count and a seconds-of-day remainder (or the
reverse), and hand the day count to the algorithm above.

**Floor division, not truncation.** C's `/` truncates toward zero, so
`-3600 / 86400` is `0` when the correct floored answer is `-1` — one second
before 1970-01-01T00:00:00 belongs to the day *before* day 0, not to day 0.
`clock.cpp`'s `floor_div()` is the one place this is handled, and every
day/remainder split in the file routes through it rather than reaching for
`/` and `%` directly a second time. This still matters with no offset in the
picture: any epoch before 1970 (a bogus RTC value, or the round-trip check's
own `-3600` case) is enough to trigger it, which is why the round-trip check
below includes exactly that case.

### The analytic window rule: a single axis shift turns wrap and no-wrap into one formula

`kf_clock_seconds_in_daily_window(from, to, start_hour, end_hour)` never
branches on whether the window wraps midnight. The trick:

1. **Duration**, folded into `[0, 24h]` with
   `((end_hour - start_hour) % 24 + 24) % 24 * 3600` — this single expression
   gives the right answer whether `end_hour > start_hour` (a same-day window)
   or `end_hour <= start_hour` (a wrap), and folds `start_hour == end_hour` to
   an empty window (0 seconds) rather than a special-cased full day, matching
   the header's documented choice.
2. **Shift** the whole time axis by `start_hour * 3600` seconds. A window
   that reads "22:00 through 07:00" becomes, on the shifted axis, "0:00
   through 09:00 of a day that itself begins at 22:00" — a plain,
   non-wrapping window at the very start of its cycle. There is no code
   anywhere that treats the wrap specially, because after the shift there is
   no wrap left to treat specially.
3. **`window_cumulative(t, period, duration)`** counts window-seconds elapsed
   by time `t`: whole cycles times `duration`, plus whichever partial cycle
   `t` currently sits inside (`min(rem, duration)`, where `rem` is `t`'s
   position within its own cycle via `floor_div`/remainder). This function is
   monotonic non-decreasing in `t` by construction.
4. **The seconds inside `[from, to)`** is exactly
   `window_cumulative(to, ...) - window_cumulative(from, ...)`. That single
   subtraction is the "whole windows plus two partials" the brief asks for:
   the two partials are the fractional cycle `from` sits in and the
   fractional cycle `to` sits in, and everything between them collapses into
   the whole-cycle term — there are only ever two partial windows total
   across a call, not one per day the span happens to touch, which is what
   makes a fourteen-day span (Task 6's actual offline-sleep case) one
   subtraction rather than fourteen.

This shape is what makes "exactly 24 hours from an arbitrary instant is
exactly `9 * 3600`" true unconditionally: adding one full period to `t`
advances `window_cumulative` by exactly `duration`, regardless of where in
the cycle `t` started, so the two-instant difference across exactly one
period is always the bare duration. The check below verifies this from four
different starting instants rather than trusting the derivation on its own.

### No DST — a decision, not an oversight

Stated in `kf/clock.h`'s header rather than left implicit: there is no
daylight-saving handling anywhere in this module, and cannot be, because
there is no timezone concept for a DST rule to attach to. The plan's own
"Timezone: settled by Chris" section is where this is decided and recorded;
this module simply has nothing to add beyond converting whatever the RTC
says.

### Kept total and honest, for the sync that is wanted later

Chris named a real future feature: hooking the device up over WiFi so it can
learn the correct local time itself, rather than requiring a hand-set clock
forever. `kf/clock.h` is deliberately shaped so that feature, when it lands,
is bounded work: it only has to **call `kf_time_set_wall()` with a new
value** — exactly what the Settings screen already does today — because this
module never attaches timezone meaning to a stored timestamp that a sync
would then have to reinterpret. There is nothing to migrate and no format to
version, because there is no offset baked into anything this module reads or
writes.

**This module also does not assume the wall clock moves smoothly.** It is
stateless, so it cannot be — there is no "last known time" anywhere in it to
be contradicted by a jump. That property is already load-bearing today, not
just for the future sync: the Settings screen already lets the owner hand-set
the clock, which is itself a jump. No comment in this file or its header
claims time advances one second per second, because it demonstrably does not
have to.

## The proof

- `run_clock_check()` (`simulator/src/headless/headless_main.cpp`,
  `--verify-clock`) covers, in order:
  1. **Round trip** — `kf_epoch_from_civil(kf_civil_from_epoch(e)) == e` for
     seven epochs: a leap day (`1709208000`), both sides of a year boundary
     (`1767225599`, `1767225600`), an hour either side of midnight
     (`1786489200`, `1786496400`), epoch `0`, and a negative epoch
     (`-3600`) — the one case that actually exercises floor division rather
     than truncation.
  2. **Hand-computed civil values** for four of those epochs (one more than
     the plan's minimum of three), checked against the literal output of
     `python3 -c "import datetime as dt; print(dt.datetime.utcfromtimestamp(EPOCH))"`
     run while writing the check:
     - `1709208000` → `2024-02-29 12:00:00` (leap day)
     - `1767225599` → `2025-12-31 23:59:59` (year boundary, last second)
     - `0` → `1970-01-01 00:00:00`
     - `-3600` → `1969-12-31 23:00:00` (negative epoch)
  3. **Window arithmetic**, 22:00-07:00, against hand-computed answers: a
     span entirely inside the night (3600s), entirely outside (0s), starting
     mid-night and running past the window's end (8h of 9), ending mid-night
     (1.5h), exactly 24 hours from four different starting instants (always
     `9 * 3600` = `32400`), 14 whole days (`453600`), and 14 days crossing a
     month boundary (also `453600` — this case exists to demonstrate the
     window function is calendar-agnostic, not to find a different number).
     All boundary epochs were computed the same way as group 2's, with
     `python3 -c "import datetime as dt; print(int(dt.datetime(Y, M, D, h, m,
     s, tzinfo=dt.timezone.utc).timestamp()))"`.
  4. **Anti-vacuity**, verified by hand rather than left in the source:
     `kf_clock_seconds_in_daily_window()`'s body was temporarily replaced
     with `return 0;` and the suite re-run. Every non-zero assertion in
     group 3 went red:
     ```
     FAILED: 22:00-23:00 is entirely inside the night: 3600s
     FAILED: 23:00 -> next 08:00 spends 8h of it inside the night
     FAILED: 21:00 -> 23:30 spends 1.5h of it inside the night
     FAILED: exactly 24h from epoch 1786449600 must be exactly 9*3600 = 32400, got 0
     FAILED: exactly 24h from epoch 1786485600 must be exactly 9*3600 = 32400, got 0
     FAILED: exactly 24h from epoch 1786491000 must be exactly 9*3600 = 32400, got 0
     FAILED: exactly 24h from epoch 1786514400 must be exactly 9*3600 = 32400, got 0
     FAILED: 14 days is 14 whole nights: 453600s
     FAILED: 14 days across a month boundary is still 14 whole nights: 453600s
     FAIL
     ```
     — nine of the check's assertions, correctly, while the "entirely
     outside" case correctly stayed green (0 is the right answer for that
     one even with a stubbed-zero function, so its silence is expected, not
     a gap in the stub test). The real implementation was restored
     immediately after and reconfirmed green.
- Before any implementation existed, `cmake --build build -j8` produced the
  expected undefined-symbol link error for all three functions
  (`kf_civil_from_epoch`, `kf_epoch_from_civil`,
  `kf_clock_seconds_in_daily_window`), confirming the check was written
  against the header alone and fails for the right reason.
- `ctest --test-dir build` is **45/45** — the desktop baseline of 44 plus
  `clock_check`. `headless_determinism` (`2aceae654b21ca1b`) and
  `headless_fullscreen` (`3fbdcabfba49e9ef`) are unmoved, as expected: this
  task draws nothing and touches no file either golden check depends on.
- `python3 tools/check_no_heap.py .` stays clean. `clock.cpp` has no
  `float`/`double` anywhere and no file-static state at all (searched by
  hand, not just by the heap checker, since the no-state property is a
  design claim this ADR makes above).
- The ESP-IDF cross-compile (`-DKF_PANEL=ili9341`) is clean, zero warnings.
  Firmware image: `672,672` bytes (57% of the app partition free) — the same
  figure ADR 0043 recorded for `KF_HOME_SCREEN=cpp` before this task, which
  is expected rather than coincidental: nothing in `ports/esp32/main` calls
  any `kf/clock.h` function yet, so the linker's dead-code elimination drops
  `clock.cpp`'s object from the image entirely. The 672,672-byte figure is
  therefore evidence that this task added a compiled, tested module at
  effectively zero cost to the shipped firmware until something calls it —
  which is exactly the point of building it here, ahead of any caller.

## Not verified

**Nothing calls `kf/clock.h` yet.** No Lua binding, no Settings screen, no
sleep logic — this task is deliberately self-contained, per the plan's own
"How you would know it worked" for Task 3. Task 4 is the first caller (the
time API in Lua and the Settings screen, which will also be the first thing
to persist a wall-clock value under its own storage key) and Task 6 is the
second (sleep's night-window accounting, which is exactly one call to
`kf_clock_seconds_in_daily_window()`). Until one of those lands, this module
has been proved correct against hand-computed cases and against itself, but
never against a real Lua script, a real button press, or real hardware.

The timezone decision itself is Chris's, recorded verbatim in the plan under
"Timezone: settled by Chris"; this ADR implements it but did not make it.

### Superseded in part

Two claims above do not hold up against the tree, both worth flagging because
they are the load-bearing argument for why the no-offset design is safe.

**"exactly what the Settings screen already does today"** (the "Kept total
and honest" section) and **"the Settings screen already lets the owner
hand-set the clock"** (two paragraphs later) were never accurate — this
document's own "Not verified" section above says plainly that no Settings
screen exists. There is still no Settings screen as of this correction. The
underlying argument (a future sync only ever has to call
`kf_time_set_wall()`, the same call the Settings screen editor will make) is
sound and does not depend on the screen existing yet; only the present-tense
"already does" was wrong.

**"the time API in Lua and the Settings screen, which will also be the
first thing to persist a wall-clock value under its own storage key"** is
also superseded: the screens/clock/sleep plan's
Task 4 was corrected to *not* add a storage key — nothing about the clock
persists outside the RTC itself, "ready for" internet sync or otherwise. See
that plan's own note against adding one.
