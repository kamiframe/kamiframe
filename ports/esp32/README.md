# ports/esp32

**Real ESP32 HAL backends -- display, input, time (DS3231-backed, see
ADR 0026), memory, entropy, logging, storage, power -- all compiling and
linking clean against the real ESP-IDF v6.0.2 toolchain, driving the real
`kf_app_init()`/`kf_app_frame()` loop from `kf/app.h`, with a real,
NVS-backed `kf_pet_session` ticking alongside it (ADR 0025). LVGL and Lua
are both up (ADR 0027, ADR 0028), and a real hardware session on 2026-08-08
put the pet screen on real glass for the first time -- see "What ADR 0027
added" below for what that found and fixed, and `main/app_main.cpp`'s own
header comment for exactly what is and is not confirmed as a result. Lua is
present and not crashing through that same session; whether the demo
creature is actually driving what the screen shows is a separate, still-open
question -- same place has the details.** See ADR 0019 for the hello-world
this was built on top of, ADR 0020 for the HAL backends, ADR 0025 for the
pet session, and ADR 0026 for the DS3231 real-time clock driver.

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

## What ADR 0027 added

- `kamiframe_lvgl_port`, a real ESP-IDF component wrapping the exact same
  portable glue desktop already builds (`kf_lvgl_pool.cpp`,
  `kf_lvgl_display.cpp`, `kf_pet_screen.cpp`, `kf_screen_nav.cpp`, and the
  rest) by relative path -- one canonical file, not a fork. `app_main.cpp`
  now drives `kf_lvgl_port_init()`/`kf_screen_nav_init()` at boot and pet
  session -> screen nav -> LVGL pump every frame, and `KF_DEMO_SPRITE` is
  gone (`KF_DEMO_NONE`) now that LVGL owns the framebuffer.
- Three real build failures were found and fixed, not guessed at: LVGL's own
  build system tries to self-register as an ESP-IDF component when fetched
  from inside one that's already registering; ESP-IDF's early
  "requirements scan" pass runs component `CMakeLists.txt` code in a mode
  that breaks `FetchContent`; two ARM-only LVGL `.S` files aren't
  assembly-safe outside an ARM toolchain and were never compiled on desktop
  at all.
- **Confirmed on real hardware, 2026-08-08.** The pet screen is on real
  glass. First flash was not clean: a DMA race in the display driver's own
  byte-swap path duplicated bands and dropped the top of the frame, unsent
  dirty rectangles kept re-transmitting unchanged frames, and the D-pad
  could not move focus off Feed. All three were found by watching the real
  board, not by reasoning about the code, and all three are fixed -- see
  "What ADR 0032 added" below for what came after.

## What ADR 0028 added

- A real ESP-IDF component for Lua itself (`ports/esp32/components/lua`),
  compiling `cmake/fetch_lua.cmake`'s own source list into a real
  `liblua.a` rather than linking the desktop build's `lua_core` target --
  the latter built clean but failed to LINK (`__wrap_longjmp` never pulled
  in, because a plain CMake target is invisible to the machinery that
  orders an ESP-IDF link line).
- `-DLUA_32BITS`: this toolchain's C++ translation units never see
  `LLONG_MAX`, so `lua_Integer`/`lua_Number` are 32-bit on ESP32 (desktop
  keeps 64-bit `lua_Integer`). Every value `pet.*`/`kf.*` pass today fits
  comfortably; a future 64-bit value would silently truncate instead of
  erroring.
- `app_main.cpp` calls `kf_lua_port_init()`/`_frame()`/`_shutdown()` in the
  same order, for the same reasons, `sdl_main.cpp` already does, loading the
  real demo creature script (ADR 0018) unforked.
- **Present and not crashing on real hardware**, confirmed by the same
  2026-08-08 session: this firmware, Lua linked in, stayed up through the
  LVGL and KFDBG work that followed. **Not confirmed:** whether the demo
  creature's own `pet.feed()`/`play()`/`rest()` calls actually reach and
  move the live `kf_pet_state` the pet screen reads. Nobody has
  independently watched that link happen yet.

## What ADR 0029 added

- `kf_panel_profile.h`: everything that differs between panel modules
  (init sequence, byte order, inversion, window gap) becomes data, not a
  hardcoded ST7789 assumption. `esp_display.cpp` now knows how to drive a
  240x320 SPI panel and not which one.
