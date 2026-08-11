# ADR 0026: A real DS3231 RTC driver, closing the wall-clock gap

**Status:** Accepted
**Date:** 2026-08-06

## Context

Every ADR since 0020 has named the same open gap without closing it:
`kf_time_wall()` on ESP32 has never had a real RTC behind it. `esp_time.cpp`
seeds the chip's own system clock invalid on every cold boot, which means
the one thing `kf/hal/time.h` calls non-negotiable -- the pet ages while the
device is switched off -- has never actually worked on real hardware. ADR
0025 wired the pet session itself up for real and said as much in its own
"What this slice does NOT reach" section: `kf_pet_session_init()`'s offline
fast-forward runs, but with nothing valid to fast-forward across.

This slice closes that gap. It is software-only, same as ADR 0025 --
`ports/esp32-bringup/main/bringup_main.cpp`'s `stage_i2c_and_rtc()` function
is the only code in this repo that has actually talked to a DS3231, and it
is a diagnostic, not a driver: it scans the whole I2C bus, prints
human-readable pass/fail lines, and deliberately seeds a fake time on OSF
because a human is standing there watching it happen at bring-up. None of
that is appropriate for something that runs unattended on every boot. This
driver is adapted from it, not copied from it: same pins, same register
map, same bus config, checked directly against that file's source (not
memory of it, given the field-naming bug ADR 0025 already ran into once
this session) -- but with production error handling instead of a printed
diagnosis, and no auto-seed.

Real hardware arrives 2026-08-07 -- tomorrow, as of this writing, still not
today -- so nothing here has run against a physical chip yet. See "Not yet
done" below for exactly what that leaves unverified.

## Decision

### 1. The driver lives in `esp_time.cpp` itself, not a new file or component

