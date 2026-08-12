# ADR 0054: `KFDBG CLOCK`, `KFDBG RTC`, and sleep-state STATE parity

**Status:** Accepted
**Date:** 2026-08-11

## Context

Chris, after sleep (ADR 0048), the drowsy window and tuck-in bonus (ADR
0052), overnight floors (ADR 0053), and the home-screen wall clock all
landed:

> "Make sure all of these sleep related changes and new debug window sleep
> related functions (and the digital clock on the wall) are all also
> implemented on the hardware version of the pet and debug window that
> remotely controls the hardware version. I want feature parity."

Two claims needed checking before any code changed, per CLAUDE.md's rule on
verifying a subagent's (or, here, an earlier task's own) confident claims:

1. **Is sleep itself missing from the ESP32 build?** No. Sleep, the drowsy
   window, the overnight floors, the seven futon sprites, and the
   home-screen clock all live in `hakoniwaos/` (Core) or Lua, both of which
   are compiled and packed into the firmware unconditionally --
   `pet.cpp` is in `ports/esp32/main/CMakeLists.txt`'s component source
   list, and the ESP32 build flashes `examples/creature_demo/assets.kfpack`,
   the same pack the desktop simulator uses, futons included. **A rebuild
   carries all of it.** Nothing in this task re-implements any gameplay
   logic.
2. **Is `KF_PET_SESSION_ENABLE_DEBUG_CONTROLS` actually on for the ESP32
   build**, the flag `kf_pet_session_debug_clock_target()`/`_set_clock()`
   ride? Checked directly against `ports/esp32/main/CMakeLists.txt` rather
   than assumed: `target_compile_definitions(${COMPONENT_LIB} PRIVATE
   KF_PET_SESSION_ENABLE_DEBUG_CONTROLS=1 ...)`. Confirmed on, and confirmed
   load-bearing already (it is what `KFDBG ADVANCE`/`RESET`/`MULT`/`JUMP`
   already reach through).

**The actual gap: the serial debug bridge.** `KFDBG` had 16 verbs -- `PING
SHOT STATE SCANLINE BTN BTNHOLD ADVANCE RESET MULT VSYNC FEED PLAY REST BATH
FLUSH JUMP` -- and not one touched the clock. So the desktop debug window's
Drowsy/Bedtime/Morning buttons had no hardware equivalent, and
`tools/kf_panel.py` (the window that drives a real board) could not offer
them, or show whether a real pet was asleep at all.

`KFDBG RTC` specifically closes a second, older gap: Task 5 of
the screens/clock/sleep plan specified this
exact command and never built it, instead reading the DS3231's state off a
boot log line during its one bench run (see that plan's own STATUS block).
That proved the coin cell holds time across a real power cut, but left no
way to compare the RAM clock against the physical chip **at will**, and no
way to observe the chip's OSF (oscillator-stopped) flag remotely -- which is
what the plan's still-open negative case (coin cell removed) needs.

## Decision

### 1. `KFDBG CLOCK` -- mutate tier

`KFDBG CLOCK DROWSY|BEDTIME|MORNING` and `KFDBG CLOCK EPOCH <seconds>`, both
gated by `require_mutate_enabled()` exactly like `ADVANCE`/`RESET`/`MULT`/
`JUMP` -- it changes the pet's own notion of what time it is, the identical
reasoning ADR 0035 already applies to every other time-control verb. The
mutating set grows from eleven commands to **twelve**.

Both branches call straight into `kf_pet_session_debug_clock_target()` (for
the three named points) and `kf_pet_session_debug_set_clock()` -- **neither
time is reimplemented in this file.** The three times (21:50:05, 22:00:05,
07:00:05) are defined exactly once, in `kf_pet_session.h`, specifically so
the desktop debug window's buttons and this KFDBG verb can never drift
apart the way an earlier version of the desktop buttons once did against
their own headless test (see `kf_pet_debug_clock_point`'s own comment for
that history -- the first cut aimed ten seconds short of each transition
while the test checked ten seconds past it, and every assertion passed
while the buttons still did nothing visible).

`EPOCH <seconds>` is new relative to the desktop window, which only offers
the three named points -- it is what makes an arbitrary date settable on
the board, closing `ports/esp32/README.md`'s "no way to set the date from
the device" open question (the Settings screen edits hour and minute only,
per ADR 0026's bench run finding).

### 2. `KFDBG RTC` -- observe tier

Gated by `KF_DBG_BRIDGE_ENABLE` alone, never `KF_DBG_MUTATE_ENABLE` --
reading a chip's registers changes nothing. Reads the DS3231 **directly**
over I2C via one new accessor, `kf_esp_time_debug_read_rtc()`
(`ports/esp32/hal/kf_esp_time_debug.h`, implemented in `esp_time.cpp`
alongside the register map it already owns) -- **not** `kf_time_wall()`,
which is the in-RAM clock and never touches the bus again after boot.
Reading the RAM clock instead would make the whole command vacuous, which
is exactly the trap Task 5's own plan text called out.

Replies `json`: `present` (always `true` in a `json` reply -- see below),
`epoch`, `osf`, and the RAM clock's `wall`/`wall_valid` alongside, so a
human or script can compare the two in one line rather than a second
`KFDBG STATE` round trip racing a clock that could tick in between (and
which does not carry `wall`/`wall_valid` at all).

**No chip ever having answered at boot replies `err`, not `present:false`.**
Same "bail before building a reply with nothing to report" shape
`require_read_line()` already uses for `SCANLINE`/`VSYNC` on a panel with no
read line (ADR 0039) -- picked over a `present:false` JSON so a host script
never needs to branch on frame type AND a boolean to know "there is nothing
to read here," and so `tools/kf_debug_selftest.py` has a clean third case
(alongside a matching reply and an `osf==1` reply) that needs no hardware
to prove.

**No register map duplicated.** `kf_esp_time_debug.h` exposes exactly one
function; its implementation in `esp_time.cpp` calls the same
`ds3231_read()`/`ds3231_regs_to_epoch()` helpers `kf_time_init()` already
uses, not a second copy of the BCD-to-epoch conversion or the
MPU-6050-at-the-same-address disambiguation.

### 3. `STATE` gains `asleep`, `drowsy`, `tucked_in`

The desktop debug window has always had these in-process (it reads
`kf_pet_session_state()` directly); the hardware bridge never carried them
at all, so the panel had no way to show sleep state. `asleep`/`tucked_in`
are plain `kf_pet_state` fields; `drowsy` is derived via `kf_pet_drowsy()`
(`kf/pet.h`) -- the same function every other reader of drowsiness already
calls, not a second inference of it.

**Reply-buffer arithmetic, recomputed, not re-derived from the old
comment.** The old comment claimed 430 bytes of literal text, 342 bytes of
worst-case substituted value width, 773 total. Enumerating the actual
format string and every argument's cast in the current source (not
re-deriving the old figure, which no longer matches a literal+specifier
count of even the pre-this-task format string) gives:

