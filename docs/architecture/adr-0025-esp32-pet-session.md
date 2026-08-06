# ADR 0025: The pet session, running for real on ESP32

**Status:** Accepted
**Date:** 2026-08-06

## Context

ADR 0020 got the ESP32 HAL backends compiling and linking against real
ESP-IDF, and proved it in Wokwi. It deliberately stopped there: `app_main.cpp`
drove `kf_app_init()`/`kf_app_frame()` against `kf/demo.h`'s placeholder
sprite, not the pet -- `kf_pet_session.{h,cpp}` (the code that owns the live
`kf_pet_state`, offline fast-forward, and per-frame decay) lived in
`simulator/src/pet/`, reachable from the desktop and headless backends only.
`ports/esp32/CMakeLists.txt`'s `EXTRA_COMPONENT_DIRS` pointed at `hakoniwaos`
alone.

Real hardware arrives 2026-08-07. Bring-up (ADR 0024) proves the wiring;
it does not prove the game. This slice closes part of that gap ahead of
bring-up, on the software side only, so there is something real for the
board to run the moment bring-up passes: a genuinely live pet, saved to and
loaded from real NVS, decaying against real elapsed time -- not a claim
about wiring, since none of this has run on physical hardware yet (see
"Not verified" below).

LVGL and Lua are explicitly **not** part of this slice. They are each a
real external dependency the desktop build fetches at configure time
(`fetch_lvgl`/`fetch_lua` in `simulator/CMakeLists.txt`), unlike the pet
session, which depends on nothing beyond `hakoniwaos` (already a `main`
`REQUIRES`). Porting either onto ESP-IDF's component system is its own
piece of work, most likely its own `EXTRA_COMPONENT_DIRS` entry and its
own ADR, and neither has to happen before the other -- the pet session
does not need a screen to be alive, and does not need Lua to decay
correctly. Building it alone first, and proving it alone first, keeps this
change small enough to review and small enough that if hardware bring-up
turns up a surprise (a pin move, a different display interface), nothing
here is affected -- see "Cost to change" below.

## Decision

### 1. Reference `kf_pet_session.cpp` by relative path, not a new component

Added directly to `main`'s `SRCS`/`INCLUDE_DIRS` in `ports/esp32/main/
CMakeLists.txt`, the same pattern already used for every HAL backend in
`../hal/`: one canonical file, not a fork, no new `EXTRA_COMPONENT_DIRS`
entry. It has exactly one dependency (`hakoniwaos`, already declared), so
it does not earn its own component the way a real external library would
-- unlike the LVGL/Lua work still ahead, which will.

### 2. The debug snapshot ring is compiled out on this build

`kf_pet_session.h`'s "DEBUG ONLY" section (`kf_pet_session_debug_advance()`/
`_reset()`/`_age_seconds()`/`_seek()`, used only by `sdl_main.cpp`'s debug
key bindings and the debug window's draggable timeline) backs onto a
2048-entry ring of full `kf_pet_state` snapshots, sized for a desktop
process and never audited against a device budget because there was no
device to audit it against. At roughly 100-120 bytes per snapshot that is
200KB+ of unconditional static memory -- on an ESP32-S3 with 512KB of
internal SRAM, that is not a rounding error, and nothing on this build
calls any of those four functions.

Rejected: leaving it in as dead code. "Unused" is not "free" for a static
array; it is allocated whether or not anything reads it.

Rejected: an `#ifdef ESP_PLATFORM` inside the shared file. Every other
backend difference in this codebase lives in a separate file or a separate
translation unit compiled differently, never a platform check inside
shared logic -- see `hakoniwaos/CMakeLists.txt`'s own `if(ESP_PLATFORM)`
branch, which switches the *build rule*, not the *source*.

What this does instead: a new compile-time flag,
`KF_PET_SESSION_ENABLE_DEBUG_TOOLS`, defaulting to 1 (so every existing
backend keeps today's behaviour with zero changes to their build files) and
set to 0 explicitly in `ports/esp32/main/CMakeLists.txt` via
`target_compile_definitions()`. All four debug functions stay **declared**
unconditionally in `kf_pet_session.h` -- callers never need to know which
backend they're in -- but are only **defined** when the flag is on. The
`Session` struct's snapshot-ring members, `DebugSnapshot` itself, and the
two now-debug-only helper functions (`elapsed_before_stage()`,
`total_age_seconds()`) move inside the same `#if`. Calling one of the four
from ESP32 code is a link error, not silently-wrong behaviour, which is
the correct outcome for a function this build was never supposed to reach.

### 3. `app_main.cpp` keeps `KF_DEMO_SPRITE` and adds the pet session alongside it

The pet session never touches the framebuffer -- it owns its own state
struct and talks to NVS, nothing else -- so there is nothing to coordinate
with the placeholder sprite. `kf_app_init(KF_DEMO_SPRITE)` stays exactly
as ADR 0020 left it, still the cheapest available proof that the display
HAL and dirty-rect path work. `kf_pet_session_init()` is called immediately
after, matching `sdl_main.cpp`'s own ordering (it only needs the storage/
power/time HAL, already up); `kf_pet_session_frame(0)` runs once per frame
inside the existing `while (kf_app_frame())` loop, `0` meaning "track real
elapsed time yourself," the same convention `sdl_main.cpp` relies on when
it has no debug time multiplier to fold in -- which this build never does.

