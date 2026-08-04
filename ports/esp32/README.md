# ports/esp32

**Not built yet. Nothing here has ever been compiled or flashed.**

This directory exists to document the intended shape of the ESP-IDF build and
to force the repository layout to be compatible with it from the first commit.
It is deliberately a skeleton: writing a firmware entry point that cannot be
compiled or run produces code that has never been correct, and pretending
otherwise is worse than an empty folder.

## When this gets filled in

Phase 1b of `04-roadmap-diy-release.md`, after the hardware trigger is met:

- the demo pet runs in the simulator under enforced constraints, with save and
  offline fast-forward working, **and**
- an ESP-IDF hello-world boots in [Wokwi](https://wokwi.com) (browser-based
  ESP32-S3 simulation with an ST7789, no parts required).

Only then are parts ordered.

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

## What still has to be written

- `sdkconfig.defaults`: PSRAM enabled in octal mode, flash size 16MB, C++
  exceptions and RTTI left off (they are off by default), CPU at 240MHz.
- A partition table: firmware, OTA, NVS for save state, and an asset
  partition sized to `KF_FLASH_ASSET_BUDGET_BYTES` in `kf/budget.h`.
- HAL backends under `ports/esp32/hal/`: ST7789 over SPI with DMA, GPIO input,
  `esp_timer` plus an external RTC, `esp_random`, and internal-SRAM versus
  PSRAM pools via `heap_caps_malloc` with `MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA`
  and `MALLOC_CAP_SPIRAM`.
- A `main` component that spawns a FreeRTOS task calling `kf_app_frame()`.
  It owns the loop; core does not. See `kf/app.h`.

## The number to check first at bring-up

`KF_DISPLAY_SPI_HZ` in `kf/budget.h` is currently an **assumption**: 40MHz,
which puts a full 240x320 RGB565 frame at about 30ms of wire time. Everything
the simulator says about transfer cost rests on it. Measure the real figure on
day one of bring-up and correct it.