- **Literal text** (every key name, quote, colon, comma -- no
  substitutions): **463 bytes**.
- **33 specifiers**: 4 × `%llu` (20 digits worst case) + 24 × `%lu`/`%zu`/
  `%u`/`%d` (10 digits) + 5 × `%s` (`"false"`, 5 chars) = 80 + 240 + 25 =
  **345 bytes**.
- Plus the trailing NUL `snprintf` always writes: **809 bytes**, the new
  worst case.

`json[1024]` is unchanged -- 809 clears it with 215 bytes of margin, still
enough for a field or two before this buffer is next in line to grow.
Verified by exhaustive enumeration (see the implementation report for the
Python cross-check against the literal file contents), not eyeballed.

### 4. Host side, same commit: `tools/kf_debug.py`, `tools/kf_debug_selftest.py`

Per the original Task 5 spec's own rule -- one wire contract in two
languages, landed together so the two sides cannot drift the way they have
before on this project:

- `kf_debug.py clock drowsy|bedtime|morning|<epoch>` and `kf_debug.py rtc
  [--json]`, following the existing subcommand style (`_expect()`, the
  `name`-or-`number` convention `jump`'s stage argument already uses).
- `kf_debug_selftest.py` gained `test_clock_command_building()` (all three
  named points, case-insensitivity, the epoch form, and an unknown-target
  rejection) and `test_rtc_command_decode()` (a healthy reply, an
  `osf==1` reply, and the `err`-when-no-chip case, plus `--json`
  passthrough) -- all wire-decoding proofs needing no hardware, the entire
  point of that file. `CLOCK` was also added to
  `test_mutate_gate_rejection_is_actionable()`, alongside the existing
  `FEED`/`JUMP` cases, proving its rejection is generic across `cmd_*()`
  rather than a special case.

### 5. `tools/kf_panel.py` parity

A new "Sleep cycle:" row inside the existing "Time controls" section --
**Drowsy / Bedtime / Morning**, same labels, same left-to-right order as
the desktop debug window's own row (`sdl_debug_window.cpp`'s `kButtons`),
so the two windows read the same. `asleep`/`drowsy`/`tucked_in` were added
to `STATE_FIELD_ORDER`, placed with the other per-pet stats rather than the
frame-budget numbers.

`--demo` mode's `FakeDevice`/`FakeTransport` gained matching (intentionally
shallow) support for `KFDBG CLOCK` -- flipping plausible sleep-state flags,
not modelling wall time -- so clicking the new buttons in demo mode does
not regress into "fake device doesn't understand" the way an unhandled
command otherwise would have.

## Non-vacuity

Every new Python-side assertion was broken (editing the implementation,
never the test) and watched fail, then restored. Representative examples,
exact failure output:

- `test_clock_command_building()`: changed `command = f"KFDBG CLOCK
  {point.upper()}"` to `f"KFDBG CLOCK {point}"` (lowercase, wrong wire
  word). Result: `[FAIL] clock drowsy -> KFDBG CLOCK DROWSY`,
  `[FAIL] clock bedtime -> KFDBG CLOCK BEDTIME`,
  `[FAIL] clock morning -> KFDBG CLOCK MORNING`,
  `[FAIL] clock DROWSY (uppercase) -> KFDBG CLOCK DROWSY` -- four failures,
  restored.
- `test_rtc_command_decode()`'s OSF verdict: replaced the `"VERDICT: OSF is
  set..."` text with a differently-worded line. Result:
  `[FAIL] osf==1 reply's VERDICT names OSF specifically` -- restored.
- `test_rtc_command_decode()`'s no-chip case: wrapped `cmd_rtc`'s `_expect()`
  call in a `try/except KfDebugError: print("no RTC detected"); return`,
  simulating a bug that swallows the device's rejection instead of
  propagating it. Result: `[FAIL] no chip answered raises KfDebugError, not
  a silent present:false` -- restored.

The C++ side (`handle_clock_point()`/`handle_clock_epoch()`/`handle_rtc()`,
the recomputed `STATE` buffer size) has no unit test harness on this
project (ESP-IDF component tests are out of scope here) -- its correctness
rests on: the ESP32 build compiling clean against real ESP-IDF headers, the
literal-format-string enumeration above being checked by an independent
script pass against the actual file contents (not hand-counted), and the
fact that `handle_clock_point()`/`handle_clock_epoch()` call the
already-tested `kf_pet_session_debug_clock_target()`/`_set_clock()`
functions directly rather than reimplementing any of their logic.

## Verified

- `ctest --test-dir build`: 51/51, unchanged from baseline -- this task
  touches no Core or simulator behavior, only comments (see
  `kf_pet_session.h`) and the ESP32 port.
- `python3 tools/kf_debug_selftest.py`: all checks pass, including the new
  `clock`/`rtc` coverage above.
- `python3 tools/check_no_heap.py`: 36 files, unchanged.
- `idf.py -DKF_PANEL=ili9341 build`: succeeds, no warnings. Firmware image
  536,336 bytes (`.bin`), 66% of the 0x180000-byte app partition free
  (1,036,528 bytes).
- `tools/kf_panel_layout_check.py`: passes in both its serial and demo
  passes, with the three new "Time control: Drowsy/Bedtime/Morning" rows
  present and unclipped (that script derives its required-control list
  from whatever buttons actually got registered at runtime, so this is a
  real layout proof, not a hardcoded expectation).
- A headless smoke test against `kf_panel.py`'s `FakeTransport` (no Tk event
  loop): `KFDBG CLOCK DROWSY/BEDTIME/MORNING/EPOCH <n>` all round-trip as
  `ack`, and a subsequent `KFDBG STATE` reply carries the updated
  `asleep`/`drowsy`/`tucked_in` flags.

## Not verified -- needs the bench

Nothing in this task flashes a board (CLAUDE.md: Chris flashes, the agent
builds). The following need Chris at the bench, and `KFDBG RTC` is what
makes both checkable at all:

1. **`KFDBG CLOCK`/`KFDBG RTC` against real firmware.** Compiled clean, but
   never run against silicon -- confirm `clock drowsy` visibly seats the
   futon and `rtc` reports a plausible `epoch` against a phone's clock.
2. **The coin-cell-removed OSF path** -- Task 5's still-open negative case.
   `rtc` should report `present: true` (chip runs off 3V3), `osf: True`,
   and the RAM clock unset.
3. **Offline ageing across a real power gap**, observed rather than
   inferred -- `stage_elapsed_s` (via `KFDBG STATE`) before an unplug and
   after a re-plug, compared against `rtc`'s `epoch` delta over the same
   gap.

See the implementation report's bench checklist for the exact ordered
commands.

## Consequences

- the screens/clock/sleep plan's Task 5
  STATUS block, which said `KFDBG RTC` was never built, is updated to
  reflect this task -- see that file directly rather than duplicating its
  text here (CLAUDE.md's own instruction: update the plan, don't append a
  second source of truth beside it).
- `ports/esp32/README.md`'s "no way to set the date from the device" open
  question is closed: `KFDBG CLOCK EPOCH <seconds>` is exactly the "a
  `KFDBG` command taking a full epoch" option that bullet named as one of
  three ways to fix it.
- The mutating command count referenced throughout `kf_dbg_bridge.cpp`/
  `kf_dbg_bridge.h`/`CMakeLists.txt`'s comments moves from eleven to
  twelve; every occurrence was updated in the same commit rather than left
  to drift, per CLAUDE.md's own rule about sweeping every place a changed
  count was described.