- Byte order earned its own field because it's the one quirk `esp_lcd`
  doesn't hide: RGB565 goes on the wire high-byte-first, `esp_lcd`
  byte-swaps commands but not colour data, and the ST7789 masks this with a
  register the ILI9341 has no equivalent of. Confirmed on hardware with
  full-screen fills: red showed blue, green showed pink, blue showed green
  -- a plain byte swap and nothing else.
- Defaults to the ILI9341 (the bring-up panel that actually works), not the
  ST7789 (the intended primary, which arrived faulty) -- deliberately: a
  default that produces a black screen on the only working board is a bad
  default however defensible on paper.
- **Confirmed by the same 2026-08-08 hardware session as ADR 0027/0028** --
  this is the driver that put the pet screen on glass.

## What ADR 0030 added

- The device half of the KFDBG serial debug bridge
  (`kf_dbg_bridge.{h,cpp}`, `kf_dbg_codec.{h,cpp}`): a low-priority FreeRTOS
  task pair reads and writes the console UART, so `tools/kf_debug.py` can
  pull a screenshot, read live pet state, or press buttons remotely, over
  the exact same UART `idf.py monitor` already uses.
- Command parsing runs on the main frame-loop thread, because
  `kf_fb_pixels()`/`kf_pet_session_state()` aren't safe to read from a
  second thread while the frame loop writes them; only the slow
  encode-and-transmit half runs on the background tasks.
- The codec (RLE, base64, CRC32) has zero ESP-IDF dependency and was
  checked against known-answer vectors and the real host-side
  `tools/kf_debug.py` before a byte of it ran near the firmware.
- **Confirmed on real hardware the same day it landed** -- this is the tool
  the rest of the 2026-08-08 session used to drive and observe the board
  (screenshots, button presses, state reads), and it is how the LVGL
  focus bug and the panel's scanline behaviour (below) were found.

## What ADR 0031 added

- Three new KFDBG commands -- `ADVANCE <seconds>`, `RESET`, `MULT <n>` --
  giving the device the same time control the desktop debug window has had
  all along, because the shipped decay rates (four real days for hunger to
  empty) make watching a real board impractical otherwise.
- Split `KF_PET_SESSION_ENABLE_DEBUG_TOOLS` into two flags: the cheap trio
  (`_advance`/`_reset`/`_age_seconds`, no extra static memory) is on for
  ESP32; the 200KB+ scrubbable-timeline ring (`_seek()`) stays
  desktop/headless only.
- `MULT` scales only the pet session's own per-frame delta, never LVGL's
  tick or Lua's frame delta -- mirroring `sdl_main.cpp` exactly, so
  animation playback and script frame timing don't silently speed up with
  pet time.
- **Exercised on real hardware** as part of `tools/kf_panel.py`, the desktop
  control panel built the same day -- driving it for real is what found the
  D-pad focus bug and the fast-click debounce bug fixed alongside it.

## What ADR 0032 added

- No driver decision changed here; this documents what was tried against
  real tearing and what it means for panel selection. Once the pet was
  rendering on real hardware, three flicker faults were found and fixed
  (the DMA race, the dirty-rectangle fix, and an idempotent-widget-update
  fix). What's left is genuine tearing: the panel scans its own memory out
  to the glass on its own clock, uncoordinated with our writes.
- The ILI9341 module in use has no TE (tearing-effect) pin -- confirmed
  against the manufacturer's schematic. `KFDBG SCANLINE`, a new command,
  polls the panel's `Get_scanline` register instead: measured on hardware
  at three clock speeds, a real monotonic, wrapping counter, ~96Hz refresh,
  and a reply framing with no dummy byte (contrary to the datasheet's own
  reading).
- A scanline-wait was built and tried on hardware. It made no measurable
  difference to the flicker and is now defaulted off -- the code stays, for
  a future panel that answers reads more cleanly, or exposes TE outright.