`kf_time_init()` and `kf_time_set_wall()` are the only two functions that
touch the DS3231, and both already live in this file -- it already owns the
time HAL contract, and I2C access here is single-purpose enough (one chip,
two functions) that it does not earn a separate translation unit the way
`esp_display.cpp`'s SPI+DMA/`esp_lcd` machinery does. `ports/esp32/main/
CMakeLists.txt` gets one new line: `esp_driver_i2c` added to `REQUIRES`,
the exact component name already confirmed working in
`ports/esp32-bringup/main/CMakeLists.txt`'s own `REQUIRES` list for this
exact chip on this exact board.

### 2. Epoch/BCD conversion is hand-rolled, not `mktime()`/`timegm()`

ESP-IDF's default newlib-nano does not reliably carry `timegm()`, and this
needs no libc calendar support at all -- Howard Hinnant's well-known
`days_from_civil()`/`civil_from_days()` algorithm is pure integer math, no
timezone state, no libc dependency to verify against a toolchain this
environment does not have. It host-compiles and behaves identically to
however it runs on-device, which is exactly the property this conversion
needs given it is the one part of this driver with no hardware dependency
at all -- see "Verified" below for how thoroughly that let it be checked
ahead of any real build.

### 3. Every failure path leaves the wall clock exactly as unset as today

Bus init fails, no device answers at `0x68`, the device answers but its
temperature register reports an implausible room temperature (see #4
below), or the OSF flag says the oscillator has stopped since it was last
set -- every one of these just returns, leaving `kf_time_wall()`'s
`.valid` false, identical to a device with no RTC wired at all. Nothing
here can crash or hang the boot; a missing or dead RTC degrades to exactly
today's behaviour, not a new failure mode.

**Deliberately not auto-seeding on OSF.** The bring-up diagnostic seeds a
placeholder time and clears OSF because a human is watching it run and can
verify the result. Doing that quietly on every boot in production would
mean a board with a dead coin cell silently reports a plausible-looking but
wrong time forever, which is worse than reporting none. This driver leaves
OSF-recovery to `kf_time_set_wall()` being called by something that
actually knows the correct time -- not yet written, see "What this slice
does NOT reach."

### 4. A temperature sanity check guards the address collision

`kf_esp_pins.h`'s own comment already names the risk: the DS3231 and an
MPU-6050 both answer at `0x68`, and cannot share a bus without the MPU's
AD0 pin pulled high. An ACK at that address is not proof it is the RTC.
The bring-up diagnostic tells them apart with the temperature register
(`0x11`) and a plausible-room-temperature check; this driver does the same
check for the same reason, just as a silent bail-out instead of a printed
diagnosis. This is defense in depth on top of ADR 0024's bring-up pass,
not a replacement for it -- bring-up runs once, by a human, at assembly
time; this check runs on every boot, unattended, and is cheap enough (one
extra 2-byte read) that there is no reason to skip it. Without it, a
mis-wired board (DS3231 not populated, MPU-6050 answering instead) would
not fail to find an RTC -- it would silently feed `kf_pet_session` a wrong
epoch built from whatever the MPU-6050's registers happen to contain,
which is a much worse failure than "no wall clock yet."

### 5. `kf_time_set_wall()` writes through to the chip, not just RAM

`kf/hal/time.h`'s own doc comment says this function is "used by whatever
configures the RTC: a settings screen, an NTP sync, a companion app" --
the contract has always implied hardware persistence, even though nothing
called it in production until now. Before this slice, `kf_time_set_wall()`
only called `settimeofday()`; a correction made through it would not have
survived a genuine power-off, silently defeating the entire point of
having a battery-backed chip. Now, when a DS3231 answered at boot, a
`kf_time_set_wall()` call also writes the same epoch back to the chip's
time registers and clears OSF on success. This is a best-effort write: a
failure here does not undo the RAM clock update (still correct for the
rest of this power cycle), it just means the correction will not survive
the next power-off -- the same class of failure this file's header comment
already documents for a dying coin cell, not a new one.

Grep confirms nothing in this codebase calls `kf_time_set_wall()` outside
tests yet -- there is no settings screen or NTP sync to call it in
production today. The HAL contract is implemented correctly regardless of
whether a caller exists, the same discipline ADR 0020 used for
`esp_power.cpp`'s real deep sleep before anything called that either.

## What this slice does NOT reach

> **Both of the first two bullets below were retired on 2026-08-11.** The
> driver has now run against a physical DS3231 and survived a power cut, and
> the Lua Settings screen is a real production caller of
> `kf_time_set_wall()`. See "Confirmed on hardware, 2026-08-11" below. They
> are left in place, struck through, because the rest of this section is
> still accurate and rewriting history here would hide what this slice
> actually did and did not reach when it was written.

- ~~**Not run on real hardware.**~~ Retired 2026-08-11 -- see below.
- ~~**No caller for `kf_time_set_wall()` in production.**~~ Retired
  2026-08-11: the Lua Settings screen calls it via
  `kf_lua_port_apply_clock()`.
- **No timezone or DST handling.** DS3231 registers and `epoch_seconds`
  are both treated as one unlabelled number, always. This slice called that
  number "raw UTC"; `kf/clock.h` later settled it explicitly as **local
  time**, with no timezone and no UTC offset anywhere in the system (ADR
  0046/0047). Nothing changed in behaviour -- the number was always
  unlabelled -- but the naming here predates that decision, so read
  `kf/clock.h`, not this bullet, for what the epoch means.
- **No day-of-week tracking.** Register `0x03` is written with a fixed,
  valid-but-meaningless placeholder, matching the bring-up seed's own
  approach -- nothing in this codebase reads it back.

## Verified

- The pure-math half of this driver -- `bcd_to_bin()`/`bin_to_bcd()`,
  `days_from_civil()`/`civil_from_days()`, and the two functions built on
  them (`ds3231_regs_to_epoch()`/`epoch_to_ds3231_regs()`) -- was extracted
  into a standalone host program and checked two ways: cross-validated
  against libc's own `timegm()` for a spread of hand-picked cases (the
  exact bring-up seed date, both leap-year edges, the 2000/2099 boundaries
  of the DS3231's representable range, and the values nearest the 32-bit
  epoch rollover), and swept across every single day from 2020 through
  2035 (5,844 dates) checking three things at once: the epoch matches
  `timegm()` exactly, consecutive days differ by exactly 86400 seconds, and
  `epoch -> regs -> epoch` and `regs -> epoch -> regs` both round-trip
  byte-for-byte. All passed.
- `esp_time.cpp` was checked against a hand-written stub of the exact
  ESP-IDF `driver/i2c_master.h` surface it calls (`i2c_master_bus_config_t`,
  `i2c_device_config_t`, `i2c_new_master_bus()`,
  `i2c_master_bus_add_device()`, `i2c_master_transmit_receive()`,
  `i2c_master_transmit()`), matched field-for-field against
  `stage_i2c_and_rtc()`'s real, working usage of the same API -- plus the
  **real, current** `kf_esp_pins.h` pulled fresh from the repo for this
  check (not a stale local copy -- an earlier stale copy in this
  environment was missing the I2C pin section entirely, caught by exactly
  this cross-check before it could hide a real error). Compiles clean
  under `-Wall -Wextra -Werror` against that stub. This is a real
  compile-time check of every call site's argument types and struct field
  names -- the same category of bug ADR 0025 hit for real
  (`hunger`/`happiness`/`energy` vs. `hunger_mp`/`happiness_mp`/
  `energy_mp`) -- but it is still a stub, not the real ESP-IDF headers, so
  it cannot catch a wrong assumption about the *real* API's behavior, only
  a wrong assumption about its *shape*.
- **`idf.py build` now genuinely succeeded, on Chris's own machine,
  2026-08-06.** Same toolchain ADR 0025 confirmed (ESP-IDF v6.0.2,
  xtensa-esp-elf 15.2.0, Ubuntu under WSL), same command, clean build with
  no fixes needed this time: `esp_driver_i2c` shows up in the real
  component list ESP-IDF prints (confirming the `REQUIRES` addition in
  decision #1 was correct), and `kamiframe-firmware.bin` grew from
  ADR 0025's `0x41a20` bytes to `0x45ff0` bytes (~280KB) against the same
  1MB app partition -- 73% still free, down one point from ADR 0025's 74%,
  which is the right ballpark for what one new driver file should cost.
  This is the stub-based check above turning into the real thing: the stub
  proved the code *should* link; this proves it *does*, against the actual
  ESP-IDF headers this environment cannot see.

## Confirmed on hardware, 2026-08-11

This is the Task 5 bench result from
`docs/superpowers/plans/2026-08-13-screens-clock-sleep.md`, which required
that what was *seen* be recorded here rather than a verdict. Observed on the
owner's board -- ESP32-S3-WROOM-1 N16R8, ILI9341 panel, DS3231 module with
its coin cell fitted, I2C on GPIO13 (SDA) / GPIO14 (SCL).

The clock was set from the Lua Settings screen, then read back from the
boot log (`DS3231: wall clock set from RTC (epoch ...)`) across three
boots, with USB power **fully removed** for roughly one minute between the
second and third:

| Reading | Epoch | Delta |
|---|---|---|
| first boot | 1786384202 | -- |
| before the power cut | 1786384432 | +230 s |
| after ~1 minute unplugged | 1786384549 | **+117 s** |

**+117 seconds across the power cut**, consistent with about a minute
unplugged plus reconnect, boot and monitor attach. Every read was
monotonic and tracked real elapsed time. OSF was clear on all three boots,
so the oscillator never stopped -- the coin cell carried the chip with the
board's own supply removed. That is the claim this driver was written to
support, and it now holds on silicon rather than on reasoning.

What this establishes, end to end: the I2C bus config and pins are right,
the temperature-based DS3231/MPU-6050 disambiguation passes on a real
DS3231, the register map reads back correctly, `kf_time_set_wall()`'s
write-through reaches the physical chip from a production caller, and the
seed survives a genuine power-off.

### Still not exercised on hardware

The success path is proven; the failure paths are not. Specifically, on a
real board nothing has yet exercised: the OSF-set branch (would need a
drained or removed coin cell), the wrong-chip temperature rejection (would
need an MPU-6050 at 0x68), a bus-init or device-add failure, or the
write-through failure path. These are all reasoned and host-testable only.

One real limitation surfaced by this run rather than by the code: the
board's **date** was a day behind while its time-of-day was correct, because
the Settings screen edits hour and minute only and
`kf_lua_port_apply_clock()` preserves whatever date the RTC already held.
Nothing in the system reads the date today -- the night window uses
hour-of-day, ageing counts elapsed seconds -- but there is no way to
correct a drifted date from the device. A `KFDBG` command taking a full
epoch, or an NTP sync, is the fix; neither exists yet.

## Cost to change

If bring-up turns up a different register layout than documented (unlikely
-- the DS3231 datasheet and the bring-up diagnostic agree, and this driver
was checked against the latter's source directly) or the temperature
disambiguation proves unreliable in practice: the blast radius is this one
file. Nothing else in the codebase depends on any DS3231-specific detail;
`kf_pet_session_init()` already calls `kf_pet_load_and_advance()`, which
already reads whatever `kf_time_wall()` reports, exactly as ADR 0025's own
"Cost to change" predicted -- the fast-forward starts working the moment
the HAL layer beneath it does, with no change needed on the pet-session
side at all.

## Superseded in part

**"No timezone or DST handling... DS3231 registers and `epoch_seconds` are
both treated as raw UTC, always"** is superseded by ADR 0046: `epoch_seconds`
is defined to be LOCAL time, directly, with no timezone or offset applied
anywhere -- not UTC. ADR 0046 predates neither `kf/types.h`'s
`kf_wall_time` contract this ADR cites nor this driver's registers; both
this driver and `kf/clock.h` read and write the same `epoch_seconds`, so
this driver's write-through (decision #5) writes local time into the
DS3231's registers, and `ds3231_regs_to_epoch()`/`epoch_to_ds3231_regs()`
should be read as converting to and from local time, not UTC. Implementing
this ADR's Task 4 (or any future write path) against the "raw UTC" reading
above would write UTC into the chip that `kf/clock.h`'s night-window
accounting then reads back as local time -- exactly the bug ADR 0046 exists
to prevent elsewhere. The pure BCD/civil-date arithmetic this ADR verifies
is unaffected either way; only the meaning attached to the epoch second
changes.
