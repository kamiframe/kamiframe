# ADR 0039: The panel profile owns the read line, and something turns the backlight on

**Status:** Accepted
**Date:** 2026-08-11

## Context

Two independent defects sat in `esp_display.cpp` and `kf_esp_pins.h`, and both
were invisible for the same reason: the only panel ever tested is the HiLetgo
ILI9341, whose LED pin happens to be soldered straight to the board's 3V3
rail. Nothing this codebase does through software has ever controlled that
module's backlight, so both bugs below produced the correct visual result on
the only hardware anyone has actually looked at.

**1. `KF_ESP_PIN_LCD_BL` and `KF_ESP_PIN_LCD_MISO` were both `GPIO_NUM_6`,
deliberately, and the collision was resolved on the wrong variable.**
`esp_display.cpp:320-324` (before this change) reserved GPIO6 as SPI MISO
whenever `KF_DBG_BRIDGE_ENABLE` was on, and `:376-385` skipped configuring
GPIO6 as the backlight GPIO under the identical condition, in the name of
avoiding electrical contention between the SPI peripheral's input matrix and
a push-pull GPIO output. Both halves of that reasoning were correct *about
the wrong question*. "Is the debug bridge compiled in" is a policy decision
about how much of KFDBG a build ships; it says nothing about what GPIO6 is
physically wired to. The real question is "does the panel screwed to this
board have a data-out line at all" — and the two panels this project supports
disagree. The HiLetgo ILI9341 has a real SDO pin, wired to GPIO6 for the
KFDBG SCANLINE diagnostic (ADR 0024). The Waveshare 2in ST7789's eight-pin
header is `VCC GND DIN CLK CS DC RST BL` — there is no ninth pin for SDO, and
no separate tearing-effect pin either. Reserving GPIO6 as MISO for that
module buys nothing (there is no signal to read) and costs two things: the
backlight stays unconfigured, and — per `esp_display.cpp`'s own analysis of
ESP-IDF's `bus_uses_iomux_pins()` — a non-native MISO drops the *whole* SPI
bus (MOSI, SCLK, CS too, not just MISO) off the IOMUX fast path onto the
slower GPIO matrix. The measured 40MHz write ceiling (ADR 0024) was measured
on the IOMUX path. Tying the MISO decision to the panel profile rather than
the build flag hands the ST7789 back a fast path it should never have been at
risk of losing.

**2. `kf_display_set_backlight()` had no caller anywhere in the repository.**
Confirmed by grepping the whole tree before this change: the function was
declared in `hakoniwaos/include/kf/hal/display.h`, defined in all three HAL
backends (SDL, headless, ESP32), and invoked by nothing — not core, not
`app_main.cpp`, not any KFDBG handler. Even on the `!KF_DBG_BRIDGE_ENABLE`
path the backlight GPIO was configured and immediately driven low, and left
there forever. On the ILI9341 this is invisible, because that module's LED
pin never depended on it. The Waveshare ST7789's BL pin is real. Left as-is,
a build against that profile would boot, initialise the panel correctly over
SPI, log a clean bring-up sequence, and show a black screen — the worst
failure shape there is, because every instinct sends a debugger into the
render path instead of the one GPIO nobody ever drives.

A third, smaller trap surfaced while fixing the first two: with the MISO
reservation now conditional, `KFDBG SCANLINE` on a profile with no read line
has nothing to read. Before this change that situation could not arise on a
real device (the ILI9341 always had the pin), but a `KF_PANEL=st7789` build
now can reach it, and a floating GPIO read back through
`esp_lcd_panel_io_rx_param()` returns *something* — plausible-looking, wrong
numbers, not an obvious failure.

## Decision

### `has_read_line`, a new field on `kf_panel_profile`

`kf_panel_profile.h` gains `bool has_read_line`, documented with each panel's
actual header pinout so the next person does not have to find the module's
datasheet to know why the field is what it is. `true` for the ILI9341
(HiLetgo 2.8in, real SDO). `false` for the ST7789 (Waveshare 2in, no SDO pin
on the module at all). Both profile initialisers were updated in the same
edit — they are aggregate initialisers, and updating only one would leave the
other silently zero-initialised, which happens to be correct for the ST7789
and would quietly break the one configuration that has ever worked for the
ILI9341.

