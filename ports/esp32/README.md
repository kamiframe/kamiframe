# ports/esp32

**Real ESP32 HAL backends -- display, input, time (now DS3231-backed, see
ADR 0026), memory, entropy, logging, storage, power -- all compiling and
linking clean against the real ESP-IDF v6.0.2 toolchain, driving the real
`kf_app_init()`/`kf_app_frame()` loop from `kf/app.h`, with a real,
NVS-backed `kf_pet_session` ticking alongside it (ADR 0025). Not yet run
against real hardware -- parts arrive 2026-08-07. Still not running LVGL or
Lua -- see ADR 0025 for exactly why not, and ADR 0020 for why the pet
session itself waited this long.** See ADR 0019 for the hello-world this
was built on top of, ADR 0020 for the HAL backends, ADR 0025 for the pet
session, and ADR 0026 for the DS3231 real-time clock driver.

This directory used to be a documented skeleton, never compiled. It now
builds and boots: `idf.py build` produces a real `kamiframe-firmware.elf`/
`.bin` for the esp32s3 target, `hakoniwaos`'s entire source list compiles
through the real xtensa cross-compiler as part of that build, and
`wokwi-cli .` (Chris's own machine, his own token -- this environment
cannot generate one) ran an earlier hello-world build of that same binary
through Wokwi's `board-esp32-s3-devkitc-1` simulation and captured a clean
boot: real ROM bootloader output, into `app_main()`, printing every line
`main.c` wrote at the time. See ADR 0019's "Verified" section for the full
log and exactly what still hasn't been checked (real hardware).

## Phase 1's exit trigger is now met

Phase 1b of `04-roadmap-diy-release.md` begins once both are true:

- the demo pet runs in the simulator under enforced constraints, with save and
  offline fast-forward working (true since ADR 0017), **and**
- an ESP-IDF hello-world boots in [Wokwi](https://wokwi.com) (browser-based
  ESP32-S3 simulation) -- true since the Wokwi run described above.

Both are satisfied. Parts have been ordered and arrive 2026-08-07; hardware
bring-up (ADR 0024, on the separate `ports/esp32-bringup` project) runs
first to confirm the wiring before this firmware ever touches a real board.

**Correction to this section's own past claim:** the ST7789 parenthetical
above used to say Wokwi simulates "an ST7789, no parts required." It
doesn't -- Wokwi's supported-hardware list does not include the ST7789 at
all; the closest part it has is `wokwi-ili9341` (also 240x320 SPI). The
ADR 0019 hello-world sidesteps the question entirely by not wiring up a
display (see `diagram.json`), matching the official
`wokwi/esp-idf-hello-world` reference project's own scope. Whatever panel
this project eventually simulates on Wokwi, it will not be simulating the
exact ST7789 real hardware will use -- worth knowing before ordering parts
on the strength of a Wokwi screenshot.

## How the two builds share one source tree

The desktop build starts at the repository root. The ESP-IDF build starts
*here*, and points `EXTRA_COMPONENT_DIRS` back at the same module directories:

```
kamiframe/
  CMakeLists.txt            <- desktop entry point
  hakoniwaos/
    sources.cmake           <- the source list. Exists ONCE.
    CMakeLists.txt          <- if(ESP_PLATFORM) component else() library
  ports/esp32/
    CMakeLists.txt          <- ESP-IDF project entry point (this directory)
    main/                   <- IDF requires a component called `main`
```

`hakoniwaos/CMakeLists.txt` branches on `ESP_PLATFORM`, which ESP-IDF defines.
The same directory is a plain CMake static library on the desktop and an
ESP-IDF component on the device, reading its file list from the same
`sources.cmake` either way. That is what stops the two builds from drifting
apart, which is the failure this whole architecture is arranged to avoid.

## Constraints this layout puts on the rest of the repo

These are already true and need to stay true:

