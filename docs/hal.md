# The HakoniwaOS HAL

The HAL (hardware abstraction layer) is the boundary between code that is the
same everywhere and code that is different per machine. Everything above it is
written once. Everything below it is written per target.

**There is no emulator.** The simulator is this firmware compiled against a
desktop implementation of these headers. Same sprite engine, same frame loop,
same simulation code, different bottom layer. Two codebases that mimic each
other is a failure state, not a design.

## The rule for what goes in

Only what **must** differ between targets, at the narrowest width that still
permits a good device implementation.

The failure mode is not "too much" or "too little" in the HAL. It is putting
something at the wrong *width*: an interface that works fine on desktop but
forces the device implementation to be slow or impossible. Every "widen this
later" is a place where that would happen. See ADR 0004.

## Status

| Header | Implemented | SDL | Headless | ESP32 |
|---|---|---|---|---|
| `kf/hal/display.h` | yes | yes | yes | yes (`ports/esp32/hal/esp_display.cpp`) |
| `kf/hal/input.h` | yes | yes | yes | yes (`esp_input.cpp`) |
| `kf/hal/time.h` | yes | yes | yes | yes (`esp_time.cpp`) |
| `kf/hal/log.h` | yes | shared host | shared host | yes (`esp_log.cpp`) |
| `kf/hal/entropy.h` | yes | shared host | shared host | yes (`esp_entropy.cpp`) |
| `kf/hal/memory.h` | yes | shared host | shared host | yes (`esp_memory.cpp`) |
| `kf/hal/storage.h` | yes | shared host | shared host | yes (`esp_storage.cpp`) |
| `kf/hal/power.h` | yes | shared host | shared host | yes (`esp_power.cpp`) |
| `kf/hal/assets.h` | yes | yes | yes | yes (`esp_assets.cpp`, `esp_partition_mmap()`) |
| `kf/hal/audio.h` | **not written** | | | |

`kf/hal/storage.h` and `kf/hal/power.h` are save-state and deep-sleep-until
only -- see ADR 0012. The rest of power (light sleep, wake-on-button config,
battery telemetry) is still not written. `kf/hal/audio.h` does not exist yet
either; there is no buzzer or I2S speaker HAL in this repo.

Headers that do not exist are not stubbed. A stub that returns success is a
lie that will be discovered at the worst possible moment.

## Rules a backend must follow

1. **Report raw state, not smoothed state.** Input reports button levels and
   a timestamp. Debounce, repeat, edge detection and chords are core's job, so
   the device and the simulator feel identical. A backend that helpfully
   smoothed input would make the simulator feel better than the hardware,
   which is the exact failure this architecture exists to prevent.
2. **Never allocate on core's behalf.** Core owns the framebuffer and passes
   a pointer to it. The HAL provides pool blocks once at startup and nothing
   else. See ADR 0008.
3. **Never convert formats visibly.** If a panel wants byte-swapped RGB565,
   the backend swaps. Core holds native-endian `uint16_t` and never knows.
4. **Report honest capabilities.** `supports_partial_update` must be false if
   the backend ignores the dirty rectangle, even if that is harmless.
5. **Report the device's link speed, not the host's.** See ADR 0009. Zero
   would silently switch off the transfer-cost estimate.
6. **Keep development affordances private.** The ability to fast-forward the
   clock lives in `simulator/src/host/host_time.h`, which core cannot include.
   Game code that could fast-forward time would make the pet's ageing
   meaningless.
7. **Headers stay valid C.** Enforced by the `hakoniwaos_hal_c_check` build
   target. See ADR 0001.

## Two clocks, not one

The single most likely way to break the pet is conflating them.

| | `kf_time_mono_us()` | `kf_time_wall()` |
|---|---|---|
| Source | `esp_timer` / OS steady clock | RTC chip with coin-cell backup |
| Monotonic | always | no |
| Survives power off | no | yes, and that is the product |
| Use for | frame timing, animation, timeouts | the pet's offline ageing |

The wall clock is adversarial. It can be unset on first boot (hence the
`valid` flag), jump an hour when a timezone is corrected, jump years when a
user sets it, and go backwards when the coin cell dies. Every consumer must
handle all three.

Use the wall clock for frame timing and the pet stutters when the clock
corrects. Use the monotonic clock for ageing and the pet never ages while
switched off, which is the product.

## Adding a backend

1. Create a directory under `simulator/src/` or `ports/<target>/hal/`.
2. Implement every function in every `kf/hal/*.h`. A missing one is a link
   error, which is the intended failure.
3. Add a CMake target linking `hakoniwaos` plus those sources.

That is the whole mechanism. `kamiframe-headless` exists partly to prove it
works before the ESP32 backend depends on it: it links the same `hakoniwaos`
library as the SDL simulator, with a different bottom layer, and produces
byte-identical frames.

## Built, and what's still ahead of them

**Storage** splits in two, because the needs are different. `kf_store_*` --
save state: small, frequent, atomic, power-loss safe -- is written; see ADR
0012. `kf_assets_*` -- read-only bulk data: open, seek, read, plus a
`try_map()` that returns a direct pointer where the platform can memory-map
flash -- **is also written now**, on every backend including ESP32
(`ports/esp32/hal/esp_assets.cpp`). That escape hatch matters: mapping flash
into the address space means reading sprites with no RAM copy, and an API
without it would leave the device RAM-starved. `esp_partition_mmap()` is
confirmed on real silicon -- the board booted, mapped the partition, and
rendered from it, first against a 1,156-byte pack and later against the full
556 KB creature pack.

**Power**: `deep_sleep_until()` is written, and its desktop implementation is
the time machine described in ADR 0004 and proven in ADR 0012 -- three days
of offline pet ageing costs one function call in a test, not three days of
waiting. The rest of what people forget until month eight is not written
yet: light sleep between frames (where the battery life actually comes
from), wake-on-button GPIO configuration, battery voltage, and charging
state. None of them were what the save-then-offline-age proof needed.

## Not yet written, and the shape it should take

**Audio** should be a pull callback ("fill N frames, 16-bit mono, rate R"),
with mixing and synthesis in core. The piezo buzzer is a separate capability,
`tone(hz, ms)`, because some hardware will have one and some will not.

## Deliberately out of scope

WiFi, BLE, ESP-NOW and IR. When they arrive they should be an optional
capability module rather than baseline HAL: a minimal Kamiframe build must not
require a radio.