### `esp_display.cpp`'s MISO and backlight decisions both move onto this field

```
reserve_miso_for_read_line = KF_DBG_BRIDGE_ENABLE && kPanel.has_read_line
own_backlight_pin          = !reserve_miso_for_read_line
```

Both are plain runtime `bool`s, computed with a plain `if`, not `#if`/`#else`
— the same shape ADR 0035 chose for the KFDBG mutate gate, and for the same
reason: `KF_DBG_BRIDGE_ENABLE` is a compile-time 0/1 and `kPanel` resolves at
compile time too, so the compiler folds this exactly as a `#if` would, but
the code on both sides of the condition is real, type-checked C++ in every
build configuration rather than two textually-divergent branches that can
silently drift apart.

`g_caps.has_backlight` (the HAL capability struct `kf_display_get_caps()`
returns) is now set from `own_backlight_pin` at the end of `kf_display_init()`
rather than hardcoded `true` — so a caller asking "can I control the
backlight" gets an honest answer for the one configuration (ILI9341 +
`KF_DBG_BRIDGE_ENABLE=1`) where this build does not own the pin.

### `kf_display_set_backlight(255)` is called, once, at the end of `kf_display_init()`

Placed after `rebuild_panel_io()` has run the init table and after the
byte-swap strip buffers are allocated — the panel is in a known, configured
state (even though no frame has been presented to it yet), rather than
whatever it powered on with. Gated on `own_backlight_pin`: on the ILI9341 +
bridge-enabled combination the call is skipped, matching this profile's
existing behaviour exactly (the pin was never this build's to drive, and the
module's LED is hardwired to 3V3 regardless). On every other combination —
which as of this ADR includes the ST7789 unconditionally, on every
`KF_DBG_BRIDGE_ENABLE` setting — this is the first and only place in the tree
that turns a backlight on.

### `SCANLINE` and `VSYNC` refuse, with a comprehensible reply, on a profile with no read line

`esp_display.cpp` gains two small accessors,
`kf_esp_display_has_read_line()` and `kf_esp_display_panel_name()`, mirroring
the file-scope `kPanel` out to `kf_dbg_bridge.cpp` (which has no other way to
reach it — `kPanel` is deliberately not part of any header). `kf_dbg_bridge.cpp`
gains `require_read_line(label)`, the same shape as ADR 0035's
`require_mutate_enabled()`: check first, reply `err` naming the panel and
return `false` before doing any real work. `handle_scanline()` and
`handle_vsync()` both call it as their first line. `tools/kf_debug.py`'s
`_expect()` already turns any `err` reply into a `KfDebugError` carrying the
device's text verbatim (ADR 0035), so no host-side protocol change was
needed — only a docstring update recording the new refusal reason alongside
the mutate-gate one it already documented.

`VSYNC` refusing is a smaller case than `SCANLINE`'s: enabling the wait on a
panel with no read line is not *dangerous* (`kf_vsync_read_scan_row()`
already fails closed and writes immediately when a read fails, since MISO
was never reserved on that profile either), but it is silently pointless —
every call pays for a doomed SPI read and gets exactly the behaviour of
leaving the wait off. Refusing tells a human that up front instead of
letting them believe the toggle did something.

### `KF_PANEL`, a CMake cache variable

`ports/esp32/main/CMakeLists.txt` gains `KF_PANEL`, accepting `ili9341`
(default) or `st7789`, validated at configure time with `message(FATAL_ERROR
...)` on anything else, and translated into
`target_compile_definitions(${COMPONENT_LIB} PRIVATE
KF_PANEL_PROFILE=kf_panel_${KF_PANEL})`. A typo that reached the compiler
instead would fail deep inside `kf_panel_profile.h` with "use of undeclared
identifier kf_panel_st7889" or similar — a message that reads like a code
bug, not a command-line mistake. Catching it at configure time turns that
into one clear, actionable error.

```
idf.py -DKF_PANEL=st7789 build
```

Documented in the CMake file's own comment, `ports/esp32/CMakeLists.txt`'s
top-of-file usage block, and `ports/esp32/README.md`.

## Stale comments fixed

This codebase treats a comment that contradicts its code as a defect. Five
were touched by this task:

- `kf_esp_pins.h`'s `KF_ESP_PIN_LCD_MISO` comment described the GPIO6/
  backlight collision as resolved by `KF_DBG_BRIDGE_ENABLE` alone, and
  claimed "the day real GPIO or PWM backlight control gets wired up for
  real, one of these two names has to move" — which this task makes false
  the moment it lands, since real backlight control now exists for the
  ST7789 on this exact pin. Rewritten to describe the profile-based
  resolution and which panel lands on which side of it.
- `kf_esp_pins.h`'s display-block header comment had the same
  `KF_DBG_BRIDGE_ENABLE`-only framing; rewritten alongside the MISO comment.
- `kf_esp_display_vsync.h:54-56` said "Default true" for
  `g_vsync_enabled`. `esp_display.cpp:512` has read `false` since ADR 0032
  (the wait was measured on real hardware and made no difference to flicker
  on changing content). Rewritten to say `false` and cite ADR 0032 directly,
  rather than pointing at a sibling comment that itself no longer said what
  this one claimed it said.
- `kf_esp_display_vsync.h`'s "ON KF_DBG_BRIDGE_ENABLE=0" section described
  the MISO reservation as gated on the bridge flag alone; rewritten to name
  the panel profile as the second, now-primary factor.
- `ports/esp32/CMakeLists.txt:4-8` said "Builds a plain hello-world (ADR
  0019) ... The ESP32 HAL backends kf_app_frame() needs still do not exist."
  They have existed since Phase 1b; this project builds the whole firmware.
  Rewritten, and grown to mention the new `KF_PANEL` build-time choice.

