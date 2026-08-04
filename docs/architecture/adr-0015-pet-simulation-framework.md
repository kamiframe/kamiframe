# ADR 0015: The pet simulation framework

**Status:** Accepted, 2026-08-04
**Reversal cost:** Medium. `kf/pet.h`'s four verbs (`init`, `advance`,
`feed`/`play`/`rest`, `save`/`load_and_advance`) are a small, pure-logic
surface with one HAL-touching pair sitting on top of it. Nothing else in
core depends on its shape yet -- the real Lua `pet.*` API this is meant to
be designed against does not exist yet either. The save format itself is
versioned (see below) precisely so it can change without that being a
reversal.

## Requirement

`02-min-spec-sheet.md` names this directly: *"This is your secret weapon
and the thing no generic console has."* A dev builds a pet by configuring
and skinning needs/decay, not by writing a simulation from scratch --
which is why `kf_pet_config` is a value a caller supplies, not a constant
baked into this file. It is item 4 in the spec's own build order, coming
after storage/power (ADR 0012) and Lua embedding (ADR 0014), and for a
concrete reason: ADR 0012's hardware-purchase trigger is *"the save/load
and sleep adapters... are what let you test the pet ageing while switched
off"* -- that trigger names offline ageing, but nothing before this slice
actually had a pet's needs to age. This slice is what makes that trigger
real rather than aspirational.

`08-phase1-slice1-decisions.md`'s HAL boundary table settles where the line
sits before any code was written: *"Pet simulation, needs, decay, evolution
| Core | Pure logic. Should be unit-testable with no HAL at all."* That
line, not a preference discovered while coding, is why `kf_pet_advance()`,
`kf_pet_feed()`, `kf_pet_play()` and `kf_pet_rest()` take no HAL types and
make no HAL calls -- only `kf_pet_save()` and `kf_pet_load_and_advance()`
do, and they are thin wrappers around the pure functions, not where the
maths lives.

## Decision: millipercent needs, config not constants, one closed-form step

**Needs are `kf_pet_millipercent`** -- `uint32_t`, 0..100000 representing
0.000%..100.000% -- not `float`. The reason is specific to what this slice
actually does with elapsed time: `kf_pet_advance()` computes decay as one
multiply-and-divide over an arbitrary `elapsed_seconds`, which for offline
fast-forward is routinely three real days in a single call, not a value
accumulated frame by frame. A `float` percentage drifting by float epsilon
once a frame would be invisible in any test run short enough to notice; the
same drift compounding across months of real device uptime, the actual
target hardware's actual lifespan, is not the kind of bug a demo catches.
Integer millipercent is exact by construction: there is no accumulation
step to drift.

**`kf_pet_config` is a parameter, not `#define`d constants.** The spec's own
framing -- *"configure and skin"* -- means the decay rates are exactly the
thing a dev building a real pet needs to change without touching this
file's logic. `kf_pet_default_config()` supplies illustrative values
(hunger fastest, ~4 days full-to-empty; energy slowest, ~8 days; happiness
between) used by the demo and the determinism check, not a tuning
recommendation -- there is no real pet yet to tune against, only the
mechanism it will eventually run on.

**Decay is one closed-form step**, `delta = rate_mp_per_hour *
elapsed_seconds / 3600`, computed in `uint64_t` before clamping back to
`kf_pet_millipercent`. `rate_mp_per_hour` (a few thousand, at most) times
`elapsed_seconds` (which for offline fast-forward can genuinely be weeks,
so up to roughly 10^7) does not fit `uint32_t` with headroom to spare; it
cannot overflow `uint64_t` for any elapsed time this project will ever
see. This is what makes "the device was off for 3 days" a single multiply
inside `kf_pet_load_and_advance()` rather than three days of simulated
loop iterations -- the same reasoning ADR 0012's
`kf_power_deep_sleep_until()` already applies one layer down, for the wall
clock itself.

**The on-disk save is hand-packed, byte by byte, not a raw struct write.**
`kf/pet.cpp`'s file header explains why in more detail than repeated here:
this project builds with both GCC and MSVC (see the CI matrix), and a
C++ struct's padding and member layout are not something two compilers are
obliged to agree on. A save that only round-trips on the compiler that
wrote it is exactly the kind of lie `kf/budget.h`'s own header comment
warns about, in a different place. `pack()`/`unpack()` write and read each
field explicitly at a fixed byte offset (`KF_PET_SAVE_BYTES = 22`: 1 version
byte + 3×4 need fields + 1 valid flag + 8 epoch bytes), and the format is
versioned -- `unpack()` rejects anything not written by the current
version rather than guessing at a layout that changed. A rejected load
falls back to `kf_pet_init()`'s fresh-pet state, which is always
well-defined, rather than partially populating `state` from a corrupt or
foreign-version buffer and running with whatever came out.

**Offline fast-forward is one function, `kf_pet_load_and_advance()`**,
built directly on ADR 0012's storage and power HAL rather than anything
new: read the save (or start fresh if there is none), compute
`elapsed = kf_time_wall().epoch_seconds - state.last_advanced.epoch_seconds`,
call `kf_pet_advance()` once with that value, then adopt the new wall-clock
reading as the baseline for next time. The three adversarial cases
`kf/hal/time.h` names explicitly for this exact clock are all handled,
matching precedent already set elsewhere in this codebase rather than
invented fresh:

- **Wall clock not yet valid** (RTC unset on first boot): skip ageing this
  call, leave `last_advanced` invalid so the next call with a real clock
  reading starts fresh rather than fast-forwarding from an epoch of zero.
