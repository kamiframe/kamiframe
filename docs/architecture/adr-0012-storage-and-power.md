# ADR 0012: Save state and deep sleep

**Status:** Accepted, 2026-08-04
**Reversal cost:** Medium. `kf_store_*` is a small key-value contract that a
future save-format design sits on top of, not inside; `kf_power_*` is one
function. Neither has anything else in core depending on its shape yet.

## Requirement

`docs/hal.md` named both as **not written** from slice one, with the shape
they should take sketched in but no code behind them. The planning notes are
more specific about why they matter together rather than separately: *"The
save/load and sleep adapters... are what let you test the pet ageing while
switched off, which is on your hardware-purchase trigger list."* A virtual
pet that only ages while its owner is looking at it is not a virtual pet.
Proving that offline ageing actually works -- and proving it without waiting
three real days for every test run -- is the thing this slice exists to
unblock.

Also carried into this slice, as a standing instruction rather than a
one-off request: keep barriers to entry low for a casual person picking this
up, with the flexibility to add more capability later without a rewrite.
Concretely, that shaped two choices below: the desktop save path needs zero
configuration, and the API surface is deliberately the minimum this
requirement needs, not everything `docs/hal.md` eventually lists.

## Decision: a key-value store, not a filesystem, and one sleep call

**`kf/hal/storage.h`** is `kf_store_write(key, data, bytes)`,
`kf_store_read(key, out, max_bytes, out_bytes)`, `kf_store_erase(key)`. Not a
filesystem, not a single monolithic save blob: a small key-value store,
because that is what the device backend (ESP-IDF's NVS) actually is, and an
API shaped like a filesystem would be an API the device backend has to fake.

Two limits come from `kf/budget.h`, and they are NVS's real limits, not a
design choice made here: `KF_STORE_MAX_KEY_LEN` (15 bytes, NVS's fixed
ceiling) and `KF_STORE_MAX_VALUE_BYTES` (4000 bytes, an ASSUMPTION flagged
the same way `KF_DISPLAY_SPI_HZ` is -- a single NVS page in the common case,
to be corrected at hardware bring-up same as the SPI clock). A desktop
backend that accepted a longer key or a bigger value would be exactly the
kind of lie `budget.h`'s own header comment warns about: a save format that
works on a laptop and fails the first time it runs on the device.

The desktop and headless backends share one implementation
(`simulator/src/host/host_storage.cpp`, linked into `kamiframe_host_common`
alongside time, log, entropy and memory -- both run on a host OS, so neither
needs its own). One file per key, written atomically via the standard
write-temp-then-`rename()` trick: every desktop OS this project targets
either fully replaces the destination file or does not touch it at all, so a
reader can never observe a half-written value. The temp file is `fsync`'d
(`_commit()` on Windows) before the rename, which is real durability against
an ordinary crash, not just a torn-write guard. What it does not claim is
surviving the host machine's own power being cut mid-rename -- that would
need fsyncing the containing directory too, which has no simple portable
equivalent on Windows and is a guarantee about the desktop's disk, not the
pet's. NVS gives the real guarantee on the device.

**Zero configuration on desktop.** `kamiframe-sim` creates `kf_save/` next
to wherever it is run from and starts using it, no flag, no setup, nothing
to explain to a casual contributor. This is the barriers-to-entry
instruction made concrete: someone picking up this project for the first
time should not need to learn what NVS is, or pass a path, before a save
works. `kamiframe-headless`'s tests override this via a simulator-private
`kf_host_storage_set_dir()` (matching how `host_time.h`'s wall-clock pinning
already works) pointed at a fresh, PID-named temporary directory per run, so
tests are hermetic: never reading a leftover save, never leaving one behind,
safe to run concurrently, which was verified, not assumed (see Verified).

**`kf/hal/power.h`** is `kf_power_deep_sleep_until(wall_time)`, one call.
The desktop and headless backends implement it exactly the way ADR 0004
described before any code existed: advance the simulated wall clock
(`host_time.h`'s existing `kf_host_time_advance_wall()`, unchanged, no new
mechanism needed) instantly and return. This is what turns three real days
of offline ageing into one function call in a test.

The header comment carries a caveat worth restating here because it is easy
to miss reading the code alone: **on the device, this call may not return.**
Real deep sleep on this chip powers RAM down and resets the chip; execution
resumes at boot, not at the line after the call. Anything the caller needs
afterward must already be on the other side of that reset, saved via
`kf_store_*` beforehand. The desktop and headless backends return normally
because there is no separate boot stage to simulate, and simulating one
would be worse than not simulating it: it would make desktop code that
happens to run something after `deep_sleep_until()` look correct right up
until it is tested on hardware.

## Verified

- Full clean rebuild, GCC 13, same strict warning set as prior slices
  (`-Wall -Wextra -Wshadow -Wconversion -Wsign-conversion -Wcast-qual
  -Wdouble-promotion -Werror`), clean. `hakoniwaos_hal_c_check` confirms both
  new headers are valid C.
- `tools/check_no_heap.py`: clean. Both new headers are HAL surface, not
  core; `host_storage.cpp`/`host_power.cpp` live in `simulator/`, which is
  free to use `std::filesystem` and normal allocation the same way
  `host_time.cpp` already uses `<chrono>` and `<thread>`.
- A new `storage_power_check` `ctest` target, run via `kamiframe-headless
  --verify-storage-power`, bypassing the frame loop entirely (this is a
  storage/power check, not a rendering one). It exercises, against the real
  backend, not a mock: a fresh key reading as unavailable; an over-length
  key, an invalid-character key, and an oversized value all rejected;
  write-then-read-back byte-identical; the wall clock advancing by exactly
  three simulated days from one `kf_power_deep_sleep_until()` call; the
  saved value surviving that simulated sleep; sleeping until a time already
  in the past being a harmless no-op rather than an error; and erase making
  a key read as unavailable again. All 4 `ctest` targets, this one included,
  pass consistently across 10 repeated full-suite runs under sustained
  artificial CPU load (the exact condition that exposed ADR 0011's
  debounce-timing bug), and `--verify-storage-power` alone passes across 15
  repeated runs under the same load plus 8 concurrent invocations at once,
  confirming the per-PID temp directory scheme does not collide with itself.
- The zero-configuration desktop path verified for real: running
  `kamiframe-sim` with no prior setup creates `kf_save/` next to the binary
  and logs it, with no flag, no environment variable, no prompt.

## Accepted cost

`kf_assets_*` (bulk read-only data -- sprites, sound, level data) is not
built. `docs/hal.md` already named this as a separate concern with an
opposite shape (large, static, wants memory-mapping) from save state (small,
changing, wants atomicity), and this slice's actual requirement -- prove
save-then-offline-age -- never touched it. Designing it now would mean
guessing at a shape with nothing real to design against yet.

Power is one call. Light sleep between frames -- the mechanism actual
battery life depends on -- wake-on-button GPIO configuration, battery
voltage, and charging state are all real and eventually necessary, and none
of them are in this slice, because none of them were what the
hardware-purchase trigger asked for. `kf_power_wake_reason()` (was it a
timer, a button, or first boot?) is named in `kf/hal/power.h`'s own comment
as future work rather than built, for the same reason.

The desktop storage backend's atomicity is real (fsync before rename) but
stops short of surviving the host's own power loss mid-rename, which would
need directory-fsync semantics with no simple Windows equivalent. This is a
guarantee about a development machine, not about the pet; NVS provides the
guarantee that matters on the device.

## Later

- `kf_assets_*`, once there is a real asset pack to design it against.
- The rest of power: light sleep, wake-on-button configuration, battery
  telemetry, `kf_power_wake_reason()`.
- A real save FORMAT and versioning belongs above this HAL, in core, once
  there is a pet with state worth versioning. This slice deliberately built
  and proved the mechanism underneath that, not the format itself --
  `kf/demo.h` is still explicitly placeholder content with nothing built on
  top of it, unchanged by this slice on purpose.