`esp_display.cpp`'s `KF_ESP_DISPLAY_DIAG_CMD_GET_SCANLINE` comment in
`kf_esp_display_diag.h` was also updated, though it was not one of the five
named defects: it previously said to "treat a SCANLINE run against
`kf_panel_st7789` as doubly unverified," which this task makes stronger than
true — that profile can no longer run a SCANLINE at all, by construction.

## What this does NOT reach

- **`hakoniwaos/` is untouched.** This entire decision lives in the ESP32
  port (`ports/esp32/hal/`, `ports/esp32/main/`) and its own CMake files.
  Core's HAL contract (`kf/hal/display.h`) is unchanged in shape; only the
  ESP32 backend's own `g_caps.has_backlight` value became honest.
- **The desktop and headless backends are untouched.** Neither has a real
  SPI bus or a real backlight GPIO; `has_read_line` and the MISO/backlight
  split are ESP32-specific concerns that do not generalise to a backend with
  no physical pins.
- **`tools/kf_panel.py` is untouched** — the same boundary ADR 0031/0034/0035
  each drew for their own new commands.
- **No new KFDBG wire command.** `SCANLINE` and `VSYNC` exist already; this
  task only adds a new reason either can reply `err`, using the existing
  generic `err`-handling path on the host side.

## Verified

- **Desktop build.** `cmake --build build -j8` clean (no desktop source file
  was touched; the build reused every cached object outside `ports/esp32/`).
  `ctest --test-dir build`: **38/38 passed**, unchanged from baseline.
  `python3 tools/check_no_heap.py .`: **core is heap-free**, unchanged.
  `python3 tools/kf_debug_selftest.py`: all checks pass.
