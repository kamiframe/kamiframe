# ADR 0019: An ESP-IDF hello-world, and what it actually proves

**Status:** Accepted, 2026-08-04
**Reversal cost:** Low for `main.c` itself (a dozen lines, no dependents).
Higher for the `sdkconfig.defaults` flash-size choice and `wokwi.toml`/
`diagram.json`'s board type, since Phase 1b's real HAL work and any future
Wokwi config will build directly on top of both -- changing them later means
touching whatever by then depends on them, not just this file.

## Requirement

Chris, after the demo creature script slice: *"what would you suggest to
work on next?"*, offered four options and picked **ESP-IDF hello-world in
Wokwi**. The reasoning behind recommending it: README.md names two Phase 1
exit triggers before parts get ordered -- the demo pet running in the
simulator (true since ADR 0017) and an ESP-IDF hello-world booting in Wokwi.
`ports/esp32/` had existed since early in this project as a deliberately
unbuilt skeleton -- its own README said so in the first line -- specifically
to keep the repository layout ESP-IDF-compatible without writing firmware
code that had never been compiled. This slice is the second trigger: turn
that skeleton into something that actually compiles for a real ESP32
target, honestly scoped to what this environment can verify without
hardware or a Wokwi account.

## Decision: prove the build shape compiles; do not call into the app yet

**`main.c` is a plain ESP-IDF hello-world, not a call into
`kf_app_frame()`.** `kf/app.h`'s frame loop needs real ESP32 HAL backends --
ST7789 over SPI/DMA, GPIO input, `esp_timer` plus an RTC, `heap_caps_malloc`
pools -- and none of them exist yet; writing them was never this slice's
job (see `ports/esp32/README.md`'s own "What still has to be written," now
reframed as Phase 1b work). Calling `kf_app_init()`/`kf_app_frame()` from
`main.c` today would fail to link against those missing backends, or -- if
stubbed just to make it link -- would boot against HAL functions that had
never been checked against anything, which is worse than not calling them:
a green build log that proves nothing.

**`main`'s `REQUIRES hakoniwaos` anyway, even though `main.c` never calls
into it.** ESP-IDF compiles every source in a required component's
`sources.cmake` regardless of whether anything calls the result; the linker
only drops what stays unreferenced. That distinction is exactly what this
slice wanted: real xtensa-esp32s3 compilation of every file in
`hakoniwaos/sources.cmake` -- the literal claim `ports/esp32/README.md` and
ADR 0002 both make, "one source list, two build systems" -- actually
exercised, without needing the app to run. Proving the shared source list
cross-compiles is the architecturally risky, valuable half of "hello world
boots in Wokwi"; proving the app runs on a screen that doesn't exist yet in
this codebase is Phase 1b's job, not this one's.

**`sdkconfig.defaults` sets only what this milestone needs: the target chip
and a matching flash size.** The fuller list README.md already named --
octal PSRAM, 240MHz CPU -- is real bring-up configuration for hardware that
does not yet have HAL code to run on it. Setting it now, unused and
untested, would be config drifting ahead of the code that depends on it,
the same reasoning ADR 0018 applied to not adding autosave logic nowhere
needed it yet.

**`diagram.json` wires nothing but the serial console, deliberately
matching the official `wokwi/esp-idf-hello-world` reference project.** No
display component is defined. See "A correction, found doing this" below
for why that is not just conservatism.

## A correction, found doing this

`ports/esp32/README.md` described Wokwi's ESP32-S3 simulation as including
"an ST7789, no parts required." Checking Wokwi's own supported-hardware
list while building `diagram.json` found that this was never true: Wokwi
does not support the ST7789 at all. The closest available part is
`wokwi-ili9341`, a different (if visually similar) 240x320 SPI controller.
This hello-world sidesteps the question rather than quietly substituting
one panel for another -- `diagram.json` wires only the serial console, the
same scope the official ESP-IDF Wokwi example ships with. Whoever picks up
real display work on Wokwi will need to decide then whether an ILI9341
stand-in is good enough for early bring-up or whether Wokwi simply isn't
where display work happens. That decision does not need making today, but
the wrong assumption it corrects was live in this repository's docs until
now, so it is corrected here rather than silently.

## What this slice actually builds

- `ports/esp32/main/main.c`: prints chip model, core count, flash size and
  free heap once, then a one-per-second "alive" line forever. No dependency
  on anything in `hakoniwaos` beyond making the linker's job trivial (it
  drops the whole unused archive).