1. **Module directory names are ESP-IDF component names.** ESP-IDF takes the
   component name straight from the directory name, and it must be unique
   against every component inside ESP-IDF itself. `hakoniwaos` is safe. A
   directory named `driver`, `console`, `main` or `log` anywhere in the tree
   would collide. (This is also why the OS folder is `hakoniwaos` and never
   bare `hakoniwa`.)
2. **Components cannot nest.** Keep the module list flat.
3. **`REQUIRES` here and `target_link_libraries` in the desktop branch must be
   kept in sync by hand.** There is no mechanism for this, so keep the
   dependency graph small enough that it does not matter.

## What Phase 1b (ADR 0020) added

- Real backends for every HAL header under `ports/esp32/hal/`: `esp_display.cpp`
  (ST7789 over SPI+DMA via `esp_lcd`), `esp_input.cpp` (GPIO, active-low with
  internal pull-ups), `esp_time.cpp` (`esp_timer` for mono; at the time, the
  chip's own system clock for wall -- **not** a real RTC yet, see ADR 0020's
  "A real, named gap" -- ADR 0026 below closes that gap), `esp_memory.cpp`
  (`heap_caps_malloc` against `MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA` and
  `MALLOC_CAP_SPIRAM`), `esp_entropy.cpp` (`esp_random()`), `esp_log.cpp`
  (`esp_log_write` plus a panic-and-reboot path), `esp_storage.cpp`
  (ESP-IDF NVS), `esp_power.cpp` (real deep sleep).
- `sdkconfig.defaults` now also enables octal-mode PSRAM
  (`CONFIG_SPIRAM_MODE_OCT`) and 240MHz CPU
  (`CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240`) -- both needed the moment
  `esp_memory.cpp`'s `MALLOC_CAP_SPIRAM` pool has to actually exist. Flash
  size unchanged from ADR 0019.
- `main/app_main.cpp` replaces `main.c`: it calls the real
  `kf_app_init()`/`kf_app_frame()` loop against the backends above. As of
  ADR 0020 it did **not** run the pet session, LVGL, or Lua -- that's not a
  missing call, it's a missing component directory (`EXTRA_COMPONENT_DIRS`
  only points at `hakoniwaos`); see ADR 0020's "What this slice does NOT
  reach." ADR 0025 below changes the pet session half of that.

## What ADR 0024 added

- `ports/esp32-bringup`, a **separate** ESP-IDF project, not a mode of this
  one: a standalone diagnostic (`bringup_main.cpp`) that scans the I2C bus,
  probes the display, and checks the DS3231/microSD/buttons one at a time,
  each with a human-readable pass/fail line and a plain-English fix
  suggestion -- built to be run once, by a human, while holding a
  soldering iron, not part of this firmware's normal boot.
- The full pinout in `ports/esp32/hal/kf_esp_pins.h` reached its current
  shape here: display, buttons, I2C (DS3231, and whatever else eventually
  shares the bus), microSD, and the reserved-but-unwired I2S lines all
  documented in one file, cross-checked against `docs/hardware-bringup.md`.
- Not yet run against real hardware -- parts arrive 2026-08-07. This ADR's
  own pinout is reasoned from datasheets and the DevKitC-1's known-bad pins,
  not yet proven against a real board.

## What ADR 0025 added

- `kf_pet_session.{h,cpp}` (`simulator/src/pet/`) is now built as part of
  `main`, referenced by relative path the same way the HAL backends
  already are -- one canonical file, not a fork. `app_main.cpp` calls
  `kf_pet_session_init()` after `kf_app_init()` and
  `kf_pet_session_frame(0)` every frame, the same two calls and the same
  ordering `sdl_main.cpp` uses on desktop. The pet is genuinely alive,
  saved to and loaded from real NVS, decaying against real elapsed time.
  Nothing renders it yet -- see the next bullet.