- **`idf.py build` clean for esp32s3 against the real ESP-IDF v6.0.2 install,
  zero warnings, in BOTH panel configurations, each a full
  `-DKF_PANEL=<value> build`:**
  1. `KF_PANEL=ili9341` (default). `kamiframe-firmware.bin`: **661,104
     bytes** (`0xa1670`), 58% of the app partition free.
  2. `KF_PANEL=st7789`. `kamiframe-firmware.bin`: **660,944 bytes**
     (`0xa15d0`), 58% of the app partition free. Marginally smaller than the
     ILI9341 build — expected, since that profile's `has_read_line == false`
     removes the SCANLINE/VSYNC MISO-reservation branch from ever being
     taken and the ST7789's init table is shorter than the ILI9341's.
  3. **`KF_PANEL` validation confirmed**: `-DKF_PANEL=bogus` fails
     `cmake` configure with `KF_PANEL must be 'ili9341' or 'st7789', got
     'bogus'`, not a compiler error inside a header.
  - The tree was left configured for `-DKF_PANEL=st7789` after both builds,
    per the hardware bring-up plan's instruction — that is the panel Tasks
    5-8 use.
- **Reasoning, confirmed by re-reading the code rather than a fresh
  measurement:** ESP-IDF's `spicommon_bus_initialize_io()` grants the IOMUX
  fast path only when every configured SPI pin (MOSI, MISO, SCLK, CS)
  matches the peripheral's native set. `bus_config.miso_io_num` is `-1`
  unconditionally for `has_read_line == false` (the ST7789), so that
  profile's bus never leaves the IOMUX path regardless of
  `KF_DBG_BRIDGE_ENABLE` — the same path the ADR 0024 40MHz measurement was
  taken on. This is not a new clock sweep; there is no ST7789 on hand this
  session to run one against.

## Not verified

**Nothing in this task has run against real hardware.** There is no board in
this environment (`CLAUDE.md`'s "no hardware yet" rule). Everything above is
a clean cross-compile in two configurations, a desktop test suite that never
touches this code, and reasoning from ESP-IDF's own source and this
project's prior, hardware-confirmed measurements (ADR 0024, ADR 0032) — never
a KFDBG SCANLINE actually sent to a device, never an oscilloscope on GPIO6,
never a Waveshare ST7789 that has actually lit up.

Specifically:

- **The ST7789 panel profile has still never driven a physical panel.** The
  first unit was faulty (ADR 0024) and was returned; a replacement has not
  arrived. This task changes which pin the backlight uses and adds the one
  call that turns it on, but whether the ST7789's BL pin responds the way
  this reasoning expects — correct polarity, correct voltage level, no
  surprise inversion — is exactly what Task 6 (first pixels on the ST7789)
  has to find out.
- **The IOMUX-fast-path claim for the ST7789 is reasoning, not a
  measurement.** No clock sweep has been run against that profile at all,
  on any path.
- **`own_backlight_pin == false` on the ILI9341 + bridge-enabled build was
  not re-confirmed electrically this session** (no hardware). It is
  unchanged behaviour from before this task, carried forward by
  construction (`has_read_line == true` for that profile reproduces the old
  `KF_DBG_BRIDGE_ENABLE`-only condition exactly), not a fresh claim.
- **The bench workaround, if the ST7789 still comes up dark on first
  contact:** wire BL straight to 3V3 and move on. That isolates the
  backlight question from the SPI question, which is the reason this task
  exists ahead of Task 6 rather than being discovered during it.

## Cost to change

**Adding a third panel:** one more `has_read_line` value in its profile
table, and if `true`, the same "does GPIO6 collide with backlight" question
this ADR already answers generically — no new code path, since the decision
is already keyed off the field rather than a hardcoded panel name.

**Un-splitting the MISO/backlight decision back onto `KF_DBG_BRIDGE_ENABLE`
alone:** would reintroduce the exact bug this ADR fixes — a real ST7789 BL
pin silently unclaimed whenever the bridge happens to be compiled in. Not a
one-line revert with no cost.

**Adding PWM backlight dimming:** unaffected by this ADR — `kf_display_set_
backlight()`'s on/off-only behaviour is a separate, pre-existing decision
(see `esp_display.cpp`'s file header comment), and this task's caller only
ever passes `255`.
