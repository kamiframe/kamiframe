# ports/esp32

**Real ESP32 HAL backends -- display, input, time, memory, entropy,
logging, storage, power -- all compiling and linking clean against the real
ESP-IDF v6.0.2 toolchain, driving the real `kf_app_init()`/`kf_app_frame()`
loop from `kf/app.h`. Not yet run against real hardware, and not yet
running the pet, LVGL, or Lua (see ADR 0020 for exactly why not).** See
ADR 0019 for the hello-world this was built on top of, and ADR 0020 for
this slice.

This directory used to be a documented skeleton, never compiled. It now
builds and boots: `idf.py build` produces a real `kamiframe-firmware.elf`/
`.bin` for the esp32s3 target, `hakoniwaos`'s entire source list compiles
through the real xtensa cross-compiler as part of that build, and
`wokwi-cli .` (Chris's own machine, his own token -- this environment
cannot generate one) ran that exact binary through Wokwi's
`board-esp32-s3-devkitc-1` simulation and captured a clean boot: real ROM
bootloader output, into `app_main()`, printing every line `main.c` writes.
See ADR 0019's "Verified" section for the full log and exactly what still
hasn't been checked (real hardware).

## Phase 1's exit trigger is now met

Phase 1b of `04-roadmap-diy-release.md` begins once both are true:

- the demo pet runs in the simulator under enforced constraints, with save and
  offline fast-forward working (true since ADR 0017), **and**
- an ESP-IDF hello-world boots in [Wokwi](https://wokwi.com) (browser-based
  ESP32-S3 simulation) -- true since the Wokwi run described above.

Both are now satisfied. Parts can be ordered whenever it's time to start
Phase 1b's real HAL work.

**Correction to this section's own past claim:** the ST7789 parenthetical
above used to say Wokwi simulates "an ST7789, no parts required." It
doesn't -- Wokwi's supported-hardware list does not include the ST7789 at
all; the closest part it has is `wokwi-ili9341` (also 240x320 SPI). This
hello-world sidesteps the question entirely by not wiring up a display (see
`diagram.json`), matching the official `wokwi/esp-idf-hello-world` reference
project's own scope. Whatever panel this project eventually simulates on
Wokwi, it will not be simulating the exact ST7789 real hardware will use --
worth knowing before ordering parts on the strength of a Wokwi screenshot.

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
  internal pull-ups), `esp_time.cpp` (`esp_timer` for mono; the chip's own
  system clock for wall -- **not** a real RTC yet, see ADR 0020's "A real,
  named gap"), `esp_memory.cpp` (`heap_caps_malloc` against
  `MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA` and `MALLOC_CAP_SPIRAM`),
  `esp_entropy.cpp` (`esp_random()`), `esp_log.cpp` (`esp_log_write` plus a
  panic-and-reboot path), `esp_storage.cpp` (ESP-IDF NVS), `esp_power.cpp`
  (real deep sleep).
- `sdkconfig.defaults` now also enables octal-mode PSRAM
  (`CONFIG_SPIRAM_MODE_OCT`) and 240MHz CPU
  (`CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240`) -- both needed the moment
  `esp_memory.cpp`'s `MALLOC_CAP_SPIRAM` pool has to actually exist. Flash
  size unchanged from ADR 0019.
- `main/app_main.cpp` replaces `main.c`: it calls the real
  `kf_app_init()`/`kf_app_frame()` loop against the backends above. It does
  **not** run the pet session, LVGL, or Lua -- that's not a missing call,
  it's a missing component directory (`EXTRA_COMPONENT_DIRS` only points at
  `hakoniwaos`); see ADR 0020's "What this slice does NOT reach."

## What still has to be written

- A real DS3231 RTC driver over I2C, to close the wall-clock gap
  `esp_time.cpp` currently leaves open -- see ADR 0020.
- A partition table: firmware, OTA, NVS for save state, and an asset
  partition sized to `KF_FLASH_ASSET_BUDGET_BYTES` in `kf/budget.h`. Today's
  build uses ESP-IDF's default single-app partition table, which is enough
  to boot but not what real bring-up needs.
- Porting `kf_pet_session`, then LVGL, then Lua onto ESP-IDF -- each real,
  separate work, most likely each its own `EXTRA_COMPONENT_DIRS` entry and
  its own ADR, once there's real hardware to debug them against.
- The real hardware bring-up pass: flash an actual board, confirm every pin
  in `kf_esp_pins.h`, measure the real achievable SPI clock (see below).

## The number to check first at bring-up

`KF_DISPLAY_SPI_HZ` in `kf/budget.h` is currently an **assumption**: 40MHz,
which puts a full 240x320 RGB565 frame at about 30ms of wire time. Everything
the simulator says about transfer cost rests on it. Measure the real figure on
day one of bring-up and correct it.
