# ADR 0027: LVGL on ESP32 -- the pet screen, on the real panel

**Status:** Accepted
**Date:** 2026-08-08

## Context

ADR 0025 got a live `kf_pet_state` ticking for real on the device -- saved to
and loaded from real NVS, decaying against real elapsed time -- but the only
way to see it was a `KF_LOGI` line every ten seconds. That ADR's own "Context"
section named exactly why: `ports/esp32/CMakeLists.txt`'s
`EXTRA_COMPONENT_DIRS` pointed at `hakoniwaos` alone, and LVGL "earning its
own component the way hakoniwaos itself does" was explicitly left for later,
since it is a real external dependency and the pet session is not.

The pet screen itself (ADR 0017) already exists and already works on
desktop -- `kf_pet_screen.cpp`, reached via `kf_screen_nav.cpp` (ADR 0022).
Neither depends on Lua: they read `kf_pet_session_state()` and call
`kf_pet_session_feed()`/`_play()`/`_rest()`, the same C++ pet-session API
`app_main.cpp` already called before this slice. This slice's job is
entirely plumbing -- get LVGL reachable from the ESP-IDF build, without
forking or copying a single line of the port glue that already works.

## Decision

### 1. A new component, `kamiframe_lvgl_port`, earning what ADR 0025 deferred

`ports/esp32/kamiframe_lvgl_port/CMakeLists.txt` -- a real ESP-IDF
component, added to `EXTRA_COMPONENT_DIRS` in `ports/esp32/CMakeLists.txt`
alongside `hakoniwaos`. It owns no source of its own: its `SRCS` list
references `simulator/src/lvgl/kf_lvgl_pool.cpp`, `kf_lvgl_tick.cpp`,
`kf_lvgl_display.cpp`, `kf_lvgl_input.cpp`, `kf_lvgl_port.cpp`,
`kf_pet_screen.cpp`, `kf_pet_info_screen.cpp` and `kf_screen_nav.cpp` by
relative path -- the exact "one canonical file, not a fork" pattern
`ports/esp32/main/CMakeLists.txt` already uses for `kf_pet_session.cpp`,
just wrapped in `idf_component_register()` instead of pasted into `main`'s
own `SRCS`, because this module pulls in a third-party dependency
(LVGL) and `kf_pet_session.cpp` does not.

`kf_pet_info_screen.cpp` is in that list even though the task that started
this slice didn't originally name it: `kf_screen_nav.cpp` calls
`kf_pet_info_screen_init()`/`_update()` directly (ADR 0022's Info screen),
so it is a hard compile dependency, not an optional extra. Checked against
its own includes before assuming it was portable -- it is (`<lvgl.h>`,
`../pet/kf_pet_session.h`, `kf/hal/log.h`, `kf/pet.h`, same shape as
`kf_pet_screen.cpp`), so it earns the same treatment.

`kf_lvgl_pointer.cpp` (the desktop mouse cursor) is deliberately **not** in
the list -- its own header comment already says "no ESP32 counterpart and
never will." See decision 4.

### 2. LVGL fetched via the same `cmake/fetch_lvgl.cmake`, not a second mechanism

Considered three shapes for getting LVGL into the ESP-IDF build: a managed
component from the ESP Component Registry, a git submodule vendored
straight into the repo, or reusing the desktop's own `FetchContent`-based
`cmake/fetch_lvgl.cmake`. The managed-component route means Kconfig-driven
configuration, not this project's one curated `lv_conf.h` (ADR 0013's whole
point -- five widgets, a PSRAM-backed pool allocator, one file both builds
share); a submodule means a second place the pinned version could drift
from the desktop's. Reusing `fetch_lvgl.cmake` unmodified in mechanism --
same `KAMIFRAME_LVGL_TAG` (`v9.2.2`), same `LV_CONF_PATH` pointing at
`simulator/src/lvgl/lv_conf.h` -- means there is exactly one LVGL
configuration to reason about on either target, which is the same
reasoning `cmake/fetch_lua.cmake` and `fetch_sdl.cmake` already established
for this project's other third-party dependencies.