- `ports/esp32/main/CMakeLists.txt`: registers `main.c`, `REQUIRES
  hakoniwaos spi_flash` (the latter for `esp_flash_get_size()`).
- `ports/esp32/CMakeLists.txt`: unchanged in shape, updated only to say the
  build now works and to add the `idf.py wokwi` line to its own usage
  comment.
- `ports/esp32/sdkconfig.defaults`: new. Target `esp32s3`, 16MB flash
  (matches Wokwi's `board-esp32-s3-devkitc-1`).
- `ports/esp32/wokwi.toml` and `diagram.json`: new. Point at the real build
  output path this slice's own build confirmed (`build/kamiframe-
  firmware.elf`/`.bin`, from this port's `project(kamiframe-firmware)`
  call), and wire the board's UART0 to Wokwi's serial monitor only.

## Two real bugs, found only by cross-compiling

**A latent, architecture-dependent format-string bug in `hakoniwaos/src/
app.cpp`.** Several `KF_LOGI(...)` calls passed `uint32_t` values (frame
timing figures: `draw_us`, `transfer_us`, `mean_us`, and others in
`kf_app_log_budget_report()`) against a bare `%u` format specifier. On the
desktop target (x86-64 Linux), `uint32_t` is `unsigned int`, so `%u` is
exactly right and GCC has never once complained across eighteen prior ADRs'
worth of desktop builds. On xtensa-esp32s3, `uint32_t` resolves to `long
unsigned int` -- a real type-width mismatch, not just the signedness `%u`
vs `%d` mismatches the desktop build already tolerates via
`-Wno-sign-compare` -- and `-Werror` (on for this build, matching every
other target in this repo) turned it into a hard failure the moment
`app.cpp` was cross-compiled for the first time. Fixed by including
`<cinttypes>` and switching every affected specifier to `PRIu32`, which
resolves to the correct specifier on both targets rather than assuming one.
This is exactly the class of bug a single-platform build can carry
indefinitely without ever showing a symptom -- found here specifically
because ADR 0002's "two build systems, one source tree" decision means
`app.cpp` had to actually compile for a second, different-width target for
the first time. Verified fixed by both a clean ESP-IDF rebuild (below) and
re-running the full desktop `ctest` suite afterward -- all 10 targets still
pass, and `kamiframe-sim` still links clean, confirming the fix changed
only which format specifier gets used, not what gets logged.

**A missing dependency in my own first draft of `main.c`.** `esp_flash.h`
(for `esp_flash_get_size()`) lives in ESP-IDF's `spi_flash` component,
which `main`'s `CMakeLists.txt` did not `REQUIRES`. Fixed by adding it.
Caught immediately by the same build, not worth a full section of its own,
but named because it is the same lesson as the bug above stated smaller:
untested code carries mistakes that only a real build surfaces.

## Two more real bugs, found only by Chris's actual Wokwi run

**`diagram.json`'s serial wiring used the wrong pin names.** `esp:TX0` /
`esp:RX0` -- the original classic-ESP32 UART0 naming this file's first
draft copied by habit -- do not exist on `board-esp32-s3-devkitc-1`.
`wokwi-cli .`'s own diagnostic output named the problem exactly (`Invalid
pin "TX0" for part "esp"... Valid pins: ... RX, TX`), which is the entire
reason this only surfaced once a real Wokwi run happened: nothing in the
ESP-IDF build validates a Wokwi diagram, because the build has no idea
Wokwi exists. Fixed by wiring `esp:TX`/`esp:RX` instead, confirmed by
re-running with Chris's own token.

**The flash size Wokwi reports at runtime does not match what
`sdkconfig.defaults` requested.** `main.c`'s own `esp_flash_get_size()`
call printed `flash=4MB` in the real boot log, not the 16MB
`CONFIG_ESPTOOLPY_FLASHSIZE` asks for. Not a bug to fix: `esp_flash_get_
size()` queries the actual (simulated) flash chip's own reported size,
independent of what size the image was packaged for, and the boot
succeeded regardless -- this is a fact about what `board-esp32-s3-
devkitc-1` simulates, not a build misconfiguration. Left as-is, but worth
recording plainly rather than leaving the "flash=4MB" line in a boot log
unexplained: whoever debugs a future Wokwi run and sees that number should
not go looking for a bug that isn't there.

## Verified

- `idf.py set-target esp32s3` then a full clean `idf.py build` (`idf.py
  fullclean` first, so nothing was reused from the build that hit the two
  bugs above): 1071/1071 build steps, zero warnings, zero errors, against
  ESP-IDF v6.0.2's own `-Wall -Wextra -Werror` flags for this target.
  Produces a real `build/kamiframe-firmware.elf` and `.bin`
  (0x28290 bytes, 84% of the smallest app partition free).
- The component list ESP-IDF's own CMake configure step printed includes
  `hakoniwaos` by name, confirmed compiled (`Building CXX object esp-idf/
  hakoniwaos/CMakeFiles/__idf_hakoniwaos.dir/src/{arena,blit,framebuffer,
  font,rng,pet,app,demo}.cpp.obj`, all eight files from `sources.cmake`,
  the exact list the desktop build also compiles) -- this is the concrete
  evidence behind this ADR's "prove the build shape compiles" framing, not
  an assumption from reading the CMake files.
- The desktop build, rebuilt clean afterward with
  `-DKAMIFRAME_WARNINGS_AS_ERRORS=ON`: clean, and all 10 `ctest` targets
  pass, confirming the `app.cpp` format-string fix changed nothing the
  desktop build's own checks would catch.
- `tools/check_no_heap.py`: clean, unmodified.
- **The actual Phase 1 trigger: a real Wokwi boot, run by Chris on his own
  machine with his own `WOKWI_CLI_TOKEN`** (this environment cannot create
  or hold one -- see below). `wokwi-cli .` against `board-esp32-s3-
  devkitc-1` booted the exact `.elf`/`.bin` this ADR's build produced,
  through real ROM bootloader output, into `app_main()`, printing every
  line `main.c` writes -- chip model, core count, free heap, the
  hakoniwaos-linked line, and the once-a-second heartbeat -- before the
  CLI's default 30-second capture window ended it. That timeout is
  expected, not a failure: `main.c`'s `while(1)` never returns by design
  (see "Decision" above), so there is no "simulation finished" event for
  the CLI to detect; a firmware that runs forever getting cut off after a
  fixed capture window is exactly what should happen. Two more real bugs
  turned up doing this -- see the section above -- both fixed and the
  second run (after the `diagram.json` pin fix) is the one that produced
  the clean boot log this bullet describes.

**Explicitly NOT verified here, and why:**

- **Does not run on real hardware.** No device, no `idf.py flash`, no
  serial monitor output from an actual chip. Wokwi is a real simulation of
  real ROM/bootloader/IDF startup code, not a hardware substitute --
  actual bring-up (and the `KF_DISPLAY_SPI_HZ` measurement below) still
  needs a physical board.
- **The ESP-IDF toolchain install itself needed a workaround to complete in
  this sandbox.** `./install.sh esp32s3`'s Python dependency step downloads
  a "constraints file" from `dl.espressif.com`, which this sandbox's
  network allowlist returns 403 Forbidden for (confirmed via the install
  log's own `Tunnel connection failed: 403 Forbidden` line) -- `github.com`
  itself, including the ESP-IDF clone and toolchain binary downloads from
  GitHub releases, worked throughout. Completed the install instead with
  `IDF_PYTHON_CHECK_CONSTRAINTS=no` (`tools/idf_tools.py`'s own documented
  escape hatch, not a hack), which installs the same package *versions*
  ESP-IDF v6.0.2 ships in its Python requirements files, just without
  double-checking them against the separately-hosted constraints file. This
  matters only to reproducing the build in an equally locked-down sandbox
  again -- it says nothing about whether the firmware itself is correct,
  which the build and its cleanliness above already speak to directly.

## Later

- Phase 1b, now that both this trigger and the simulator one are satisfied:
  real HAL backends under `ports/esp32/hal/`, a `main.c` that spawns the
  FreeRTOS task calling `kf_app_frame()`, a real partition table, and the
  rest of `sdkconfig.defaults` (octal PSRAM, 240MHz CPU).
- Resolving the ST7789-vs-Wokwi gap for real, once display work on Wokwi is
  in scope: either accept `wokwi-ili9341` as a bring-up stand-in and say so
  loudly in whatever ADR adds it, or treat Wokwi as serial-only forever and
  do display bring-up exclusively on real hardware.
- Measuring `KF_DISPLAY_SPI_HZ`'s real value (`kf/budget.h`) on day one of
  actual hardware bring-up -- unchanged from what `ports/esp32/README.md`
  already said, repeated here because this slice didn't touch it and
  shouldn't be read as having done so.