- **Wall clock reads earlier than the saved `last_advanced`** (RTC reset,
  or a user sets the date back): clamp elapsed to zero rather than
  underflowing a `uint32_t` subtraction. The pet does not get younger; it
  just does not age this call. This mirrors
  `kf_power_deep_sleep_until()`'s own "already in the past is a no-op, not
  an error" handling for the identical clock, in the opposite direction.
- **Elapsed time absurdly large** (a defensive cap, not a realistic one:
  `0xFFFFFFFF` seconds is about 136 years): clamped before the cast to
  `uint32_t`, so the cast itself is never undefined behaviour.

## What this slice actually builds

Needs (hunger, happiness, energy), millipercent decay over an arbitrary
elapsed time, three care actions that raise a need and clamp at 100%
without banking overfeeding against future decay, a versioned save format,
and the offline-fast-forward mechanism proven end to end: save, simulate
sleep, reload, and get the same needs a direct `kf_pet_advance()` call
would have produced.

## What deliberately is not built

Life stages and evolution graphs, personality traits, care-mistake
tracking, and the random event scheduler -- all named in the spec, none of
them needed to prove the hardware-purchase trigger, and every one of them
a real design surface that deserves its own slice once there is a Lua
`pet.*` API to design them against rather than guessing at their shape now.
This is the same scoping discipline ADR 0013 applied to LVGL ("just a proof
screen") and ADR 0014 applied to Lua ("just the embedding mechanism, no
cartridge format") -- needs, decay and offline fast-forward alone, the
piece everything else in the spec's pet feature actually sits on top of.

No Lua binding yet. `kf.pet.*` (or whatever the eventual name is) is
explicitly future work; this slice is Core-only, unit-testable with no HAL
and no VM in the picture, on purpose -- see the Requirement section's HAL
boundary quote.

No UI. Nothing here draws a pet, a stat bar, or a menu. `kf_pet_state`'s
three millipercent fields are exactly the values a future screen would
read; this slice stops at producing correct values, not presenting them.

## Verified

- Full clean rebuild, GCC 13, the same strict warning set as every prior
  slice (`-Wall -Wextra -Wshadow -Wconversion -Wsign-conversion -Wcast-qual
  -Wdouble-promotion -Werror`), clean. Two real warnings-as-errors were
  caught and fixed by this build, not found by inspection: a `"/*" within
  comment` from a `kf/hal/*.h` literal substring in `kf/pet.h` (the same
  class of bug ADR 0014 hit in `kf_lua_port.h`, fixed the same way -- reword
  around the literal), and a sign-conversion warning in `get_i64()`'s loop
  index feeding a `size_t`-indexed array access, fixed by making the index
  `unsigned`.
- `tools/check_no_heap.py`: clean. `kf/pet.h` and `pet.cpp` are Core, no
  `kf-allow-heap` marker used or needed -- the whole implementation is
  fixed-size buffers and integer maths.
- A new `pet_offline_ageing_check` `ctest` target, run via
  `kamiframe-headless --verify-pet`, using the same isolated per-PID
  storage directory trick as `storage_power_check`. It exercises, against
  the real storage and power HAL, not a mock: a fresh pet starting at full
  needs; hunger, happiness and energy each decaying by exactly their
  configured `mp_per_hour` rate over one simulated hour; needs clamping at
  zero rather than underflowing over a full simulated year; care actions
  clamping at max rather than banking overfeeding; and -- the strongest
  proof, and the actual hardware-purchase trigger -- saving state, fast-
  forwarding the wall clock 3 simulated days via
  `kf_power_deep_sleep_until()`, reloading via
  `kf_pet_load_and_advance()`, and confirming the result is identical to
  calling `kf_pet_advance()` directly on a snapshot of the pre-sleep state
  with the same elapsed seconds. A fifth case proves a backwards clock jump
  (`kf_time_set_wall()` to simulate an RTC reset) does not age the pet
  negatively.
- All 7 `ctest` targets, this one included, pass consistently across 5
  repeated full-suite runs. `--verify-pet` alone passes across 10 repeated
  runs and 3 concurrent invocations at once, confirming the per-PID
  temporary-directory scheme does not collide with itself -- the same
  property ADR 0012 verified for `storage_power_check`.

## Accepted cost

The illustrative decay rates in `kf_pet_default_config()` are not tuned
against anything real, the same caveat ADR 0012's storage limits and ADR
0014's allocator sizing both carry: there is no real pet yet to tune
against. Changing them is a config change, not a code change, which is the
point of `kf_pet_config` being a parameter rather than a constant.

`KF_PET_SAVE_BYTES` (22) is fixed at this slice's exact field set. Adding a
field later means bumping the save version and either migrating old saves
or accepting that they reset -- a real decision, deliberately deferred
rather than over-engineered into a self-describing format this slice does
not need yet.

## Later

- The real Lua `pet.*` (and `save.*`, `draw.*`, `time.*`) API surface --
  more buildable now that there is a real pet state to bind to, rather than
  guessed at when only the VM mechanism existed (ADR 0014's own "Later").
- Life stages, evolution, personality traits, care-mistake tracking, the
  random event scheduler -- all named in the spec, all deferred, per "What
  deliberately is not built" above.
- A UI: a stat display, a menu screen reading these values, presumably
  built on the LVGL port glue ADR 0013 already proved out.
- Save-format migration, once there is a real field worth adding to a save
  someone already has on a device.