The file needed two small, genuinely portable fixes to actually be
callable from a second location (see "Found after delivery" below), not a
rewrite: every path in it now resolves against `CMAKE_CURRENT_LIST_DIR`
(this file's own directory, `cmake/`, fixed regardless of caller) instead
of `CMAKE_CURRENT_SOURCE_DIR` (the *including* file's directory, which used
to work only because `simulator/CMakeLists.txt` was the only caller).

### 3. `kf_lvgl_pointer.cpp` is guarded out of `kf_lvgl_port.cpp`, not ported

`kf_lvgl_port.cpp`'s `kf_lvgl_port_init()` unconditionally called
`kf_lvgl_pointer_init(group)` -- a desktop-only mouse-cursor `lv_indev_t`
backed by `kf_sim_pointer_poll()`, implemented once per desktop backend
(`sdl_input.cpp`, `headless_input.cpp`) and, by its own header comment,
never meant to exist on ESP32. Rather than add a third, no-op ESP32
implementation of `kf_sim_pointer_poll()` just to keep the call
unconditional, the one call site (and its `#include`) is wrapped in
`#ifndef ESP_PLATFORM` -- the same macro `hakoniwaos/CMakeLists.txt`
already branches its own build rule on, ESP-IDF's own build-wide compile
definition (`tools/cmake/build.cmake`), not something invented for this
change. `kf_lvgl_port.cpp` stays the one canonical file for both targets;
the keypad group it returns is unaffected either way, since ESP32 simply
never adds a second `lv_indev_t` to it.

### 4. `KF_DEMO_NONE` replaces `KF_DEMO_SPRITE` in `app_main.cpp`

`kf/demo.h`'s own `KF_DEMO_NONE` comment already names the failure mode:
`KF_DEMO_SPRITE`'s bouncing sprite continuously erases the small patch it
moved off of, and LVGL's partial-render mode only re-flushes pixels where
its own object tree changed -- so an erased patch under a static LVGL
widget stays erased, visible as a black trail. ADR 0013 diagnosed this on
desktop before a real device ever ran LVGL at all; `sdl_main.cpp` already
made the identical switch once its own screen went in. `app_main.cpp` now
calls `kf_app_init(KF_DEMO_NONE)`, then `kf_pet_session_init()`,
`kf_lvgl_port_init()` and `kf_screen_nav_init()`, then per frame
`kf_pet_session_frame(0)` → `kf_screen_nav_frame()` → `kf_lvgl_port_pump(0)`
-- the exact order and the exact reasoning `sdl_main.cpp`'s own comments
already give for why the pet session and the active screen must run
*before* LVGL's `lv_timer_handler()` redraws and flushes.

The flush path itself needed no new code: `kf_lvgl_display.cpp`'s
`flush_cb()` already writes into `kf_fb_pixels()` and calls
`kf_fb_mark_dirty()`, and `hakoniwaos/src/app.cpp`'s existing
`kf_app_frame()` already calls `kf_display_present()` with whatever is
dirty, every frame, regardless of what wrote it. Wiring LVGL in did not
touch `kf_display_present()`, `esp_display.cpp`, or `kf_esp_pins.h` at
all -- out of scope for this slice by instruction, and turned out to need
no exception.

## Found after delivery

Three real problems, none guessed at, all found by an actual
`idf.py build`, not reasoned about in advance:

**LVGL's own build system self-registers as an ESP-IDF component when
`ESP_PLATFORM` is set, colliding with the one wrapping it.** LVGL v9.2.2
ships `env_support/cmake/esp.cmake`, included automatically by its own
`CMakeLists.txt` whenever `ESP_PLATFORM` is truthy, which calls
`idf_component_register()` itself -- intended for use as a top-level
managed/`EXTRA_COMPONENT_DIRS` component, not for being fetched from
*inside* an already-registered component's own `CMakeLists.txt`. Because
`FetchContent_MakeAvailable(lvgl)`'s `add_subdirectory()` runs as a child
CMake scope of `kamiframe_lvgl_port`'s own directory, ESP-IDF's
directory-scoped "current component" context is inherited by that child
scope, so LVGL's self-registration tried to create a *second* target under
the exact same name (`__idf_kamiframe_lvgl_port`) as the one already
registered a few lines above it -- `CMake Error ... add_library cannot
create target "__idf_kamiframe_lvgl_port" because another target with the
same name already exists`. Fixed by locally shadowing `ESP_PLATFORM` to
`OFF` (a plain, non-`CACHE` `set()`, restored immediately after) for
exactly the duration of `include(fetch_lvgl.cmake)` -- this routes LVGL
down its *other* branch, `custom.cmake`'s plain `add_library(lvgl ...)`,
the same code path the desktop build already takes (desktop never has
`ESP_PLATFORM` set at all), which is exactly what `fetch_lvgl.cmake` was
already written against. Nothing about the fix is ESP32-specific; it just
stops LVGL from noticing it's being built for ESP32 for the narrow moment
its own fetch runs.

**ESP-IDF's "requirements scan" pass runs component `CMakeLists.txt` code
in a restricted CMake mode that breaks `FetchContent`.** Before the real
configure, `tools/cmake/scripts/component_get_requirements.cmake` walks
every candidate component and `include()`s its `CMakeLists.txt` to
discover `REQUIRES`/`PRIV_REQUIRES`, with `idf_component_register()`
redefined as a macro that captures its arguments and immediately
`return()`s. Everything in a component's `CMakeLists.txt` *before* that
call still runs in that pass, in its own genuinely restricted execution
context -- `FetchContent_Declare`'s internal `define_property()` call
isn't valid there (`CMake Error ... define_property command is not
scriptable`), so having `include(fetch_lvgl.cmake)` above
`idf_component_register()` broke `idf.py set-target` outright, before a
single line of C++ compiled. Fixed by moving the `include()` to *after*
`idf_component_register()` -- the standard, documented shape for exactly
this situation, and one that costs nothing here since this component's
`SRCS`/`INCLUDE_DIRS` never depended on LVGL being fetched yet.

**Two ARM-only LVGL SIMD assembly files aren't assembly-safe outside an
ARM+assembly-tolerant-libc environment.**
`src/draw/sw/blend/helium/lv_blend_helium.S` and its NEON counterpart
`lv_blend_neon.S` unconditionally `#include lv_conf_internal.h` -- a plain
C header, never written expecting to be fed to an assembler -- two lines
above the `#if defined(__ARM_FEATURE_MVE) ...` guard that correctly
excludes the file's actual SIMD body on any non-ARM target. That guard
never gets a chance to matter: parsing `lv_conf_internal.h`'s C
declarations as assembly mnemonics fails immediately (`unknown opcode or
format name 'div_t'`, and hundreds more, straight out of picolibc's
`stdlib.h`/`assert.h`). Desktop never hit this: the root `CMakeLists.txt`'s
`project()` call never enables the `ASM` language (`LANGUAGES C CXX` only),
so CMake has silently never compiled either `.S` file there. ESP-IDF
enables `ASM` project-wide (its own startup/vector code needs it), which
is what exercised this LVGL bug for the first time. Fixed in
`fetch_lvgl.cmake`, after `FetchContent_MakeAvailable(lvgl)`: both files
are filtered out of the `lvgl` target's `SOURCES` property by name. Applies
to both builds, not just ESP32 -- neither Xtensa nor this project's
desktop targets (x86-64/ARM64, never Cortex-M) can ever legitimately need
Helium or NEON kernels, so pruning them is correct regardless of which
build happens to compile `.S` files today.

**One genuinely platform-specific `-Werror` warning**, unrelated to any of
the above: `kf_lvgl_display.cpp`'s `KF_LOGI` call passed `kDrawBufRows`
(`constexpr int32_t`) to a `%d` format specifier. On the desktop toolchain
`int32_t` is `int`, so `%d` matched; on `xtensa-esp32s3-elf`'s toolchain
`int32_t` is `long` (both 32 bits, but distinct types), so it did not.
Fixed with an explicit `static_cast<int>(kDrawBufRows)` at the call site --
correct on both targets, not just the one that happened to warn first.

## What this slice does NOT reach

- **Lua.** Out of scope by instruction. The pet screen needs none of it;
  see "Context" above for exactly why.
- **Offline ageing on real hardware.** Unchanged from ADR 0025/0026's own
  scope -- this slice adds a screen for the pet that was already ticking,
  it does not touch `kf_time_wall()` or the DS3231 driver.
- **The display driver itself.** `esp_display.cpp` and `kf_esp_pins.h` were
  explicitly off-limits (another slice owns them concurrently) and turned
  out not to need touching either -- LVGL's flush path goes through the
  existing `kf_fb_pixels()`/`kf_fb_mark_dirty()`/`kf_display_present()`
  chain unchanged.
- **Touch/pointer input on the device.** Never applicable -- see decision
  3; the keypad indev (buttons) is the only input LVGL has on ESP32, same
  as the real hardware's actual button count.

## Verified

- `idf.py set-target esp32s3` then a full `idf.py fullclean` + `idf.py
  build` succeed clean from scratch, twice in a row (the second a genuine
  cold rebuild after `fullclean`), for the `esp32s3` target, against real
  ESP-IDF v6.0.2. `grep -i warning` over the complete build log: zero
  matches, satisfying `-Wall -Wextra -Werror` project-wide, LVGL's own
  vendored source included (suppressed deliberately via `fetch_lvgl.cmake`'s
  existing `-w` on the `lvgl` target, same as the desktop build already
  does -- LVGL is not code this project owns or holds to its own bar).
- `kamiframe-firmware.bin`: `0x72340` bytes (~445KB) against a `0x100000`
  (1MB) app partition, 55% free -- LVGL, the pet screen, and Info screen
  all linked in alongside everything ADR 0025 already had.
- Link order needed no `WHOLE_ARCHIVE` on the new component: `main`
  (`WHOLE_ARCHIVE`, unchanged from ADR 0020) directly references
  `kf_lvgl_port_init()`/`kf_screen_nav_init()`, which pulls
  `kamiframe_lvgl_port.a` in through the ordinary single-pass link ESP-IDF
  already uses (no `--start-group`/`--end-group` in this project's link
  line -- confirmed by reading `tools/cmake/project.cmake`, not assumed);
  the one genuine circular reference (LVGL calling
  `kf_lvgl_mem_pool_alloc()`, defined in `kamiframe_lvgl_port`) is resolved
  the same way `simulator/CMakeLists.txt` already resolves the identical
  cycle for desktop -- a reciprocal `target_link_libraries()` pair, which
  CMake's own generator repeats on the final link line for a detected
  static-library cycle either way. No link errors of the WHOLE_ARCHIVE
  story's shape (ADR 0020's own "found the hard way" section) recurred.
