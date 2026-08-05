# ADR 0020: Real ESP32 HAL backends, compile-verified but not hardware-verified

**Status:** Accepted, 2026-08-05
**Reversal cost:** Medium. `ports/esp32/hal/kf_esp_pins.h`'s pin assignments
and `sdkconfig.defaults`' PSRAM/CPU settings are explicitly flagged
"assumption, not measured" and expected to change at bring-up -- cheap to
revise. The HAL function bodies themselves (SPI/esp_lcd sequencing, NVS
query-then-read pattern, deep-sleep contract) are the more expensive part to
redo, because getting them wrong would only show up once real hardware is
in hand, not from anything this environment can check.

## Requirement

Chris, with hardware parts ordered and arriving later: *"parts ordered. can
I not do anything else while waiting for the parts?"* Four options were
offered (ESP32 HAL backend scaffolding, evolution/life stages, a low-need
warning state, something else); Chris picked **ESP32 HAL backend
scaffolding**, on the reasoning that it can't be verified against real
hardware yet, but can be written and compile-checked now against the ESP-IDF
toolchain ADR 0019 already proved works in this environment -- so when parts
land, it's calibration and debugging against real code, not starting from a
blank file.

## Decision: write every backend for real, verify compile+link only

Every HAL header under `hakoniwaos/include/kf/hal/` now has a real ESP32
implementation under `ports/esp32/hal/`, each one researched against actual
ESP-IDF v6.0.2 source (`/tmp/esp-idf/components/...`) rather than
remembered or guessed API shapes -- the same discipline ADR 0019 already
established for this codebase, applied to eight files instead of one.

| Header | Backend file | What it's backed by |
|---|---|---|
| `kf/hal/log.h` | `esp_log.cpp` | `esp_log_write()` (the function every `ESP_LOGx` macro expands to -- used directly because `kf_log`'s level is a runtime argument, not a compile-time one), `esp_restart()` on panic |
| `kf/hal/entropy.h` | `esp_entropy.cpp` | `esp_random()`, a real hardware RNG |
| `kf/hal/memory.h` | `esp_memory.cpp` | `heap_caps_malloc()` against `MALLOC_CAP_INTERNAL\|MALLOC_CAP_DMA` and `MALLOC_CAP_SPIRAM` |
| `kf/hal/time.h` | `esp_time.cpp` | `esp_timer_get_time()` for mono; `gettimeofday()`/`settimeofday()` for wall (see "A real, named gap" below) |
| `kf/hal/input.h` | `esp_input.cpp` | `gpio_get_level()`, active-low, internal pull-ups |
| `kf/hal/display.h` | `esp_display.cpp` | `esp_lcd` + `driver/spi_master.h`: real ST7789-over-SPI+DMA |
| `kf/hal/storage.h` | `esp_storage.cpp` | ESP-IDF NVS (`nvs_set_blob`/`nvs_get_blob`/`nvs_commit`) |
| `kf/hal/power.h` | `esp_power.cpp` | `esp_sleep_enable_timer_wakeup()` + `esp_deep_sleep_start()` |

`ports/esp32/main/app_main.cpp` replaces ADR 0019's `main.c`: it keeps that
file's chip-info banner verbatim (already proven correct by Chris's real
Wokwi boot), then calls the real `kf_app_init(KF_DEMO_SPRITE)` /
`kf_app_frame()` loop from `kf/app.h` -- the same entry points
`sdl_main.cpp` drives on desktop -- against these backends instead of SDL3.

`ports/esp32/hal/kf_esp_pins.h` centralizes every GPIO assignment in one
file, explicitly marked **ASSUMPTION, NOT MEASURED**, and deliberately
avoids the one hard, non-negotiable constraint on this hardware: GPIO26-32
are wired inside the ESP32-S3-WROOM-1 N16R8 module itself to the octal
PSRAM and are not available for anything else, alongside the standard
strapping pins (0, 3, 45, 46) and native-USB pins (19, 20).

## What this slice does NOT reach, and why -- a real structural boundary, not an oversight

`app_main.cpp` drives `kf/demo.h`'s placeholder bouncing-sprite content
(`KF_DEMO_SPRITE`), not the pet. Three things a person watching this boot
might expect to see are structurally unreachable from this build, not
merely unfinished:

- **No pet.** `kf_pet_session.{h,cpp}` -- the code owning the live
  `kf_pet_state`, offline fast-forward, and per-frame decay (ADR 0015) --
  lives in `simulator/src/pet/`, which `simulator/CMakeLists.txt` says
  outright is "simulator-only orchestration around a Core mechanism, not a
  claim that the ESP32 build has it wired up."
- **No LVGL pet screen** (ADR 0017). `hakoniwaos/sources.cmake` -- the one
  source list both build systems read, per ADR 0002 -- contains no LVGL
  files; they live in `simulator/src/lvgl/`.
