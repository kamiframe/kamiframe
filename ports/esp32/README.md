# ports/esp32

**A real ESP-IDF hello-world. Compiles clean for esp32s3 and boots in
Wokwi, confirmed by a real run against a real `WOKWI_CLI_TOKEN`. Not yet
run on real hardware.** See ADR 0019.

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

## What still has to be written (Phase 1b, not this slice)

- `sdkconfig.defaults` currently sets only the target chip and a 16MB flash
  size (the real hardware's spec). Wokwi's simulated
  `board-esp32-s3-devkitc-1` reports its own flash size (4MB) at runtime
  regardless of this setting -- see ADR 0019 -- so this value is aimed at
  real hardware, not at matching Wokwi. Still missing: PSRAM enabled in
  octal mode, C++ exceptions and RTTI left off (they are off by default
  already, but not yet asserted here), CPU at 240MHz.
- A partition table: firmware, OTA, NVS for save state, and an asset
  partition sized to `KF_FLASH_ASSET_BUDGET_BYTES` in `kf/budget.h`. Today's
  build uses ESP-IDF's default single-app partition table, which is enough
  to boot but not what real bring-up needs.
- HAL backends under `ports/esp32/hal/`: ST7789 over SPI with DMA, GPIO input,
  `esp_timer` plus an external RTC, `esp_random`, and internal-SRAM versus
  PSRAM pools via `heap_caps_malloc` with `MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA`
  and `MALLOC_CAP_SPIRAM`.
- `main/main.c` today is a plain hello-world (ADR 0019) -- it does not spawn
  the FreeRTOS task that calls `kf_app_frame()` yet, deliberately: that call
  needs the HAL backends above to exist first, or it fails to link. See
  `kf/app.h`.

## The number to check first at bring-up

`KF_DISPLAY_SPI_HZ` in `kf/budget.h` is currently an **assumption**: 40MHz,
which puts a full 240x320 RGB565 frame at about 30ms of wire time. Everything
the simulator says about transfer cost rests on it. Measure the real figure on
day one of bring-up and correct it.