- The desktop build was re-verified after every change to shared files
  (`cmake/fetch_lvgl.cmake`, `simulator/src/lvgl/kf_lvgl_port.cpp`,
  `simulator/src/lvgl/kf_lvgl_display.cpp`): `kamiframe_lvgl_port` and
  `kamiframe-headless` both rebuild clean from scratch, and
  `lvgl_determinism_check`, `pet_screen_check` and `screen_nav_check` all
  still pass with their existing golden checksums unchanged -- confirming
  none of the ESP32-side fixes altered desktop rendering or behaviour.
  (`kamiframe-sim`, the SDL3 target, could not be verified in this
  sandbox -- see "Not verified.")
- `ports/esp32/hal/esp_display.cpp` and `ports/esp32/hal/kf_esp_pins.h`:
  confirmed byte-for-byte untouched (`git diff` against both is empty).

## Not verified

- **Anything on real hardware.** This ADR is a clean cross-compile and
  link only, the same status ADR 0020's own "Verified" section describes
  for its own slice, and the same caveat ADR 0024/0025/0026 each repeat:
  `idf.py build` succeeding is not a claim about what a real ST7789 (or
  the ILI9341 bring-up has actually been measuring against, per
  `KF_DISPLAY_SPI_HZ`'s own comment) shows when powered on. No `idf.py
  flash monitor` run happened as part of this slice.
- **`kamiframe-sim` (the SDL3 desktop target) in this sandbox.** SDL3
  itself fails to compile here on an unrelated macOS SDK/GameController
  API mismatch (`property 'physicalInputProfile' not found on object of
  type 'GCController *'`, `MTLPixelFormatB5G6R5Unorm` unavailable) --
  pre-existing, confirmed unrelated to this slice (no file this slice
  touches is anywhere in that call stack), and reproducible on a clean
  checkout before any of this ADR's changes. `kamiframe-headless`, which
  exercises the identical `kamiframe_lvgl_port` code and needs no SDL3,
  built and passed cleanly instead -- see "Verified" above.
- **Whether the Info screen (`kf_pet_info_screen.cpp`) is reachable via a
  real MENU button press on hardware.** `kf_screen_nav_frame()` reads
  `kf_app_buttons_pressed()`, which is real debounced state from
  `esp_input.cpp`'s GPIO polling (ADR 0020) -- untouched by this slice --
  but no physical button has been pressed against this build.

## Cost to change

Reverting to `KF_DEMO_SPRITE` (or adding a coordinated hybrid) is a
one-line change in `app_main.cpp`, same as it always was. Dropping
`kamiframe_lvgl_port` entirely -- if a future slice decided against LVGL
on-device after all -- is one `EXTRA_COMPONENT_DIRS` line and one
`REQUIRES` entry in `main`, plus reverting `app_main.cpp`'s four new calls;
nothing in `hakoniwaos/` or the HAL backends references it. If the LVGL
version is ever bumped and reintroduces (or worsens) the Helium/NEON `.S`
issue, `fetch_lvgl.cmake`'s `EXCLUDE REGEX` filter is the one place to
extend -- it already runs by filename pattern, not a fixed list assuming
exactly two files. Wiring the demo creature (Lua) in behind this screen is
future work this slice deliberately left alone; nothing here blocks it.

## Superseded in part

**"The pet screen itself (ADR 0017) already exists and already works on
desktop -- `kf_pet_screen.cpp`, reached via `kf_screen_nav.cpp`"** described
the live Home screen as of this ADR's date (2026-08-08); it no longer is.
Home is `kf_creature_screen.cpp` or a `kf.screen("home")` Lua group now
(ADR 0021, 0044, 0045), and `kf_pet_screen.cpp` is reachable only through
`pet_screen_check`'s direct call under `-DKF_ENABLE_LVGL=ON`.
`kf_pet_info_screen.cpp`, named throughout this ADR as a hard compile
dependency of `kf_screen_nav.cpp`, is deleted (ADR 0045 — Info moved to a
`kf.screen("info")` Lua group). `kf_screen_nav.cpp` itself moved out of
`simulator/src/lvgl/` to `simulator/src/pet/` and no longer includes
`<lvgl.h>` at all (ADR 0044/0045), so the plumbing this ADR built for
getting LVGL onto the ESP32 build remains valid, but the component's `SRCS`
list quoted above (decision 1) no longer matches
`ports/esp32/kamiframe_lvgl_port/CMakeLists.txt`'s actual contents.