### 4. Visibility is a serial log line, not a screen

With no LVGL on this build, the only way to see the pet is
`KF_LOGI`, printed every 10 seconds: stage, hunger/happiness/energy as
percentages, and `base_trait`. Ten seconds is frequent enough to watch the
fastest default decay rate (hunger, 1042 mp/hour) move by a few tenths of
a percent between lines without flooding the monitor. This is deliberately
not a permanent interface -- it goes away the moment LVGL lands and a real
pet screen (ADR 0017) is reachable.

## What this slice does NOT reach

- **Offline ageing still does not work on real hardware.** `kf_time_wall()`
  has no DS3231 behind it yet (ADR 0020's still-open gap, unchanged by this
  slice) -- `kf_pet_session_init()`'s fast-forward call runs, but with
  nothing valid to fast-forward across on a genuine power-off, the same
  "fresh device" fallback path the desktop tests already cover.
- **No screen, no buttons wired to care actions.** `kf_pet_session_feed()`/
  `_play()`/`_rest()` exist and work, but nothing on this build calls them
  yet -- that needs either LVGL's pet screen or a direct button-to-action
  wiring in `app_main.cpp` itself, neither of which this slice adds.
- **No LVGL, no Lua.** Unchanged from ADR 0020; see "Context" above for why
  neither blocks the other or the pet session itself.

## Verified

- `kamiframe-headless` and `kamiframe-sim` both rebuild clean from a full
  build directory after this change, and all 13 desktop/headless tests
  pass unchanged, including every pet-related one
  (`pet_offline_ageing_check`, `pet_stage_evolution_check`,
  `pet_personality_check`, `lua_pet_binding_check`, `pet_screen_check`,
  `screen_nav_check`) -- confirming the `KF_PET_SESSION_ENABLE_DEBUG_TOOLS`
  split changes nothing about desktop/headless behaviour, since that flag
  defaults to 1 there and none of their build files were touched.
- `kf_pet_session.cpp` compiles standalone and cleanly under
  `-Wall -Wextra -Werror` (the same warning level ADR 0024 confirmed the
  ESP-IDF toolchain itself uses) at **both**
  `KF_PET_SESSION_ENABLE_DEBUG_TOOLS=0` and `=1`, against a host compiler --
  this confirms the `#if` split itself is sound (no unused-variable/
  function warnings, no reference from the disabled branch into removed
  state) independent of ESP-IDF.
- **`idf.py build` now genuinely succeeded, on Chris's own machine, 2026-08-06.**
  A fresh install of ESP-IDF v6.0.2 (xtensa-esp-elf 15.2.0 toolchain, Ubuntu
  under WSL) built this slice clean for the esp32s3 target: bootloader
  compiled, a partition table generated, and `kamiframe-firmware.bin`
  produced at `0x41a20` bytes (~266KB) against a 1MB app partition -- 74%
  still free. This is the real toolchain run this ADR's first version named
  as the missing piece.
- **One real bug surfaced on the first attempt, exactly the category the
  first version of this ADR predicted:** `app_main.cpp`'s new
  `log_pet_state()` referenced `pet->hunger`/`->happiness`/`->energy`
  instead of the real field names `kf/pet.h` actually declares --
  `hunger_mp`/`happiness_mp`/`energy_mp`. A plain naming mistake, not an
  architecture problem, and the host-compiler check above never had a
  chance to catch it -- it only ever compiled `kf_pet_session.cpp`, not
  this new logging code, which has nothing outside ESP-IDF to compile
  against. The real compiler caught it immediately and unambiguously on
  the first build attempt. Fixed to use the correct field names; no other
  file affected.

## Not yet done

Building is not running. `kamiframe-firmware.bin` has not been flashed to
or executed on real hardware -- parts arrive 2026-08-07, and hardware
bring-up (ADR 0024) runs first, on the separate `ports/esp32-bringup`
project, to confirm the wiring before this firmware ever touches the
board. The first real on-device run of this slice -- watching the pet
actually tick over serial -- happens once that passes.

## Cost to change

If hardware bring-up (ADR 0024) turns up a pin move, a display-interface
change, or anything else affecting `kf_esp_pins.h` or `esp_display.cpp`:
zero cost to this slice. The pet session neither reads nor writes a single
pin, and `KF_DEMO_SPRITE` was already the display proof before this slice
existed. If the DS3231 driver lands: `kf_pet_session_init()` needs no
change at all -- it already calls `kf_pet_load_and_advance()`, which
already reads whatever `kf_time_wall()` reports; the fast-forward starts
working the moment the HAL layer beneath it does.