- **No Lua demo creature** (ADR 0018), for the identical reason:
  `simulator/src/lua/` is not in `hakoniwaos/sources.cmake` either.

`ports/esp32/CMakeLists.txt`'s `EXTRA_COMPONENT_DIRS` points at exactly one
directory, `hakoniwaos`. None of `simulator/src/pet`, `simulator/src/lvgl`,
or `simulator/src/lua` are reachable from an ESP-IDF build at all today --
this is not a missing `#include`, it's a missing component directory, and
adding one is real, separate work (most likely its own ADR per subsystem,
once there's hardware to debug each against), not something to guess at
compile-only. What genuinely runs, through every real backend in the table
above: the same placeholder content ADR 0019's own `kf/demo.h` quotes as
"the cheap case" -- proof the HAL backends work, not proof the game does.

## A real, named gap: the wall clock does not survive a genuine power-off

`esp_time.cpp`'s `kf_time_wall()` is backed by the chip's own internal
system clock (`gettimeofday()`/`settimeofday()`), not a DS3231. This matters
because doc 02's non-negotiable requirement -- the pet ages while switched
off -- depends on the wall clock surviving being switched off, and this
backend's clock does not survive a real power cycle: `g_wall_valid` resets
to `false` on every cold boot, identical to a brand-new device. A
`kf_power_deep_sleep_until()` call within one power-on session is fine (the
chip stays powered enough to keep ticking, or in real deep sleep the RTC
timer itself wakes it and the session's own logic re-sets the clock), but
unplug the board and come back next week, and this backend has no memory of
what time it was. The DS3231 RTC breakout already in the hardware shopping
list is the fix -- external I2C hardware with no bus pins decided yet in
`kf_esp_pins.h` and no driver written. This is the single most important
gap this slice leaves for later, named here rather than discovered the hard
way once a save file appears to lose real time.

## Two smaller, deliberate simplifications

**Backlight is on/off, not PWM.** `kf_display_set_backlight()` treats any
level above zero as "on" via a plain `gpio_set_level()` call. Dimming would
need an LEDC (PWM) channel, not wired up here -- a self-contained addition
later, not an architectural one.

**Display present ignores dirty rects.** `esp_display.cpp`'s
`kf_display_present()` pushes the full `240x320` frame with one
`esp_lcd_panel_draw_bitmap()` call every time, exactly like the desktop
backend does today (`sdl_display.cpp`), and reports
`supports_partial_update = false` honestly rather than claiming a
capability nothing here implements. Honouring the dirty-rect list for real
is a genuine future optimisation; a wrong partial-update implementation
that skips pixels it shouldn't would be a worse bug than an honest
full-frame push.

## Two real bugs, found only by the actual cross-compiler

**`volatile int` increment, rejected outright by C++26.** The first draft
of `esp_log.cpp`'s panic path used a hand-rolled busy-wait,
`for (volatile int i = 0; i < 2000000; ++i) {}`, to give the UART a moment
to flush before `esp_restart()`. `xtensa-esp32s3-elf-g++` (this project
builds `gnu++26`, per `-Werror`) rejected it: incrementing a `volatile int`
is deprecated as of C++26, and this project treats warnings as errors on
every target. Fixed with `esp_rom_delay_us(20000)` -- a ROM busy-wait
function, not a scheduler call, which is also the more correct tool for a
panic path that might run before FreeRTOS is fully up.

**A backward link dependency the automatic build system couldn't see.**
`idf.py build` failed at the final link step with a wall of `undefined
reference to kf_display_init` (and every other `kf_hal_*` symbol), despite
every one of those functions compiling clean and genuinely existing in
`libmain.a`. The cause: `hakoniwaos/src/app.cpp` calls these functions as
plain `extern "C"` symbols with no idea `main` supplies them -- that's the
whole point of the HAL boundary (ADR 0004) -- which means the actual
link-time dependency runs backward from what `main REQUIRES hakoniwaos`
declares. ESP-IDF's automatic archive-repeat logic (which resolves genuine
circular component dependencies) walks the declared `REQUIRES` graph, and
no component declares a dependency running from `hakoniwaos` back to
`main` -- correctly, since `hakoniwaos` must stay backend-agnostic -- so it
never repeated `libmain.a` in the final link line, and `libmain.a` got
scanned exactly once, before anything had referenced its HAL symbols yet.
Fixed by adding `WHOLE_ARCHIVE` to `main`'s `idf_component_register()` call
-- an ESP-IDF-documented option for exactly this situation, forcing the
whole component into the final binary regardless of reference-resolution
order. This is the identical *inversion-of-control* shape as the desktop
build's own `kamiframe_host_common`-calls-into-`hakoniwaos`-which-calls-
back-into-`kamiframe_host_common` pattern; CMake's ordinary executable
linking handles it there without needing anything special, so this bug
was specific to ESP-IDF's component-graph-based link line generation, not
to the architecture itself.

Both bugs are the same lesson ADR 0019 already drew, applied to a bigger
slice: untested code carries mistakes a real build surfaces and a design
review does not.

## Verified

- A full clean `idf.py fullclean && idf.py build`: 1083/1083 build steps,
  zero warnings, zero errors, against ESP-IDF v6.0.2's `-Wall -Wextra
  -Werror` for `esp32s3` -- every one of the eight new `hal/*.cpp` files,
  `app_main.cpp`, and every file in `hakoniwaos/sources.cmake` compiled
  clean for the real target. Produces `build/kamiframe-firmware.elf`
  (4.8MB, includes debug info) and `.bin` (0x40430 bytes; smallest app
  partition 0x100000 bytes, 75% free).
- The desktop build, rebuilt clean afterward (`cmake --build`): clean,
  only the pre-existing, unrelated `liblua_core.a` `tmpnam` warning (from
  vendored Lua source, untouched by this slice).
- All 10 desktop `ctest` targets pass, confirming nothing this slice
  touched (only `ports/esp32/`) affected the shared `hakoniwaos` behaviour
  the desktop build also exercises.
- Every struct field and function signature used in `esp_display.cpp`
  (`spi_bus_config_t`, `esp_lcd_panel_io_spi_config_t`,
  `esp_lcd_panel_dev_config_t`, and the `esp_lcd_panel_*` call sequence)
  was confirmed against real ESP-IDF v6.0.2 header and source files before
  writing the call, including reading `esp_lcd_panel_st7789.c` itself to
  confirm `LCD_RGB_DATA_ENDIAN_LITTLE` reprograms the panel's own RAMCTL
  register rather than requiring host-side byte-swapping, and reading
  `esp_lcd_panel_io_spi.c` to confirm the default (all-zero)
  `esp_lcd_panel_io_spi_config_t.flags` produce the standard "DC low =
  command, DC high = data" wiring this panel uses.

**Explicitly NOT verified here, and why:**

- **No real hardware.** Every backend above is compile-and-link verified
  against the real ESP-IDF toolchain, never run. Whether `kf_esp_pins.h`'s
  pin assignments are actually wireable the way assumed, whether the SPI
  clock in `kf/budget.h`'s `KF_DISPLAY_SPI_HZ` is achievable, whether the
  ST7789 panel actually responds to this exact init sequence, whether the
  buttons debounce cleanly with only internal pull-ups -- none of this is
  known until a board is in hand. This is exactly the gap Chris's own
  framing of the task named: compile-checked scaffolding, not hardware
  proof, so that landing parts becomes calibration and debugging against
  real code rather than starting from nothing.
- **No functional correctness beyond "it compiles, links, and the API
  calls match real ESP-IDF signatures."** Nothing here exercises whether
  the NVS query-then-read pattern in `esp_storage.cpp` actually round-trips
  a value correctly, whether `esp_power.cpp`'s deep-sleep timer wakeup
  actually fires at the right time, or any other runtime behavior --
  Wokwi does not simulate SPI-attached displays, NVS flash timing, or deep
  sleep in a way that would exercise these meaningfully differently from
  the compile check itself, so this slice did not attempt a Wokwi run the
  way ADR 0019 did for the serial-only hello-world.

## Later

- Wire a real DS3231 over I2C (bus pins not yet in `kf_esp_pins.h`) to
  close the wall-clock gap described above -- this is the highest-priority
  follow-up, since it's the one gap that silently breaks a non-negotiable
  product requirement rather than merely limiting scope.
- Port `kf_pet_session`, then LVGL, then Lua onto ESP-IDF, each most likely
  its own `EXTRA_COMPONENT_DIRS` entry and its own ADR, once real hardware
  exists to debug each against -- not attempted here since doing so
  compile-only, without hardware to catch what's wrong, risks writing
  three layers of unverified guesswork on top of each other.
- LEDC-based backlight dimming, once `has_backlight`'s current on/off
  behavior is felt to be a real limitation rather than a fine bring-up
  default.
- Honour the dirty-rect list in `kf_display_present()` for real once
  `KF_DISPLAY_SPI_HZ`'s real, measured value (still "assumption, not
  measured" per ADR 0019) makes clear whether the full-frame-every-time
  cost is actually a problem worth solving.
- The real hardware bring-up pass itself: flash a real ESP32-S3-WROOM-1
  N16R8 board, confirm every pin in `kf_esp_pins.h`, measure the actual
  achievable `KF_DISPLAY_SPI_HZ`, and correct anything this slice's
  "assumption, not measured" table got wrong.
