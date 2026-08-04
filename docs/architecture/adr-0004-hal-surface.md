# ADR 0004: What belongs in the HAL

**Status:** Accepted, 2026-08-04
**Reversal cost:** Very high. This is the decision the rest depend on.

## The rule

The HAL contains only what **must** differ between desktop and device,
expressed at the narrowest width that still permits a good device
implementation.

The failure mode is not "too much" or "too little" in the HAL. It is putting
something at the wrong *width*: an interface that works fine on desktop but
forces the device implementation to be slow or impossible.

## In the HAL

| Module | Why it must be here | Built? |
|---|---|---|
| Display | SDL texture versus ST7789 over SPI+DMA | Yes |
| Input | SDL keyboard versus GPIO reads | Yes |
| Time | Two clocks, see below | Yes |
| Log / panic | stderr and abort versus UART and a message painted on the panel | Yes |
| Entropy | OS CSPRNG versus `esp_random()` | Yes |
| Memory pools | One flat heap versus internal SRAM and PSRAM | Yes |
| Storage: save state | A file versus NVS. Must be atomic and power-loss safe | Header later |
| Storage: assets | A file versus a flash partition, possibly memory-mapped | Header later |
| Power / sleep | Real sleep versus simulated sleep | Header later |
| Audio | SDL callback versus an I2S DMA ring | Not yet |

## Not in the HAL

Sprite engine, blitting, clipping, font rendering, the Lua runtime, the pet
simulation, save *format* and versioning, asset pack format, frame pacing
policy, input debounce and repeat, audio mixing, the game-visible RNG, and
the UI toolkit. All core, so they are bit-identical on both targets.

The UI toolkit sitting above the HAL is what keeps the LVGL evaluation open.

WiFi, BLE, ESP-NOW and IR are deliberately out of scope. When they arrive they
should be an *optional capability module*, not baseline HAL: a minimal build
must not require a radio.

Simulator conveniences (file paths, CLI arguments, screenshots, the ability to
fast-forward the clock) are backend-private and must never be reachable from
core. `simulator/src/host/host_time.h` is an example: core cannot see it, by
design, because game code that could fast-forward the clock would make the
pet's ageing untrustworthy.

## The three details that would have hurt later

**1. Two clocks, not one.** `kf_time_mono_us()` is monotonic and means nothing
outside this power cycle. `kf_time_wall()` is RTC epoch seconds, survives
power off, and is adversarial: unset on first boot, jumps when a timezone is
corrected, goes backwards when the coin cell dies. Offline ageing depends
entirely on the second. Conflate them and a user correcting their clock ages
the pet forty years, or gets a negative elapsed time. The wall clock carries a
`valid` flag because on first boot there is no correct answer.

**2. `present()` takes a dirty rectangle from day one.** A full 240x320 RGB565
frame is 153,600 bytes, roughly 30ms of SPI wire time at 40MHz. That is most
of a 33ms frame before anything is drawn. Partial updates are how the headroom
comes back, and they only work if the interface permits them. Adding the
parameter now is free; adding it later means auditing every call site and
every drawing routine for what it actually touched. Desktop ignores it. The
device will not.

**3. Sleep is in the HAL because it is a time machine.** Once
`deep_sleep_until(wall_time)` is a HAL call, the desktop backend can implement
it as "advance the simulated clock instantly and return." Three days of
offline pet ageing then take a microsecond, in CI, deterministically. If sleep
were not in the HAL, testing offline fast-forward would mean waiting three
days or writing test-only time plumbing that does not match device behaviour.
The hardware-purchase trigger is literally "save and offline fast-forward
working," so this is the mechanism that proves it.

## Capability flags

`kf_display_get_caps()` returns width, height, pixel format, whether partial
update is honoured, and whether a backlight exists. Core reads it rather than
hardcoding, so a future e-ink or mono variant is a backend rather than a fork.
`kf/budget.h` is the compile-time size core allocates; caps is the panel
actually attached. Today `app.cpp` asserts they agree.