- **Decision for the real board:** prefer a panel that can synchronise with
  the host -- RGB parallel first, SPI with TE second, SPI with SDO (this
  module's tier) third. Panels with none of these still work; they simply
  tear on fast-moving content.

## What ADR 0033 added

- A real partition table (`partitions.csv`), replacing ESP-IDF's default
  single-app layout: NVS (24KB), two 1.5MB OTA app slots (`ota_0`/`ota_1`,
  wired to no OTA client code yet -- see the ADR's "Why two app slots"),
  and a 12MB `assets` data partition. `KF_FLASH_ASSET_BUDGET_BYTES` in
  `kf/budget.h` was raised from 10MB to 12MB to match it exactly.
- The asset pipeline itself: `tools/kf_pack_assets.py` packs named sprites
  (and, later, sound effects -- the directory format already carries an
  asset-type tag for that) into a `.kfpack` file; `esp_assets.cpp`
  (new, `../hal/`) mounts the `assets` partition with
  `esp_partition_mmap()` so sprite pixels are read straight out of flash,
  never copied into PSRAM; `hakoniwaos/src/assets.cpp` (new, shared with
  desktop) parses the pack and serves `kf_assets_get(name)`. `main`'s
  `REQUIRES` gained `esp_partition` for the mmap call.
- The old baked-in test sprite (`examples/hello_sprite/sprite_data.h`,
  generated by the now-deleted `tools/make_test_sprite.py`) is gone.
  `kf/demo.cpp` now loads the identical sprite -- pixel-for-pixel, verified
  -- through `kf_assets_get("test_sprite")` instead, on both backends.
- **`idf.py build` succeeded**, from a freshly regenerated `sdkconfig` (so
  the new partition-table config in `sdkconfig.defaults` is what actually
  produced it), zero warnings. `kamiframe-firmware.bin` is 653,808 bytes
  against the 1.5MB `ota_0` partition -- 58.4% free, real headroom over
  the old table's single 1MB partition, where the same binary would
  already have used 62% of the whole thing.
  `esptool_py_flash_to_partition()` correctly wires
  `examples/hello_sprite/assets.kfpack` into the `assets` partition
  (offset `0x320000`) as part of `idf.py flash`. Not run against real
  flash -- see the ADR's "Not verified" for exactly what that leaves
  unchecked, principally whether `esp_partition_mmap()` actually returns
  readable sprite pixels on real silicon.
- See `docs/architecture/adr-0033-asset-pipeline.md` for the full pack
  format, the partition table's reasoning (including the OTA question),
  and why bulk pixel data deliberately never touches `KF_ARENA_ASSETS`.

## What still has to be written

- **Confirming the demo creature is actually driving the pet screen**, not
  just present and not crashing. This is ADR 0028's own named gap: nobody
  has independently watched `pet.feed()`/`play()`/`rest()`, called from
  Lua, reach the live `kf_pet_state` the pet screen already reads on real
  hardware.
- **`esp_partition_mmap()` returning readable sprite pixels off real
  flash.** ADR 0033's asset pipeline is build-verified only -- nothing has
  read a mapped byte on a real ESP32-S3 yet. This is the single biggest
  open question left in this port; see `main/app_main.cpp`'s own header
  comment.
- A caller for `kf_time_set_wall()` in production -- ADR 0026 implemented
  the write-through correctly, but nothing (no settings screen, no NTP
  sync) actually calls it yet, so a DS3231 that lost its seed still needs
  a human re-running the bring-up diagnostic to recover, not an in-app fix.
- A panel that can synchronise with the host, if full-screen animation ever
  matters -- ADR 0032's own conclusion. Both panels on hand today (the
  ILI9341 in use and the ST7789 intended as primary) tear on fast-moving
  content, and fixing that means a different panel, not a change to this
  driver.
- A real OTA client -- the partition table (ADR 0033) already reserves two
  app slots for it, but no code calls `esp_https_ota` or anything like it
  yet.
- Replacing the faulty ST7789 reference panel and re-measuring against it.
  Everything panel-specific confirmed so far -- byte order, SPI clock,
  tearing behaviour -- was measured on the ILI9341 stand-in, not the
  intended primary.

## The number checked first at bring-up, and its answer

`KF_DISPLAY_SPI_HZ` in `kf/budget.h` was an **assumption** for all of Phase 1:
40MHz, putting a full 240x320 RGB565 frame at about 30ms of wire time, with
everything the simulator says about transfer cost resting on it.

**Measured 2026-08-08: 40MHz.** The bring-up diagnostic's clock sweep drove
the panel at 4/10/20/40/80MHz in turn; 40 rendered correctly and 80 came out
solid white. The guess was right, the ~32fps full-frame ceiling is real, and
the banner is gone.

Measured on the 2.8in ILI9341 through breadboard jumpers, which makes it a
floor rather than a verdict -- re-measure when the primary 2in ST7789 arrives
and again on the real PCB. See `docs/hardware-bringup.md` for the caveats.