- `KF_PET_SESSION_ENABLE_DEBUG_TOOLS=0` on this build only. The desktop/
  headless backends' scrubbable debug timeline (`kf_pet_session_debug_
  seek()` and friends) backs onto a 2048-entry snapshot ring that costs
  200KB+ of static memory -- fine on a desktop process, not something a
  512KB-SRAM device should pay for a feature nothing on it calls. See
  `kf_pet_session.cpp`'s own top-of-file comment.
- No screen. `main/app_main.cpp` still runs `KF_DEMO_SPRITE` (the same
  placeholder from ADR 0020) for its display proof, and separately logs
  the pet's stage/needs/`base_trait` over serial every 10 seconds -- that
  log line is the only way to observe the pet on this build. LVGL isn't
  in this component tree yet (still just `hakoniwaos` in
  `EXTRA_COMPONENT_DIRS`), so there is no pet screen (ADR 0017) to hand it
  to.
- **`idf.py build` genuinely succeeded** on Chris's own machine, 2026-08-06:
  a fresh ESP-IDF v6.0.2 install (xtensa-esp-elf 15.2.0, Ubuntu under WSL)
  built this slice clean, producing `kamiframe-firmware.bin` at `0x41a20`
  bytes (~266KB) against a 1MB app partition -- 74% still free. A real bug
  surfaced and was fixed on the first attempt (a field-naming mistake in
  the new logging code); see ADR 0025's "Verified" section for the full
  story.

## What ADR 0026 added

- A real DS3231 driver, added directly to `esp_time.cpp` rather than a new
  file: `kf_time_init()` now tries once, at boot, to read the DS3231 over
  I2C and seed the wall clock from it, and `kf_time_set_wall()` now writes
  through to the chip as well as RAM. `ports/esp32/main/CMakeLists.txt`
  gained one new `REQUIRES` entry, `esp_driver_i2c`, the same component
  `ports/esp32-bringup` already uses for the same chip.
- Every failure path -- bus init fails, nothing answers at `0x68`, the
  device answers but isn't actually a DS3231 (its temperature register
  says so -- see ADR 0026 decision #4, guarding the documented DS3231/
  MPU-6050 address collision), or the OSF flag says the oscillator has
  stopped -- leaves the wall clock exactly as unset as it was before this
  slice. Nothing here can regress a board with no RTC wired.
- This closes the gap ADR 0025's own "What this slice does NOT reach"
  named: `kf_pet_session_init()`'s offline fast-forward now has something
  real to fast-forward across, on hardware where the DS3231 is present,
  wired correctly, and has a working backup cell.
- **`idf.py build` succeeded** on Chris's machine, 2026-08-06, same day as
  ADR 0025's own build: `esp_driver_i2c` linked cleanly, and
  `kamiframe-firmware.bin` grew to `0x45ff0` bytes (~280KB) against the
  same 1MB app partition -- 73% still free. Still not run against a
  physical DS3231 -- see ADR 0026's own "Not yet done" section.

## What still has to be written

- A partition table: firmware, OTA, NVS for save state, and an asset
  partition sized to `KF_FLASH_ASSET_BUDGET_BYTES` in `kf/budget.h`. Today's
  build uses ESP-IDF's default single-app partition table, which is enough
  to boot but not what real bring-up needs.
- Porting LVGL, then Lua, onto ESP-IDF -- each real, separate work, most
  likely each its own `EXTRA_COMPONENT_DIRS` entry and its own ADR, once
  there's real hardware to debug them against. The pet session itself
  (ADR 0025) no longer waits on either.
- A caller for `kf_time_set_wall()` in production -- ADR 0026 implemented
  the write-through correctly, but nothing (no settings screen, no NTP
  sync) actually calls it yet, so a DS3231 that lost its seed still needs
  a human re-running the bring-up diagnostic to recover, not an in-app fix.
- The real hardware bring-up pass: flash an actual board, confirm every pin
  in `kf_esp_pins.h`, measure the real achievable SPI clock (see below), and
  confirm the DS3231 driver added in ADR 0026 actually talks to a real chip.

## The number to check first at bring-up

`KF_DISPLAY_SPI_HZ` in `kf/budget.h` is currently an **assumption**: 40MHz,
which puts a full 240x320 RGB565 frame at about 30ms of wire time. Everything
the simulator says about transfer cost rests on it. Measure the real figure on
day one of bring-up and correct it.
