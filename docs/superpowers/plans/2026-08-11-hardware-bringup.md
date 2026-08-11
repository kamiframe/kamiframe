# Hardware Bring-Up — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The pet care game runs on an ESP32-S3-DevKitC-1 N16R8 with a 2in
ST7789 240x320 panel and seven breadboard buttons, driven from Chris's Mac over
the KFDBG serial bridge — animated creature, working care loop, working debug,
a pet that ages while the power is off. Not a port: the identical
`hakoniwaos/` and `simulator/src/pet/` sources the desktop simulator already
runs, against the ESP32 HAL backend.

**Architecture:** Risk-first, not feature-first. Every task retires one named
risk and stops. The order is deliberately cheapest-and-most-informative first:
two desktop-only tasks build the instruments (a frame-budget number that isn't
structurally zero, a flash-integrity command), then Wokwi runs the real image
with no hardware, then silicon boots with the panel deliberately *not* wired,
then the panel, then the budget measurement, then the game. Three tasks change
production code; five are procedures with a code change attached.

**Tech Stack:** ESP-IDF v6.0.2 (`~/esp/esp-idf`), C++17, CMake/`idf.py`,
`esp_lcd` over SPI2 + DMA, `tools/kf_debug.py` over UART0 at 115200,
Wokwi CI simulator, CTest via `kamiframe-headless --verify-*` check modes.

## Status: PARTLY LANDED — Tasks 1, 4 and 9 are done

Written 2026-08-11 and overtaken the same night, because the owner had the
board wired and wanted to flash.

| Task | State | Commits |
|---|---|---|
| 1 — make the frame budget measurable | **done** | `73e76fd`, `7e2d466` |
| 4 — panel profile owns the backlight | **done** | `b730559`, `61fae4b` |
| 9 — the stats band (added later) | **done** | `fe06c8b`, `4fbc9fa` |
| 2, 3, 5-8 | not started | — |

**The board ran the game before most of this plan did.** Tasks 2, 3 and 5-8 were
overtaken by events: the owner flashed, hit a crash, and bring-up proceeded live.
What that produced anyway:

- `esp_partition_mmap()` **confirmed on real silicon** — the plan's single
  biggest risk, retired against the 1,156-byte pack exactly as sequenced, then
  the 556 KB creature pack.
- A null-pointer crash in `push_rect()` fixed (`1a33021`) — `g_swap_strip[]` was
  allocated only for big-endian panels but used by both paths, latent since the
  strips were introduced and unreachable until a native-endian profile existed.
- The wrong panel profile flashed first (ST7789 against a connected ILI9341),
  which is what surfaced that crash.
- Frame budget measured on device: **969µs against 33,333µs**, 34x headroom.

Task 1 matters to later work beyond this plan: the `KFDBG STATE` budget fields
it added are what `2026-08-12-lua-game-layer.md` relies on to measure the Lua
layer's cost on hardware.

Tasks 1, 4 and 9 have been executed, as the table above says; 2, 3 and 5-8
have not, though events overtook several of them anyway (see above).
Everything in "What is true today" was measured in the session that wrote
this plan, on this machine, in this worktree — not copied from an ADR — and
reflects that starting point, not the current tree.

---

## READ THIS BEFORE DISPATCHING ANY TASK FROM THIS PLAN

**This project's plan documents have manufactured five defects by being copied
verbatim.** Three were comments contradicting their own code; one was a real
`ValueError` in a listing; one was a function called four times against an
assert that fires on the second call, which cost two implementers time. See
`2026-08-10-animated-indexed-sprites.md`'s identical banner.

This plan therefore keeps code listings **minimal and load-bearing only**.
Where an exact snippet is not required for correctness, the step states the
requirement and names the file, and the implementer writes it. Where a listing
does appear, it is short enough to have been reasoned through line by line.
Two rules follow, and the second matters more:

1. When a review finds a bad comment or pattern, grep **this file** as well as
   the source tree. A defect here costs one defect *per remaining task*.
2. **Update this plan when a decision is made, before dispatching the task it
   affects.** Briefs are generated *from* this file, so a stale line here is
   re-served to every implementer that follows.

**One extra rule for this plan specifically.** Most tasks below end at a
hardware observation, and hardware has no assertion to fail — only a panel that
is blank or a log that is silent. Every task states *how you would know it
worked* in terms of something a human or a script can actually see. If a step's
outcome cannot be observed, the step is not done, and "it probably worked" is
not a result. Record what was actually seen in the task's ADR.

---

## What is true today, measured in this worktree on 2026-08-11

Every line here was re-run rather than trusted. Where a widely-repeated claim
turned out to be wrong or incomplete, it says so.

| Claim | Verified how | Result |
|---|---|---|
| Desktop suite green | `ctest --test-dir build` | **37/37 passed**, 0.94s |
| ESP32 firmware builds clean | `idf.py build`, ESP-IDF **v6.0.2** | success, zero warnings |
| Firmware size | build output | `kamiframe-firmware.bin` **0xa1370 = 660,336 bytes**, `ota_0` **58% free** — exactly ADR 0035's figure |
| Merged flash image | `idf.py merge-bin` | `build/merged-binary.bin`, **3,277,956 bytes (0x320484)** — bootloader + partition table + otadata + app **+ the assets partition contents** |
| Pack wired into `idf.py flash` | generated `flash_project_args` | `0x320000 ../../../examples/hello_sprite/assets.kfpack` |
| Packs in the tree | `find . -name '*.kfpack'` | hello_sprite **1,156 B**; hello_sprite_rgb565 2,116 B; hello_sprite_indexed 4,344 B; **creature_demo 556,488 B** |
| The two packs Tasks 5 and 7 verify | parsed their headers, `zlib.crc32` over the whole file | hello_sprite: magic `KFAP`, version 1, **1 entry**, directory at 16, **CRC32 `0x8f649872`**. creature_demo: `KFAP`, version 1, **94 entries**, directory at 16, **CRC32 `0xed73b5f7`**. These are the numbers `KFDBG PACK --verify` must reproduce off flash. |
| `pyserial` on this Mac | `python3 -c "import serial"` | **missing** from system python3; **present (3.5)** inside `~/esp/esp-idf`'s venv |
| Host debug tool self-test | `python3 tools/kf_debug_selftest.py` | all checks pass (no hardware needed) |
| Wokwi CLI | `which wokwi-cli` | **not installed**, and no `WOKWI_CLI_TOKEN` in the environment |
| A board attached | `ls /dev/cu.usb*` | **nothing attached right now** |

### Five things the premise of this work got wrong or did not know

These are not quibbles. Two of them would have made a task report success
against a number that cannot move.

**1. `KFDBG WATCH` is not a wire command and reports no budget numbers.**
It is `tools/kf_debug.py:857 cmd_watch()`, a loop that polls `KFDBG STATE`.
And `STATE`'s JSON (`ports/esp32/main/kf_dbg_bridge.cpp:529-534`) carries only
`fps` (derived from `mean_us`) and `frame_us` (`cpu_us`). It carries **no**
`draw_us`, `keyed_pixels`, `opaque_pixels`, `dirty_rect_count`,
`dirty_percent`, `transfer_us` or `over_budget` — every one of which Core
already computes into `kf_frame_stats` and simply never serialises. There is
today no way to read the frame budget off a device. **Task 1 fixes this.**

**2. The draw counters are structurally zero on the ESP32 build.** This is the
important one. `app_main.cpp:166` calls `kf_app_init(KF_DEMO_NONE)`, and
`kf_demo_update()`/`kf_demo_draw()` both return immediately in that mode
(`hakoniwaos/src/demo.cpp:260`, `:329`). The creature is drawn by
`kf_screen_nav_frame()` at `app_main.cpp:290` — *after* `kf_app_frame()` has
returned. But `kf_app_frame()` calls `kf_draw_counters_reset()` at its top
(`hakoniwaos/src/app.cpp:351`) and `kf_draw_counters_get()` at
`app.cpp:409`. Nothing draws between those two points on device. So
`opaque_pixels`, `keyed_pixels` and `draw_us` are **0 on every frame, forever**,
and would stay 0 if the indexed blit were a hundred times slower than assumed.
The dirty rectangles are *not* affected — they are marked after
`kf_fb_clear_dirty()` and read on the following frame, so `transfer_us` and
`dirty_rect_count` are right, just one frame late. The same hole exists in the
SDL simulator's creature path, which is why it can be fixed and tested on
desktop. **Task 1 fixes this too, and it must land before any budget claim.**

**3. Nothing ever turns the backlight on, and the debug bridge has taken the
backlight pin.** `KF_ESP_PIN_LCD_BL` and `KF_ESP_PIN_LCD_MISO` are both
`GPIO_NUM_6` (`kf_esp_pins.h:112`, `:132`) — a deliberate collision. With
`KF_DBG_BRIDGE_ENABLE=1`, which is the default and the only build that exists,
`esp_display.cpp:320-324` reserves GPIO6 as SPI MISO and `:376-385` **skips
configuring the backlight GPIO entirely**. Separately, `grep` finds **no caller
anywhere in the repo** of `kf_display_set_backlight()` — not Core, not
`app_main.cpp`. On the ILI9341 module this was invisible because that module's
LED pin is soldered to 3V3. The Waveshare 2in ST7789's header is
`VCC GND DIN CLK CS DC RST BL` — **BL is a real pin with nothing driving it**,
and it has no SDO for MISO to be worth reserving in the first place. A black
panel with a perfectly healthy log is the single most likely outcome of a naive
first run. **Task 4 fixes this; the bench workaround is one jumper.**

**4. Wokwi cannot test the panel, at all.** `ports/esp32/diagram.json` contains
exactly one part — `board-esp32-s3-devkitc-1` — wired only to the serial
monitor. No display, no buttons, no RTC. And Wokwi's parts library has **no
ST7789**; `ports/esp32/README.md:42-51` already records that an earlier claim
to the contrary was wrong. Wokwi is therefore worth running for boot, flash
mmap, KFDBG framing and the pet session — and worth nothing for pixels. Task 3
says so rather than implying otherwise.

**5. `kf_hal_assets_size()` returns the partition size on device, not the pack
size.** `esp_assets.cpp` maps `part->size` — all 12,582,912 bytes — so
`kf_assets_init()`'s `size <= KF_FLASH_ASSET_BUDGET_BYTES` check compares
12 MB against 12 MB and can never fire on hardware, and every offset bound is
checked against the partition rather than the pack. Only the `KFAP` magic
stands between a blank or stale partition and a parse. This is not a bug to fix
in this plan, but it is why Task 2's integrity check exists and why "the
firmware booted" is not evidence the right pack is in flash.

### Stale comments found while reading — defects by this codebase's own rule

A comment that contradicts its code is treated as a defect here. Five were
found. Each task below that touches the file fixes its own; the rest are listed
so nobody rediscovers them.

- `ports/esp32/CMakeLists.txt:4-8` — "Builds a plain hello-world (ADR 0019)…
  The ESP32 HAL backends kf_app_frame() needs still do not exist". They exist;
  this project builds the whole firmware. **Task 4 fixes.**
- `ports/esp32/hal/kf_esp_display_vsync.h:54-56` — "Default true".
  `esp_display.cpp:512` is `bool g_vsync_enabled = false`. **Task 4 fixes.**
- `ports/esp32/main/kf_dbg_bridge.cpp:166` — claims `kPayloadLineMax = 76`
  "Matches tools/kf_debug.py's own `len(line) > 76` check exactly". That host
  check was deleted; `kf_debug.py:245` now says the device wraps at 88 (it
  wraps at 76) and the host is width-agnostic. Harmless on the wire, wrong on
  the page. **Task 2 fixes** (it is the next task to touch this file).
- `docs/hardware-bringup.md:118-133` — the pin table still lists
  `Display DC | 9` and `Display backlight | 6`, contradicting its own line 439
  ("LCD_DC from GPIO9 to GPIO7") and `kf_esp_pins.h:110`. It also never
  mentions GPIO6 doubling as LCD_MISO. **Task 6 fixes**, and Task 6 is the task
  a human will have that page open for.
- `ports/esp32/hal/esp_time.cpp:39-41` — "NOT yet run against real hardware".
  Still literally true of *this driver* (the 2026-08-07/08 DS3231 session ran
  `ports/esp32-bringup`'s own code), so **leave it alone until Task 8** proves
  or disproves it, then update it with what was seen.

---

## Decisions already taken — record, do not re-litigate

- **Wokwi first, then the bench** (Chris). It needs no hardware and catches
  most of what a compile does not. Task 3.
- **Test the flash mapping in isolation and early, with a small pack, before
  the 556 KB one.** Tasks 2, 5, 7 in that order. The default
  `idf.py flash` already writes the 1,156-byte pack, so "small first" is the
  status quo and Task 2 makes switching to the big one a one-flag change
  rather than an edit.
- **KFDBG is the control surface.** Chris drives the board from his Mac, not by
  reaching for breadboard buttons. Anything the desktop debug window does must
  work over the wire. Buttons still get wired (Task 8) because the product has
  them, but they are not how the game gets tested.
- **The mutate gate is soft, not hard.** `KF_DBG_MUTATE_ENABLE` is a runtime
  `if` on a `constexpr bool` (`kf_dbg_bridge.cpp:393`, `:414`), so the eleven
  guarded handlers stay linked — ADR 0035 measured the firmware **32 bytes
  larger** with the gate off, and confirmed with `nm` that `handle_feed` and
  friends are still present. Fine for bench work. Nobody should read
  `KF_DBG_MUTATE_ENABLE=0` and conclude the code is gone.
- **Display tearing on changing content is expected and is not a bug.**
  ADR 0032: neither panel on the bench exposes a tearing-effect signal, the
  ST7789 has no SDO either, and the scanline-read workaround was measured and
  did not help. Do not spend bench time chasing flicker on a changing screen.
  Flicker on a *static* screen is a different thing and is a real bug — three
  such bugs were already found and fixed on 2026-08-08 (ADR 0027).

---

## The open question this plan must NOT answer

**The on-screen care guide says `1:FEED`, `2:PLAY`, `3:REST`, `4:BATH`,
`5:FLUSH`** — `simulator/src/pet/kf_creature_screen.cpp:579-581`, drawn once
from `kf_creature_screen_enter()` at `:988`, into the reserved band
y=[260,320). That file is compiled into **both** builds from one copy
(`simulator/CMakeLists.txt:88`, `ports/esp32/main/CMakeLists.txt:95`), so
whatever it says, the device says.

Those labels name keys on Chris's keyboard. The device has seven physical
buttons and no number keys, so on hardware the guide names controls that do not
exist. The source already admits this (`:566-572`) and declines to guess.
Chris said the device UI should match the simulator; taken literally, that
ships keyboard hints to hardware.

**This is Chris's call. The plan does not make it.** The four options and what
each actually costs:

| Option | What it costs | What it breaks or keeps |
|---|---|---|
| **A. Ship it as-is** | Nothing. | Keeps "one UI, both builds" literally true. The device tells the player to press keys it does not have. |
| **B. Different labels per build** — device shows `A:FEED`, simulator keeps `1:FEED` | Small: a second label table plus a compile-time choice of which. Perhaps an hour. | Correct on both. Breaks the stated rule that the device UI matches the simulator — two UIs that mimic each other, which is the failure state the architecture rules name. |
| **C. One label set, button names, on both** — both show `A:FEED` etc., and the simulator's keyboard mapping is documented so the printed name is true there too | Medium: one table, plus revisiting `sdl_input.cpp`'s 1-5 care remap so the labels aren't lying on desktop either. Half a day, and it changes how the simulator feels to use. | The only option where "the device UI matches the simulator" and "the labels are true" are both satisfied. |
| **D. Icons** | Largest: generated art that does not exist. The source comment at `:560-562` already says a true icon needs art nobody has made. | Solves it permanently and is the only option that survives having no text at all. Not a bring-up-week job. |

Tasks 6 and 8 ship whatever is in the tree at the time. If Chris decides during
bring-up, it is a separate small change, not a re-plan.

---

## Global Constraints

Every task's requirements implicitly include this section.

- **`hakoniwaos/` stays heap-free.** `python3 tools/check_no_heap.py .` runs in
  `bash dev.sh test` and fails the build. No `malloc`/`new` in any Core file.
- **`hakoniwaos/` stays free of floating point.** No `float`, no `double`.
  `KFDBG STATE` already prints fractional `fps` as two integers precisely to
  avoid float `printf` (`kf_dbg_bridge.cpp`); new numeric fields do the same.
- **240x320 RGB565, no alpha anywhere.** Transparency is colour-key only,
  magenta `KF_RGB(255,0,255)`.
- **Maximum 8 dirty rectangles per frame** (`KF_MAX_DIRTY_RECTS`,
  `hakoniwaos/include/kf/framebuffer.h:44`). Past 8 the framebuffer collapses
  to one screen-sized bounding box and re-transfers ~31 ms against a 33.3 ms
  budget. The creature costs **1 rect standing, 2 walking**. No task here may
  change either number.
- **Do NOT run `cmake -B build`** for the desktop target. It is already
  configured and reconfiguring costs ~2 minutes. Build with
  `cmake --build build -j8`. Run tests with `ctest --test-dir build`.
  **Desktop baseline was 37/37 when this plan was written; it has moved
  since — run `ctest --test-dir build -N` for today's count before starting**
  (44/44 with the default `KF_ENABLE_LVGL=OFF` as of 2026-08-11, 46/46 with it
  ON, and both will have moved further by the time this is read). Whatever
  that count is, it must hold plus whatever a task adds — the step-by-step
  "38/38" figures below were computed against the stale 37 baseline and need
  the same adjustment: read them as "today's baseline + 1", not literally.
- **ESP-IDF needs its environment sourced, and this sandbox blocks bare
  `source`.** Previous agents worked around it by writing the sequence to a
  script and running `bash script.sh`. That works; it is how every `idf.py`
  result in this document was produced:

  ```bash
  cat > /tmp/idf.sh <<'EOF'
  . $HOME/esp/esp-idf/export.sh > /dev/null 2>&1
  cd /Users/chris/Projects/kamiframe/.claude/worktrees/pixellab-mcp-server-960c5f/ports/esp32
  idf.py build
  EOF
  bash /tmp/idf.sh
  ```

  Sourcing `export.sh` also puts **pyserial 3.5** on the path, which system
  python3 does not have — so `tools/kf_debug.py` must be run from a shell that
  has sourced it, or against a python that has pyserial.
- **The pet ages while powered off.** The DS3231 is what makes that true.
  Offline fast-forward is part of what "the game works" means (Task 8) and is
  verified on device, not assumed.
- **Do not weaken death-by-neglect, the neglect accumulator, or stage durations
  to make testing easier.** `KFDBG JUMP`, `ADVANCE` and `MULT` exist precisely
  so nobody has to.
- **`kf/budget.h`'s banner forbids moving a budget number to make a test
  pass.** It does *not* forbid replacing an assumption with a measurement.
  `KF_DRAW_KEYED_PX_PER_US` is explicitly flagged "ASSUMPTION, NOT MEASURED";
  Task 7 may correct it *from data*, and must record the measurement next to it.
- **Wire protocol compatibility.** `tools/kf_debug.py` and
  `ports/esp32/main/kf_dbg_bridge.cpp` are one contract in two languages. Any
  new command changes both, plus `tools/kf_debug_selftest.py`, in the same
  commit. `tools/kf_panel.py` is out of scope for every task here — the same
  boundary ADR 0031/0034/0035 each drew for their own new commands.

---

## The four risks, and which task retires each

| # | Risk | Retired by | Cost if it bites |
|---|---|---|---|
| 1 | **`esp_partition_mmap()` has never returned a real byte on real silicon.** ADR 0033's own "Not verified" says so. It sits directly under the render path. | Task 2 builds the check, Task 3 runs it in Wokwi, **Task 5 runs it on silicon with 1,156 bytes**, Task 7 repeats it with 556,488. | Nothing draws. Every later observation becomes unattributable. |
| 2 | **The frame budget for indexed sprites is unmeasured on a 240 MHz core with no GPU.** An indexed blit is a byte load, a key compare and a palette load per pixel — no `memcpy` fast path. | **Task 1 builds the instrument** (without it the number is structurally 0); Task 7 takes the measurement. | The game misses 30fps and nobody can say which layer is at fault. |
| 3 | **None of this session's rendering code has executed on device.** It compiles. That is not the same thing. | Task 3 (boot, no pixels), Task 5 (boot on silicon), Task 6 (pixels). | Anything. |
| 4 | **Tearing is expected and unfixable in software** (ADR 0032). | Nothing — it is accepted, and named here so no task spends time on it. | Bench time, if someone chases it. |

**Sequencing rationale, in one sentence:** each task retires exactly one risk
against exactly one variable, so that when something fails you already know
what changed — which is why the panel is deliberately left unwired until
Task 6, and why the 556 KB pack does not go near flash until Task 7.

---

## Who has to be at the bench

Chris is not always at the board. Split explicitly:

| Task | Needs hardware? | Needs Chris? |
|---|---|---|
| 1 — the budget instrument | No | No |
| 2 — the flash integrity command | No | No |
| 3 — Wokwi runs the real image | No board. **Needs a `WOKWI_CLI_TOKEN`, which only Chris can issue** (`https://wokwi.com/dashboard/ci`) and which this environment cannot generate. | Once, to supply the token |
| 4 — panel selection and the backlight | No | No |
| 5 — first silicon boot, panel unwired | **Yes** — board on USB, nothing else wired | Yes, to plug it in |
| 6 — the panel | **Yes** — board + panel + eight jumpers | Yes |
| 7 — budget on silicon, real pack | **Yes** | Yes |
| 8 — care loop, buttons, RTC, power-cut | **Yes** — board + panel + 7 buttons + DS3231 + CR2032 | Yes, including a physical unplug |

Tasks 1, 2 and 4 are three self-contained code changes that can all be done,
reviewed and merged before the board is ever plugged in. **Do them first even
if the board is sitting right there.**

---

### Task 1: Make the frame budget measurable at all

Nothing here touches hardware. This task exists because the number Task 7 is
supposed to read is currently, provably, zero — and would stay zero if the
renderer were catastrophically slow. Build the instrument before trusting the
reading.

Two independent defects, one task, because they are the same measurement: the
counters are reset inside a window nothing draws in, and the fields are never
serialised for KFDBG to read.

**Files:**
- Modify: `hakoniwaos/src/app.cpp` (move `kf_draw_counters_reset()`; comment)
- Modify: `ports/esp32/main/kf_dbg_bridge.cpp` (`handle_state()`'s JSON)
- Modify: `ports/esp32/main/app_main.cpp` (measure the post-`kf_app_frame()`
  segment; publish it)
- Modify: `tools/kf_debug.py` (`cmd_state`/`cmd_watch` print the new fields)
- Modify: `tools/kf_debug_selftest.py` (a `FakeLink` STATE reply carrying them)
- Test: `simulator/src/headless/headless_main.cpp` (new
  `run_frame_counters_check()`)
- Modify: `simulator/CMakeLists.txt` (register `frame_counters_check`)
- Create: `docs/architecture/adr-0036-frame-counter-window.md`

**Interfaces:**
- Consumes: `kf_draw_counters_reset()` / `_get()` (`kf/blit.h:126-127`),
  `kf_app_last_frame()` returning `const kf_frame_stats *` and
  `kf_app_frame_summary()` returning `kf_frame_summary` (`kf/app.h:138`,
  `:118-125`).
- Produces: a new port-level accessor pair in `ports/esp32/main/app_main.cpp`,
  declared in a header `kf_dbg_bridge.cpp` can include —
  `uint32_t kf_app_post_frame_us(void);` — returning the measured microseconds
  spent between `kf_app_frame()` returning and the end of the loop body's own
  work. Task 7 reads it. Also produces the enlarged `KFDBG STATE` JSON key set
  listed below, which Tasks 5, 6, 7 and 8 all read.

**Design note — the trap here is that the fix looks like it belongs at the top
of the frame, and it belongs at the bottom.**

The instinct is "reset the counters later, just before the drawing". You
cannot: `kf_app_frame()` is Core and does not know that a port draws things
after it returns. The correct move is smaller and stranger — **reset
immediately after `kf_draw_counters_get()` instead of at the top of the
frame.** Do that and the counters' window becomes "from the end of last
frame's accounting to the end of this frame's accounting", which is
*exactly* the window the dirty rectangles already occupy: the creature's marks
survive `kf_fb_clear_dirty()` because they are made after it, and are read on
the following frame. Counters and rectangles then describe the same set of
drawing. Today they do not, which is why `dirty_rect_count` looks right on
device while `keyed_pixels` is 0.

The second trap is vacuity, and this codebase has been bitten by it twice
(see `2026-08-09-creature-on-screen.md`: "an anti-vacuity assertion turned out
to pass with the entire drawing path deleted"). A test that asserts
`keyed_pixels >= 0` proves nothing. The new check must assert a **non-zero**
count of the right order of magnitude, and must fail if the drawing call is
removed.

Third: `KF_DEMO_NONE` is not the only mode. The desktop `--stress` path uses
`KF_DEMO_FULLSCREEN`, where drawing *does* happen inside `kf_app_frame()`.
Moving the reset must not change what that path reports —
`docs/frame-budget.md`'s published numbers came from it. Under
`KF_DEMO_FULLSCREEN` the demo draws in the same iteration it is measured in,
so the window shift moves nothing: each frame still contributes exactly one
demo draw to exactly one report.

- [ ] **Step 1: Write the failing test**

Add `run_frame_counters_check()` to
`simulator/src/headless/headless_main.cpp`, near the other `run_*_check()`
functions, and register it as `--verify-frame-counters`. It must:

1. Drive the app the way the *device* does — `kf_app_init(KF_DEMO_NONE)`,
   then a loop of `kf_app_frame()` followed by the creature screen's own
   frame call, mirroring `app_main.cpp:247-290`. Reuse whatever entry point
   `run_creature_screen_check()` already uses to advance the creature screen
   rather than inventing a second one.
2. Run at least 3 frames, so the one-frame lag is exercised rather than
   accidentally avoided.
3. Assert `kf_app_last_frame()->keyed_pixels > 0`.
4. Assert it is the right order of magnitude for a 48x48 creature —
   `keyed_pixels >= 2000 && keyed_pixels <= 8000`. A window, not an equality:
   pose, mess and the guide band all legitimately move the exact figure, and an
   exact constant here would be a golden checksum nobody agreed to maintain.
5. Assert `dirty_rect_count >= 1 && dirty_rect_count <= KF_MAX_DIRTY_RECTS`.

Follow the existing checks' shape for reporting (`KF_ASSERT` or the
`printf`-and-return-nonzero convention already used in that file — match the
neighbours, do not introduce a third style).

- [ ] **Step 2: Run it and watch it fail for the right reason**

```
cmake --build build -j8
./build/kamiframe-headless --verify-frame-counters
```

Expected: **FAIL on the `keyed_pixels > 0` assertion.** If it fails on
anything else — a link error, a missing symbol, an assertion about the creature
screen not being entered — fix that first. A test that fails for the wrong
reason proves nothing about the bug.

- [ ] **Step 3: Move the counter reset**

In `hakoniwaos/src/app.cpp`: delete `kf_draw_counters_reset()` from line 351
(leaving `kf_arena_reset(KF_ARENA_SCRATCH)` where it is — the two are
unrelated and the arena reset genuinely belongs at the top), and call it
immediately after `kf_draw_counters_get()` at line 409.

Write the comment to say what is actually true — that the window deliberately
spans from the end of one frame's accounting to the end of the next, so it
covers drawing a *port* does after `kf_app_frame()` returns, and that this is
the same window the dirty rectangles already use. Do not describe it as
"per frame"; it is not, and the next reader will act on what the comment says.

- [ ] **Step 4: Run the test and the full suite**

```
cmake --build build -j8
./build/kamiframe-headless --verify-frame-counters
ctest --test-dir build
```

Expected: the new check PASSes; today's baseline + 1 (see the Global
Constraints note on why "38/38" no longer means anything literal). If
`headless_determinism` or `headless_fullscreen` moved, stop — the window shift
was supposed to be invisible to `KF_DEMO_FULLSCREEN` and something else is
going on.

- [ ] **Step 5: Commit the Core half**

```bash
git add hakoniwaos/src/app.cpp simulator/src/headless/headless_main.cpp simulator/CMakeLists.txt
git commit -m "The draw counters count the frame the port actually drew"
```

- [ ] **Step 6: Measure the segment Core cannot see**

In `ports/esp32/main/app_main.cpp`, bracket the work that happens after
`kf_app_frame()` returns — `kf_pet_session_frame()`, `kf_screen_nav_frame()`,
the conditional `kf_lvgl_port_pump()`, `kf_lua_port_frame()` — with two
`kf_time_mono_us()` reads, and store the difference in a file-static
`uint32_t`. Expose it as `kf_app_post_frame_us()`, declared in a small header
in `ports/esp32/main/` that `kf_dbg_bridge.cpp` includes.

Why here and not in Core: this segment only exists because a *port* chose to
draw outside `kf_app_frame()`, so the measurement belongs to the port. Core's
`cpu_us` keeps meaning exactly what it means today — time inside
`kf_app_frame()`, which on device is dominated by `kf_display_present()`'s
real SPI transfer. The two numbers answer two different questions in Task 7
and must not be merged into one.

- [ ] **Step 7: Serialise the budget into `KFDBG STATE`**

In `handle_state()` (`kf_dbg_bridge.cpp:477`), add these keys, all integers:

`draw_us`, `transfer_us`, `cpu_us`, `post_us`, `dirty_rects`, `dirty_pct`,
`opaque_px`, `keyed_px`, `over_budget`, `worst_us`, `p99_us`, `frames`,
`over_budget_frames`

Sources: everything but `post_us` comes from `kf_app_last_frame()` and
`kf_app_frame_summary()` — the structs already carry all of them
(`kf/app.h:56-125`). `post_us` is Step 6's accessor. Keep the existing keys
unchanged; hosts and habits depend on them.

**Check the buffer.** The reply is built into a 512-byte buffer
(`kf_dbg_bridge.cpp:529-534` region). Thirteen new `"key":N,` pairs will not
fit. Raise it and say in the comment what the new figure was computed from,
or the first `STATE` on device truncates into invalid JSON — which the host
will report as a parse error miles away from the cause.

- [ ] **Step 8: Teach the host to print them**

`tools/kf_debug.py`: `cmd_state`'s human-readable output gains a budget line;
`cmd_watch`'s one-line-per-poll format gains `fps`, `cpu_us`, `post_us`,
`rects`. `--json` already passes the whole object through, so it needs
nothing. Keep `watch`'s line short enough to read in a terminal — this is the
command Chris will actually stare at for minutes at a time.

- [ ] **Step 9: Extend the host self-test and run it**

`tools/kf_debug_selftest.py`: extend the `FakeLink` STATE reply to carry the
new keys, and assert the formatter prints them and does not crash when a key
is **absent** (an older firmware on the bench is a real scenario during
bring-up, and a `KeyError` there would look like a hardware fault).

```
python3 tools/kf_debug_selftest.py
```
Expected: `all checks passed`.

- [ ] **Step 10: Cross-compile**

```bash
cat > /tmp/idf.sh <<'EOF'
. $HOME/esp/esp-idf/export.sh > /dev/null 2>&1
cd /Users/chris/Projects/kamiframe/.claude/worktrees/pixellab-mcp-server-960c5f/ports/esp32
idf.py build
EOF
bash /tmp/idf.sh
```
Expected: success, zero warnings, size within a few hundred bytes of
**660,336**. Record the new figure.

- [ ] **Step 11: ADR and commit**

Write `docs/architecture/adr-0036-frame-counter-window.md`: the counter-window
defect and why it made the device report zero, the reset move, why `post_us`
lives in the port rather than Core, the `STATE` key additions, the buffer
size change and how it was computed. Include a "Not verified" section saying
plainly that none of it has run on hardware yet.

```bash
git add -u
git add docs/architecture/adr-0036-frame-counter-window.md
git commit -m "KFDBG STATE reports the budget, and the budget is no longer zero"
```

**How you would know it worked:** on desktop, `--verify-frame-counters` passes
and fails if you delete the creature draw. On device (Task 5 onward),
`python3 tools/kf_debug.py state --json` returns `keyed_px` in the low
thousands rather than `0`, and `post_us` is a plausible non-zero number. If
`keyed_px` is still `0` on device after this task, the fix did not take and
**no budget claim in Task 7 is admissible**.

---

### Task 2: Prove the flash mapping without a display

The highest-value test in this plan, and it needs no panel, no buttons and
(for the code) no board. `KFDBG PACK` asks the device to CRC the bytes it can
see through `esp_partition_mmap()`; the host CRCs the same file on disk and
compares. If those two numbers match, `esp_partition_mmap()` works, the flash
cache works, `esptool_py_flash_to_partition()` wrote to the right offset, and
the sprite pointers the blitter dereferences are real. One number retires most
of risk 1.

This task also makes "which pack is in flash" a build flag, which is how the
small-pack-before-big-pack decision gets expressed in code rather than in a
sticky note.

**Files:**
- Modify: `ports/esp32/main/kf_dbg_bridge.cpp` (`handle_pack()`, dispatch entry;
  fix the stale `kPayloadLineMax` comment at `:166`)
- Modify: `ports/esp32/main/kf_dbg_bridge.h` (document the new command
  alongside the others)
- Modify: `ports/esp32/main/CMakeLists.txt` (`KF_ESP_ASSET_PACK` cache variable)
- Modify: `tools/kf_debug.py` (`pack` subcommand)
- Modify: `tools/kf_debug_selftest.py` (fake-transport coverage)
- Modify: `tools/README.md` (the new subcommand)
- Create: `docs/architecture/adr-0037-kfdbg-pack-integrity.md`

**Interfaces:**
- Consumes: `kf_hal_assets_base()` / `kf_hal_assets_size()`
  (`hakoniwaos/include/kf/hal/assets.h:75-76`); `kf_dbg_crc32(const void *,
  size_t)` (`ports/esp32/main/kf_dbg_codec.h:76`); the existing
  `reply_json()` / `reply_err()` helpers in `kf_dbg_bridge.cpp`.
- Produces: wire command `KFDBG PACK [len]` (observe tier — gated by
  `KF_DBG_BRIDGE_ENABLE` only, **never** by `KF_DBG_MUTATE_ENABLE`: it changes
  nothing). Host subcommand
  `python3 tools/kf_debug.py pack [--verify PATH]`. CMake cache variable
  `KF_ESP_ASSET_PACK`. Tasks 3, 5 and 7 all run this command.

**Design note — the trap is inventing a checksum.**

Do not add FNV, do not add a new hash, do not write a byte-comparison protocol.
`kf_dbg_codec.h:63-79` already provides IEEE 802.3 CRC-32 — the identical
algorithm as Python's `zlib.crc32`, chosen for exactly that ubiquity, and
already the agreed contract for every KFDBG frame. The host side is
`zlib.crc32(open(path,'rb').read())`. Adding a second algorithm would create
precisely the forever cross-language contract ADR 0033 refused when it declined
to hash asset names.

The second trap is deciding *how many bytes* to hash. The device cannot know
the pack's true length: `kf_hal_assets_size()` returns 12,582,912 — the whole
partition — because `esp_assets.cpp` maps `part->size`. Hashing all 12 MB would
hash 12 MB of erased flash and take seconds. So **the host supplies the
length**, because the host is the one holding the file. The device validates
`len <= kf_hal_assets_size()` and refuses otherwise; `PACK` with no argument
skips the CRC and reports header fields only, which makes it a cheap "is there
anything there" probe that is safe to run before you know what should be there.

Third trap: `esp_partition_mmap()` is not the thing being tested if you read
the bytes some other way. Hash **through `kf_hal_assets_base()`'s pointer**,
not through `esp_partition_read()`. `esp_partition_read()` would very likely
work when the mapping does not, and a green result from the wrong API is worse
than a red one.

Fourth: a CRC over 556,488 bytes of memory-mapped flash is a real read of every
byte through the flash cache, on the same core that runs the frame loop, from
the frame-loop thread (KFDBG commands execute there — ADR 0030 decision 1).
At ~556 KB it is a handful of milliseconds and will make one frame late. That
is fine and expected; do not add a task or a yield for it.

- [ ] **Step 1: Add the `PACK` handler**

In `kf_dbg_bridge.cpp`, add `handle_pack()` near the other observe-tier
handlers and an entry in `process_command_line()`'s chain (`:1187-1368`),
alongside `PING`/`SHOT`/`STATE` and **not** behind `require_mutate_enabled()`.

The JSON reply carries:

`base` (the mapped pointer as hex), `mapped_size`, `magic` (the four bytes at
offset 0 as a 4-character string, so a blank partition reads as something a
human recognises rather than a number), `version`, `entry_count`,
`directory_offset`, and — only when a length argument was given —
`crc_len` and `crc32`.

Read `version`, `entry_count` and `directory_offset` at the offsets
`hakoniwaos/src/assets.cpp:39-42` documents (`version` u16 at 4, `entry_count`
u16 at 6, `directory_offset` u32 at 8). Read them **through the mapped
pointer**, byte by byte or with `memcpy` into a local — not by casting the
pointer to a struct, which would assume an alignment the mapping does not
promise.

Guard the whole handler on `kf_hal_assets_base() != nullptr` and reply `err`
with a sentence naming the partition if it is null. Guard the header reads on
`mapped_size >= 16`.

- [ ] **Step 2: Make the flashed pack a build flag**

In `ports/esp32/main/CMakeLists.txt`, replace the hard-coded path at line 131
with a cache variable, keeping the small pack as the default:

```cmake
set(KF_ESP_ASSET_PACK
    "${CMAKE_CURRENT_LIST_DIR}/../../../examples/hello_sprite/assets.kfpack"
    CACHE FILEPATH
    "Absolute path to the .kfpack `idf.py flash` writes into the `assets` partition.")
esptool_py_flash_to_partition(flash "assets" "${KF_ESP_ASSET_PACK}")
```

Two things the comment above it must say, because both have already cost
someone an afternoon somewhere: the path must be **absolute** (a relative one
resolves against the build directory, not the source directory), and the value
is **cached**, so changing it needs `idf.py -DKF_ESP_ASSET_PACK=... build` and
not an edit to a file the build has already configured.

Also note the deliberate name difference from the desktop's `KF_ASSET_PACK`
(`simulator/CMakeLists.txt:59`) — same idea, different build system, and one
name for both would suggest they are one setting.

- [ ] **Step 3: Fix the stale comment you are standing next to**

`kf_dbg_bridge.cpp:166` claims `kPayloadLineMax = 76` matches a
`len(line) > 76` check in `tools/kf_debug.py`. That check was deleted; the host
now accepts any width (`kf_debug.py:242-255`), and its own comment there says
the device wraps at 88, which is also wrong. Correct **both** comments to say
what is true: the device wraps at 76, the host does not care, and the host
deliberately stopped caring after a cosmetic disagreement broke a real
screenshot.

- [ ] **Step 4: Add the host subcommand**

`tools/kf_debug.py` gains:

```
python3 tools/kf_debug.py pack
python3 tools/kf_debug.py pack --verify examples/hello_sprite/assets.kfpack
```

Bare `pack` sends `KFDBG PACK` and prints the header fields. With `--verify`,
read the file, send `KFDBG PACK <len>` where `len` is the file's byte count,
compute `zlib.crc32(data) & 0xFFFFFFFF`, and compare.

Print a verdict a tired person can read at a glance — the matching case says
so in one line with the length and the CRC; the mismatching case says **MISMATCH**
and prints both values and the length, because "which of these two numbers is
the device's" is exactly what you need at 11pm.

Handle the case where `mapped_size < len`: the device will reply `err`, and the
host should surface that text rather than a traceback.

- [ ] **Step 5: Extend the self-test**

`tools/kf_debug_selftest.py`: a `FakeLink` returning a canned `PACK` JSON,
asserting (a) the matching path reports success, (b) a deliberately wrong CRC
reports mismatch and is non-zero-exit, (c) an `err` reply raises
`KfDebugError` carrying the device's own text.

```
python3 tools/kf_debug_selftest.py
```
Expected: `all checks passed`.

- [ ] **Step 6: Build both targets**

```
cmake --build build -j8 && ctest --test-dir build
```
Expected: today's baseline + 1 (Task 1's addition included — see the Global
Constraints note). Nothing in this task touches desktop code, so anything
else moving is a signal.

```
bash /tmp/idf.sh
```
Expected: clean, zero warnings. Confirm the generated flash args still name the
1,156-byte pack at `0x320000`:

```
grep assets ports/esp32/build/flasher_args.json
```

- [ ] **Step 7: ADR and commit**

`docs/architecture/adr-0037-kfdbg-pack-integrity.md`: why CRC-32 rather than a
new hash; why the host supplies the length; why the read goes through the
mapped pointer specifically; the `KF_ESP_ASSET_PACK` variable and the
small-before-big decision it encodes; a "Not verified" section stating that no
byte has yet been read off real flash — that is Task 5's job.

```bash
git add -u
git add docs/architecture/adr-0037-kfdbg-pack-integrity.md
git commit -m "KFDBG PACK checksums the flash the blitter will read"
```

**How you would know it worked:** on desktop, only the self-test. The real
answer arrives in Task 5, and it is binary: `pack --verify` prints match or
mismatch. There is no ambiguous middle.

---

### Task 3: Wokwi runs the real image, with the real partition table

No board. This is the last thing that can be learned for free, and it is worth
more than it looks: it executes `app_main()`, the ESP32 HAL, the pet session,
LVGL, Lua and KFDBG on a simulated S3 with a simulated 16 MB flash. What it
cannot do is show a pixel — Wokwi has no ST7789 part, and
`ports/esp32/diagram.json` today contains one board and a serial monitor.

**Files:**
- Modify: `ports/esp32/wokwi.toml` (point at the merged image)
- Modify: `ports/esp32/README.md` (the Wokwi section, with what it does and
  does not prove)
- Modify: `ports/esp32/CMakeLists.txt` (the stale hello-world header comment)
- Create: `docs/architecture/adr-0038-wokwi-bringup-run.md` (results)

**Interfaces:**
- Consumes: `idf.py merge-bin`, `KFDBG PING`/`STATE`/`PACK`.
- Produces: nothing other code depends on. A recorded boot log.

**Design note — the trap is that `wokwi.toml` points at an app-only binary.**

`wokwi.toml` currently names `build/kamiframe-firmware.bin`. That file is the
application image alone: no bootloader, no partition table, **no assets
partition**. Run it as-is and either it will not boot or it will boot against a
default partition layout with no `assets` entry, and `kf_assets_init()` will
abort at `hakoniwaos/src/app.cpp:305` — which would look exactly like the mmap
failure this plan is trying to test for, from an entirely different cause. That
is the worst possible outcome: a red light pointing at the wrong thing.

`idf.py merge-bin` is the fix and it was verified in this worktree on
2026-08-11: it writes `build/merged-binary.bin`, **3,277,956 bytes = 0x320484**
— which is `0x320000` (the assets partition offset) plus `0x484` (1,156, the
pack). The assets are in there. Point Wokwi at that.

Second trap: a build is not automatically a merge. `merged-binary.bin` goes
stale silently the moment you rebuild. Say so in the README, in the same
breath as the command.

Third: **do not add a display part to `diagram.json` to make this feel more
complete.** The closest Wokwi has is `wokwi-ili9341`, and a green rectangle
appearing under a driver configured for a different controller would be
actively misleading about the panel this project is actually bringing up.
Leave the diagram alone.

- [ ] **Step 1: Ask Chris for a Wokwi CI token**

`wokwi-cli` is not installed on this machine and `WOKWI_CLI_TOKEN` is not in
the environment (both checked 2026-08-11). Only Chris can issue one, from
`https://wokwi.com/dashboard/ci`. He installs the CLI and exports the token; no
token goes in the repo, and `.env.example` does not learn about it.

If the token is not available, **skip to Task 4 and come back.** Tasks 4-8 do
not depend on this one. Wokwi is a cheap extra look, not a gate.

- [ ] **Step 2: Build and merge**

```bash
cat > /tmp/idfmerge.sh <<'EOF'
. $HOME/esp/esp-idf/export.sh > /dev/null 2>&1
cd /Users/chris/Projects/kamiframe/.claude/worktrees/pixellab-mcp-server-960c5f/ports/esp32
idf.py build
idf.py merge-bin
ls -l build/merged-binary.bin
EOF
bash /tmp/idfmerge.sh
```

Expected: `build/merged-binary.bin` at roughly 3,277,956 bytes. If it is
`0x320000` exactly with nothing after, the assets did not make it in — check
`flasher_args.json` for the `assets` entry before going further.

- [ ] **Step 3: Point `wokwi.toml` at the merged image**

Change `firmware` to `build/merged-binary.bin`; leave `elf` pointing at
`build/kamiframe-firmware.elf` (Wokwi uses it for symbols and backtraces, and
the merged image has none).

Write the comment to say why: the app-only image has no partition table and
therefore no `assets` partition, so `kf_assets_init()` would abort at boot for
a reason that has nothing to do with the code under test. And say that
`merge-bin` must be re-run after every build.

- [ ] **Step 4: Run it**

```bash
cat > /tmp/wokwi.sh <<'EOF'
. $HOME/esp/esp-idf/export.sh > /dev/null 2>&1
cd /Users/chris/Projects/kamiframe/.claude/worktrees/pixellab-mcp-server-960c5f/ports/esp32
wokwi-cli . --timeout 30000
EOF
bash /tmp/wokwi.sh
```

Capture the whole log. The things to find, in order:

1. The `app_main` banner — chip info, **16 MB flash, ~8 MB PSRAM**
   (`app_main.cpp:147-159`). If PSRAM reads 0, nothing after it means
   anything: the Lua and LVGL arenas live there.
2. `assets: asset partition 'assets' mapped: 12582912 bytes at 0x…`
   (`esp_assets.cpp:78-79`). **This is the first time
   `esp_partition_mmap()` has ever returned on anything.** Simulated silicon
   is not silicon, but a failure here is a real failure.
3. No abort from `kf_assets_init()` — reaching the display banner at all means
   the `KFAP` magic parsed off the mapped pointer.
4. `ILI9341 (HiLetgo 2.8in) up: 240x320, 40000000 Hz SPI, RGB565,
   byte-swapped framebuffer` (`esp_display.cpp:387-390`). The profile name is
   ILI9341 because that is still the default — Task 4 changes that. Wokwi has
   no panel attached, so `esp_lcd` writes into the void and returns `ESP_OK`;
   this line proves the driver initialised, nothing about glass.
5. The pet session's 10-second `KF_LOGI` (`app_main.cpp:302-305`).
6. Whatever `kf_pet_load_and_advance()` says. On a fresh Wokwi flash there is
   no save and no RTC, so expect `wall clock not set yet -- skipping offline
   fast-forward this call` (`hakoniwaos/src/pet.cpp:1201`). **That is the
   correct output here, not a fault** — Task 8 is where it has to change.
7. The once-a-second budget report (`kf_app_log_budget_report()`).
   With Task 1 landed, its pixel counts should be **non-zero**.

- [ ] **Step 5: Drive KFDBG over the Wokwi serial link**

Wokwi's serial monitor is bidirectional. Send `KFDBG PING`, then
`KFDBG STATE`, then `KFDBG PACK 1156`, and check the replies frame correctly —
`KFDBG-BEGIN <type> <len>`, base64 lines, `KFDBG-END <crc>`.

If `wokwi-cli` cannot inject stdin conveniently in this setup, do not build
scaffolding for it: note it and let Task 5 be the first real KFDBG exercise.
This step is opportunistic.

The one thing worth real attention if it does work: **ADR 0030 names log/KFDBG
interleaving as its single biggest unverified risk.** The device mutes
`esp_log` output during a transfer (`kf_dbg_bridge.cpp:1431-1471`) but bare
`printf` — the boot banner — bypasses the filter. A `PING` issued mid-banner is
a cheap way to see whether the framing survives noise.

- [ ] **Step 6: Fix the stale project comment**

`ports/esp32/CMakeLists.txt:4-8` still describes this project as an ADR 0019
hello-world whose HAL backends "still do not exist". Replace it with what the
project is now, and keep the usage block (`set-target`, `build`,
`flash monitor`, `wokwi`) — that part is still correct and useful.

- [ ] **Step 7: Record what happened and commit**

`docs/architecture/adr-0038-wokwi-bringup-run.md`: the merged-image decision,
the log excerpts above with real values, and — with equal prominence — a
section saying Wokwi proved **nothing at all** about the ST7789, the panel
wiring, SPI timing, the DS3231, the buttons, or the frame budget on real
silicon, because it has no display part and no peripherals in the diagram.

```bash
git add -u
git add docs/architecture/adr-0038-wokwi-bringup-run.md
git commit -m "Wokwi boots the whole image, assets partition and all"
```

**How you would know it worked:** the log reaches the pet-session line without
an abort, and the mmap line reports 12,582,912 bytes. **How you would know it
didn't:** an abort at `app.cpp:305`, a PSRAM figure of 0, or a boot loop. Each
of those is a different fault and the log distinguishes them.

---

### Task 4: The panel profile owns the read line, and something turns the backlight on

No hardware needed to write this; it is what makes Task 6 survivable. Two
defects that will present as one symptom — a black screen and a clean log.

**Files:**
- Modify: `ports/esp32/hal/kf_panel_profile.h` (`has_read_line` field, both
  profiles)
- Modify: `ports/esp32/hal/esp_display.cpp` (MISO reservation, backlight
  config, turn it on)
- Modify: `ports/esp32/hal/kf_esp_pins.h` (the GPIO6 comment, which currently
  describes the old rule)
- Modify: `ports/esp32/hal/kf_esp_display_vsync.h` (the "Default true" lie at
  `:54-56`)
- Modify: `ports/esp32/main/kf_dbg_bridge.cpp` (`SCANLINE`/`VSYNC` refuse
  politely on a profile with no read line)
- Modify: `ports/esp32/main/CMakeLists.txt` (`KF_PANEL` cache variable)
- Create: `docs/architecture/adr-0039-panel-read-line-and-backlight.md`

**Interfaces:**
- Consumes: `kf_panel_profile` (`kf_panel_profile.h:88-97`),
  `KF_PANEL_PROFILE` (`:223-225`), `KF_ESP_PIN_LCD_BL` / `KF_ESP_PIN_LCD_MISO`
  (`kf_esp_pins.h:112`, `:132`).
- Produces: `bool has_read_line;` on `kf_panel_profile`; CMake cache variable
  `KF_PANEL` accepting `ili9341` or `st7789`. Tasks 5-8 all build with
  `-DKF_PANEL=st7789`.

**Design note — the trap is that the condition is currently on the wrong
question.**

Today the code asks *"is the debug bridge compiled in?"* and reserves GPIO6 as
SPI MISO if so (`esp_display.cpp:320-324`), skipping the backlight GPIO to
avoid driver contention (`:376-385`). Both halves of that are correct
reasoning about the wrong variable. The real question is **"does this panel
have a read line at all?"** The ILI9341 does — that is what the SCANLINE
diagnostic reads. The Waveshare 2in ST7789's eight-pin header
(`VCC GND DIN CLK CS DC RST BL`) has no SDO, so reserving MISO for it buys
nothing and costs two real things: the backlight stays unconfigured, and — per
`esp_display.cpp:300-317`'s own analysis of ESP-IDF's
`bus_uses_iomux_pins()` — a non-native MISO drops **the entire bus** (MOSI,
SCLK, CS) off the IOMUX fast path onto the GPIO matrix. The measured 40 MHz
ceiling was measured on the IOMUX path. So moving this decision into the panel
profile also hands the ST7789 back the fast path it should never have lost.

The second half is smaller and stranger: **nothing calls
`kf_display_set_backlight()`.** Anywhere. Even on the `!KF_DBG_BRIDGE_ENABLE`
path the GPIO is configured and immediately driven **low** (`:384`). This never
showed up because the ILI9341 module's LED pin is soldered to 3V3. Turn it on
at the end of `kf_display_init()` — after the init table has run, so you do
not light a screen full of garbage — and only on a profile that owns the pin.

Third trap, and it is the one that will actually bite on the bench: with
`has_read_line == false`, `KFDBG SCANLINE` has no MISO to read and will return
whatever a floating input returns — plausible-looking numbers that mean
nothing. Make it reply `err` naming the profile. A diagnostic that invents data
is worse than one that refuses.

Fourth: keep the ILI9341 path **byte-identical in behaviour**. It is the one
configuration that has ever put pixels on glass (ADR 0027, 2026-08-08), and it
is the fallback if the ST7789 is dead on arrival again — the last one was.

- [ ] **Step 1: Add the field**

In `kf_panel_profile.h`, add `bool has_read_line;` to `kf_panel_profile`. Set
it `true` in `kf_panel_ili9341` (`:110-155`) and `false` in `kf_panel_st7789`
(`:172-206`).

Document it on the field: it means the module physically exposes SDO on a pin
this board has wired, which is what makes `esp_lcd_panel_io_rx_param()`
possible and what forces the GPIO6/backlight choice. Name the ST7789's actual
header pinout in the comment so the next person does not have to find the
module to know why it is `false`.

**Both profile initialisers must be updated in the same edit.** They are
aggregate initialisers; adding a field and updating only one leaves the other
silently zero-initialised — which happens to be the right value for the ST7789
and the wrong one for the ILI9341, i.e. it would break the only configuration
that works, quietly.

- [ ] **Step 2: Rewire the two conditions in `esp_display.cpp`**

- MISO (`:320-324`): reserve GPIO6 only when `KF_DBG_BRIDGE_ENABLE` **and**
  `kPanel.has_read_line`. Note that `kPanel` is a `constexpr`-style reference
  resolved at `:73`, so this becomes a plain `if` rather than a `#if` — the
  same shape ADR 0035 chose for the mutate gate, and for the same reason.
- Backlight (`:376-385`): configure the GPIO whenever it was **not** taken for
  MISO. This is now a runtime condition, so the `#if !KF_DBG_BRIDGE_ENABLE`
  goes away.
- After `rebuild_panel_io()` succeeds and the swap strips are allocated, turn
  the backlight **on** if this build owns the pin.

Rewrite all three comments. The existing ones are long, careful and about to be
wrong — the contention argument, the IOMUX argument and the
"this board's LED pin is wired to 3V3" argument are all still true but are now
conditional on the profile rather than on the flag. A stale comment here is
exactly the defect class this codebase treats as a bug.

- [ ] **Step 3: Make `SCANLINE` and `VSYNC` refuse without a read line**

In `kf_dbg_bridge.cpp`, `handle_scanline()` (`:702`) and `handle_vsync()`
(`:889`) return `err` when `!kPanel.has_read_line`, with text naming the
profile and the reason — the same "comprehensible rejection" shape
`require_mutate_enabled()` (`:414-422`) already uses, so the host surfaces a
sentence rather than a timeout.

- [ ] **Step 4: Fix the vsync header's stale default**

`kf_esp_display_vsync.h:54-56` says "Default true". `esp_display.cpp:512` is
`false`, and ADR 0032 records why: with the wait enabled, flicker on changing
content was unchanged on real hardware. Say that.

- [ ] **Step 5: Add the `KF_PANEL` cache variable**

In `ports/esp32/main/CMakeLists.txt`, accept `KF_PANEL` with values `ili9341`
(default, preserving today's behaviour) or `st7789`, and translate it into
`-DKF_PANEL_PROFILE=kf_panel_<value>` on the component. **Validate the value at
configure time** and `message(FATAL_ERROR ...)` on anything else — a typo that
reaches the compiler produces an error about an undeclared identifier that
reads like a code bug rather than a command-line mistake.

Usage, which belongs in the comment and in `ports/esp32/README.md`:

```
idf.py -DKF_PANEL=st7789 build
```

- [ ] **Step 6: Build both panel configurations**

A gate compiled in only one state is half-tested — the standard ADR 0035 set
for itself. Build both:

```bash
cat > /tmp/idfboth.sh <<'EOF'
. $HOME/esp/esp-idf/export.sh > /dev/null 2>&1
cd /Users/chris/Projects/kamiframe/.claude/worktrees/pixellab-mcp-server-960c5f/ports/esp32
idf.py -DKF_PANEL=ili9341 build && echo "=== ILI9341 OK ==="
idf.py -DKF_PANEL=st7789 build && echo "=== ST7789 OK ==="
EOF
bash /tmp/idfboth.sh
```

Expected: both clean, zero warnings. Record both sizes. Leave the tree
configured for **`st7789`** — that is the panel Tasks 5-8 use.

- [ ] **Step 7: Desktop suite unmoved**

```
ctest --test-dir build
```
Expected: unmoved from whatever count this task started with (see the Global
Constraints note — "38/38" is stale). Nothing here touches desktop code; if a
test moves, something was edited that should not have been.

- [ ] **Step 8: ADR and commit**

`docs/architecture/adr-0039-panel-read-line-and-backlight.md`: the condition
was on the wrong question; the backlight had no caller anywhere; the IOMUX
consequence of a non-native MISO and what it means for the ST7789's clock
ceiling; why `SCANLINE` refuses rather than returning floating-input noise; the
`KF_PANEL` variable. State plainly in "Not verified" that the ST7789 profile
has still never driven a panel — the first unit was faulty and returned — and
that first contact is Task 6.

```bash
git add -u
git add docs/architecture/adr-0039-panel-read-line-and-backlight.md
git commit -m "The panel profile decides whether GPIO6 is a read line or a backlight"
```

**How you would know it worked:** on the bench in Task 6, the panel is lit
before anything is drawn on it. Before that, the only evidence is that both
configurations compile and the ILI9341 path is unchanged. **The bench
workaround if it still comes up dark: wire BL straight to 3V3 and move on** —
that isolates the backlight question from the SPI question, which is the whole
point of the ordering.

---

### Task 5: First silicon boot, with the panel deliberately not wired

**Needs the board. Nothing else.** No panel, no buttons, no RTC. That is not
laziness — it is the point. If the panel is attached and the screen is black,
the cause is one of about eight things. If nothing is attached and the serial
log is silent, the cause is one of about two.

**Files:**
- Modify: `docs/hardware-bringup.md` (a short "firmware first boot" section
  after the diagnostic)
- Create: `docs/architecture/adr-0040-first-silicon-boot.md` (results)

**Interfaces:**
- Consumes: `KFDBG PING`, `STATE`, `PACK` from Tasks 1 and 2.
- Produces: the first real measurement of `esp_partition_mmap()`. Nothing in
  code.

**Design note — the trap is the USB port you plug into.**

The DevKitC-1 has two USB-C sockets. **Use the one labelled `UART`, not the one
labelled `USB`** — `docs/hardware-bringup.md:239` records this from the last
session: the `USB` socket is native USB on GPIO19/20 and `idf.py` finds no port
through it. Half an hour has already been spent on this once.

Second trap: `idf.py monitor` and `tools/kf_debug.py` both want UART0
exclusively. Two processes on one port produces garbled framing that looks
exactly like a protocol bug. **Quit the monitor before running `kf_debug.py`.**
KFDBG deliberately shares the console stream — `kUartNum =
CONFIG_ESP_CONSOLE_UART_NUM` = 0, 115200 8N1 (`kf_dbg_bridge.cpp:153`,
`sdkconfig`) — which is why one at a time is a hard rule and not a preference.

Third: `tools/kf_debug.py` needs pyserial, which system python3 does not have.
Run it from a shell that has sourced `export.sh` (pyserial 3.5 lives in
`~/.espressif/python_env/idf6.0_py3.14_env`). Verified 2026-08-11.

Fourth: **`idf.py flash` writes the assets partition** — that is
`esptool_py_flash_to_partition()`'s whole job — but `idf.py app-flash` does
not. If a later reflash ever skips the pack, `PACK` is how you find out
in one second instead of by wondering why sprites are wrong.

- [ ] **Step 1: Chris plugs the board in — nothing else attached**

USB-C into the socket labelled **`UART`**. No panel, no buttons, no RTC, no
card. Confirm the Mac sees it:

```
ls /dev/cu.*
```
Expected: a new `/dev/cu.usbserial-*` or `/dev/cu.wchusbserial-*`. (Checked
2026-08-11 with nothing attached: no such device, so anything appearing is the
board.) Nothing there means the cable is charge-only or it is the wrong socket
— try the other socket before touching anything else.

- [ ] **Step 2: Flash the ST7789 build**

```bash
cat > /tmp/idfflash.sh <<'EOF'
. $HOME/esp/esp-idf/export.sh > /dev/null 2>&1
cd /Users/chris/Projects/kamiframe/.claude/worktrees/pixellab-mcp-server-960c5f/ports/esp32
idf.py -DKF_PANEL=st7789 build
idf.py flash
EOF
bash /tmp/idfflash.sh
```

Watch the flash output for **five** regions written: bootloader `0x0`,
partition table `0x8000`, otadata `0x10000`, app `0x20000`, and
**`0x320000 ../../../examples/hello_sprite/assets.kfpack`**. If the fifth is
absent, stop — Task 2's wiring is not in this build and every later
observation about assets is void.

- [ ] **Step 3: Watch it boot**

```bash
cat > /tmp/idfmon.sh <<'EOF'
. $HOME/esp/esp-idf/export.sh > /dev/null 2>&1
cd /Users/chris/Projects/kamiframe/.claude/worktrees/pixellab-mcp-server-960c5f/ports/esp32
idf.py monitor
EOF
bash /tmp/idfmon.sh
```

The five lines that matter, in order:

1. Chip banner: **16 MB flash, ~8 MB PSRAM, 240 MHz** (`app_main.cpp:147-159`).
   PSRAM of 0 means the octal PSRAM is not configured and nothing downstream is
   trustworthy.
2. `assets: asset partition 'assets' mapped: 12582912 bytes at 0x…`
   (`esp_assets.cpp:78-79`). **This line is the first time
   `esp_partition_mmap()` has succeeded on real silicon in this project's
   history.** If it is absent or an error, stop and read the error text — the
   two failure paths (`no partition named 'assets'` vs
   `esp_partition_mmap(...) failed: N`) point at completely different causes.
3. No abort. Reaching the next line means the `KFAP` magic parsed **through the
   mapped pointer**.
4. `ST7789 (Waveshare 2in) up: 240x320, 40000000 Hz SPI, RGB565, native-endian
   framebuffer`. With no panel attached the SPI writes go nowhere and
   `esp_lcd` still returns `ESP_OK` — this proves the driver initialised, not
   that anything is connected. The **profile name** is the thing to check: if
   it says ILI9341, `-DKF_PANEL=st7789` did not take.
5. The once-a-second budget report, and the pet-session log every ten seconds.

Then quit the monitor (`Ctrl-]`).

- [ ] **Step 4: Prove the flash bytes are real**

```bash
cat > /tmp/kfdbg.sh <<'EOF'
. $HOME/esp/esp-idf/export.sh > /dev/null 2>&1
cd /Users/chris/Projects/kamiframe/.claude/worktrees/pixellab-mcp-server-960c5f
python3 tools/kf_debug.py ping
python3 tools/kf_debug.py pack
python3 tools/kf_debug.py pack --verify examples/hello_sprite/assets.kfpack
python3 tools/kf_debug.py state --json
EOF
bash /tmp/kfdbg.sh
```

Expected:
- `ping` returns a build date/time and the panel profile name.
- `pack` reports `magic` `KFAP`, `version` 1, `mapped_size` 12582912, and a
  plausible `entry_count`.
- **`pack --verify` matches**, over 1,156 bytes. **This is the single most
  important result in the entire plan.** Record the CRC in the ADR.
- `state --json` returns Task 1's new keys with `keyed_px` **non-zero**. Zero
  here means Task 1's fix did not reach this build.

- [ ] **Step 5: Check that logs and KFDBG frames do not corrupt each other**

ADR 0030's own largest named risk. Run `pack --verify` five times in a row and
`state` ten times. Every reply must decode with a matching CRC. Then run
`kf_debug.py shot` once — it is the longest transfer (~1.5 s for a real
screen; the console is muted for its whole duration) and therefore the best
provocation.

A `CrcMismatchError` here is a genuine finding, not a flake: log it verbatim,
note whether it reproduces, and note whether it correlates with the
once-a-second budget report landing mid-transfer. Do not paper over it with a
retry.

- [ ] **Step 6: Write down what happened**

Add a short "First boot of the firmware" section to
`docs/hardware-bringup.md`, after the diagnostic section: the `UART`-not-`USB`
socket, the one-process-per-port rule, the sourced-shell requirement for
pyserial, the five flash regions, and the five log lines with what each proves.

`docs/architecture/adr-0040-first-silicon-boot.md`: the actual log, the actual
CRC, the actual `state` JSON. **Then go back and edit ADR 0033's "Not
verified" section** — the first bullet, `esp_partition_mmap()` never having
returned a real byte, is now either closed or confirmed, and leaving it as-is
would be the same class of stale claim this plan keeps finding.

```bash
git add -u
git add docs/architecture/adr-0040-first-silicon-boot.md
git commit -m "The mapped pack checksums the same on flash as on disk"
```

**How you would know it worked:** `pack --verify` says match. **How you would
know it didn't, and what each failure means:**

| What you see | What it means |
|---|---|
| No serial device | Wrong socket, or a charge-only cable |
| Boot loop / no banner | Flash or PSRAM config — not this project's code |
| `no partition named 'assets'` | The partition table did not get written; reflash with plain `idf.py flash` |
| `esp_partition_mmap(...) failed: N` | **Risk 1 has bitten.** See below |
| Abort in `kf_assets_init` | Mapping succeeded, bytes are wrong — a blank or stale partition |
| `pack --verify` MISMATCH | Mapping and parse succeeded but the bytes differ — wrong pack flashed, or a real mapping defect |

**If `esp_partition_mmap()` fails outright**, the fallback is already known and
does not need designing under pressure: `esp_assets.cpp` gains a path that
maps a smaller window than `part->size` (ADR 0033 records that
`esp_partition.h` names a 4 MB figure for the original ESP32 and that the S3's
MMU is unmeasured here), or falls back to `esp_partition_read()` into a PSRAM
buffer sized to the pack. The 556 KB creature pack fits in 8 MB of PSRAM with
room to spare, so **the game is not blocked either way** — it just stops being
zero-copy, which is a performance decision rather than an existential one. Do
not build either fallback speculatively.

---

### Task 6: The panel

**Needs the board, the 2in ST7789 and eight jumpers.** The first pixels.

**Files:**
- Modify: `docs/hardware-bringup.md` (fix the pin table; ST7789 wiring)
- Modify: `ports/esp32/hal/kf_panel_profile.h` (only if the init table or gaps
  turn out to need correcting)
- Create: `docs/architecture/adr-0041-st7789-first-light.md`

**Interfaces:**
- Consumes: Task 4's `-DKF_PANEL=st7789` build; `KFDBG SHOT`.
- Produces: a verified (or corrected) `kf_panel_st7789` profile.

**Design note — run the diagnostic before the firmware, and expect to correct
the profile.**

`ports/esp32-bringup` is a separate ESP-IDF project that shares exactly one
file with the firmware (`kf_esp_pins.h`) and depends on nothing else —
deliberately, so that "maybe hakoniwaos failed to compile" is not on the list
of things a black screen could mean. It runs the panel at **4 MHz**
(`bringup_main.cpp:76`) and paints a test card whose every feature diagnoses a
specific fault: a clipped edge means the panel needs `x_gap`/`y_gap`; a stripe
on the wrong side means the rotation byte; correct white and black but wrong
saturated colours means **byte order**; everything wrong means BGR. That table
(`docs/hardware-bringup.md:281-297`) is worth more than any amount of staring
at the firmware.

But note what it will be running: `kPanelIsIli9341 = true`
(`bringup_main.cpp:112`). Flip it for this panel.

ADR 0029 says it plainly and it should be believed: *"The ST7789 profile is
unverified in every respect… Expect to correct something in it on the first
real run."* The likely candidates, in order: `big_endian_fb` (currently
`false`), `x_gap`/`y_gap` (currently 0/0, and a 240x320 ST7789 in portrait
usually wants 0/0 — but a module wired for 240x240 offsets does not), and
`use_builtin_init` (currently `true`, meaning `esp_lcd`'s own ST7789 init runs
and the 11-command table is *not* sent — check that assumption before
concluding the table is wrong, because the table may never have been sent at
all).

Second trap: the ST7789 module's `VCC` is **3V3**, not 5V. The DevKitC-1's 5V
pin is also diode-blocked from USB VBUS and reads ~0.55 V unless a solder
jumper is bridged (`docs/hardware-bringup.md`, ADR 0024) — so a panel that
looks unpowered may simply be on the wrong rail.

Third, and this is the tearing one: **flicker on changing content is expected**
(ADR 0032, and Global Constraints above). Flicker on a *static* screen is not,
and is a real bug of a kind already fixed three times. Know which one you are
looking at before spending an hour.

- [ ] **Step 1: Wire the panel with the board unpowered**

Eight wires, per `docs/hardware-bringup.md` stage 2 and `kf_esp_pins.h:95-112`:

| Panel pin | GPIO |
|---|---|
| VCC | **3V3** |
| GND | GND |
| DIN (MOSI) | 11 |
| CLK (SCLK) | 12 |
| CS | 10 |
| DC | **7** (not 9 — GPIO9 measured under a millivolt at the panel, 2026-08-07) |
| RST | 8 |
| BL | 6 |

If Task 4's backlight change is in doubt, **put BL on 3V3 instead of GPIO6**
for this first run. It removes one variable, and the variable it removes is the
one that makes every other fault look identical.

- [ ] **Step 2: Fix the pin table in the doc you are reading from**

`docs/hardware-bringup.md:118-133` still says `Display DC | 9` and never
mentions GPIO6 doubling as LCD_MISO. Its own line 439 and `kf_esp_pins.h:110`
disagree with it. Fix it **before** wiring from it, not after — this is a
doc whose entire job is to be correct while someone holds a jumper.

- [ ] **Step 3: Run the standalone diagnostic first**

In `ports/esp32-bringup/main/bringup_main.cpp`, set `kPanelIsIli9341 = false`
(`:112`). Then:

```bash
cat > /tmp/bringup.sh <<'EOF'
. $HOME/esp/esp-idf/export.sh > /dev/null 2>&1
cd /Users/chris/Projects/kamiframe/.claude/worktrees/pixellab-mcp-server-960c5f/ports/esp32-bringup
idf.py set-target esp32s3
idf.py flash monitor
EOF
bash /tmp/bringup.sh
```

Stage 1 (backlight) then stage 2 (test card, held 10 s at 4 MHz). Read the card
against the fault table at `docs/hardware-bringup.md:281-297`. **Do not move on
from a wrong test card** — every fault it names is a wiring or profile fault
that the firmware will reproduce identically and explain far worse.

If the panel is completely dead, set `kPanelDebugMode = true` (`:135`) for
stage 2a, which holds each control line high for 6 s and low for 6 s so a
multimeter can find the broken wire. It costs about three minutes per run and
it is how the last dead panel was diagnosed.

- [ ] **Step 4: Flash the firmware and look at the glass**

```bash
bash /tmp/idfflash.sh    # from Task 5: -DKF_PANEL=st7789 build, then flash
```

Expected, on the panel: the creature screen. A 48x48 creature on the field, the
mess area, and the care guide band along the bottom reading `1:FEED 2:PLAY
3:REST 4:BATH 5:FLUSH` — which is the open question above, showing up exactly
as predicted, and **not a bug to fix here**.

- [ ] **Step 5: Compare the framebuffer to the glass**

```bash
cat > /tmp/shot.sh <<'EOF'
. $HOME/esp/esp-idf/export.sh > /dev/null 2>&1
cd /Users/chris/Projects/kamiframe/.claude/worktrees/pixellab-mcp-server-960c5f
python3 tools/kf_debug.py shot --out /tmp/kf_shot.png
EOF
bash /tmp/shot.sh
```

This is the diagnostic that separates the two halves of the render path, and it
is worth being deliberate about. `SHOT` encodes `kf_fb_pixels()` — the
framebuffer **before** the display driver touches it. So:

- **PNG right, glass right** — done.
- **PNG right, glass wrong** — the fault is in `esp_display.cpp` or the wiring:
  byte order, gaps, rotation, the init table.
- **PNG wrong** — the fault is above the driver, in shared code that the
  desktop simulator runs identically. Reproduce it on desktop; do not debug it
  on the board.

That third case is the one worth protecting: a bug reproducible on the Mac
should never be chased on a breadboard.

- [ ] **Step 6: Watch a static screen for thirty seconds**

Not a formality. Static flicker is a real bug class here and three instances
were found this way on 2026-08-08 (ADR 0027): a DMA race duplicating bands,
unsent dirty rects re-transmitting unchanged frames, and non-idempotent LVGL
updates. A creature standing still should be **completely still**. Tearing
while it walks is expected and is not to be chased.

- [ ] **Step 7: Correct the profile if the panel asked you to, then rebuild**

Any change to `kf_panel_st7789` gets a comment saying what was observed and on
what date — the ILI9341 profile's fields are trusted precisely because they
carry that provenance.

- [ ] **Step 8: ADR and commit**

`docs/architecture/adr-0041-st7789-first-light.md`: the wiring as actually
built, what the test card showed, every profile field that had to change and
what symptom drove it, whether BL was on GPIO6 or 3V3, whether Task 4's
backlight change worked, and the static-screen result. **Then update ADR 0029's
"Not verified"** — "this driver has never driven a panel" is either closed or
newly qualified.

```bash
git add -u
git add docs/architecture/adr-0041-st7789-first-light.md
git commit -m "The ST7789 shows the creature, and the profile says what it took"
```

**How you would know it worked:** a creature on the glass, and a `SHOT` PNG
that matches it. **How you would know it didn't:** black (backlight or power),
white (ADR 0024 records that 80 MHz produced solid white on the ILI9341 —
though this build runs 40 MHz, so white here means something else), garbled
colour (byte order), or offset (gaps).

---

### Task 7: The frame budget on real silicon, with the real pack

**Needs the board and the panel from Task 6.** Two risks in one task because
they share a flash cycle: the 556 KB pack, and the first honest reading of what
an indexed blit costs on a 240 MHz core.

**Files:**
- Modify: `hakoniwaos/include/kf/budget.h` (**only** if the measurement
  contradicts `KF_DRAW_KEYED_PX_PER_US`, and only with the measurement recorded
  beside it)
- Modify: `docs/frame-budget.md` (a measured-on-silicon section)
- Create: `docs/architecture/adr-0042-measured-frame-budget.md`

**Interfaces:**
- Consumes: `KF_ESP_ASSET_PACK` (Task 2); `KFDBG STATE`'s budget keys and
  `post_us` (Task 1); `KFDBG PACK --verify`.
- Produces: measured values for `KF_DRAW_KEYED_PX_PER_US` and the real
  transfer rate. Nothing depends on them at compile time except the estimator.

**Design note — the trap is believing a number Task 1 was written to make
believable.**

Before quoting any figure, confirm `keyed_px` in `KFDBG STATE` is **non-zero**.
If it is zero, Task 1's fix is not in the flashed build and every number in
this task is meaningless. This is not a hypothetical: it is the exact state the
firmware was in when this plan was written.

Second trap: `cpu_us` and `post_us` measure different things and neither is
"the frame". `cpu_us` is time inside `kf_app_frame()`, which on device is
dominated by `kf_display_present()` — the real SPI transfer of the *previous*
frame's dirty rectangles. `post_us` is the creature's drawing plus the pet
session, LVGL and Lua. Together they are the frame's real cost; the pacing
delay at `app.cpp:445-446` is excluded from both, correctly.

Third: the pack swap and the budget reading are separate observations that
happen to share one flash. Do `pack --verify` **first**. If the 556 KB mapping
is wrong, the sprites are wrong, and any budget number taken afterwards is
measuring the wrong pixels.

Fourth, and this is the rule: **`kf/budget.h`'s banner forbids moving a number
to make a test pass. It does not forbid replacing an assumption with a
measurement.** `KF_DRAW_KEYED_PX_PER_US = 25` is explicitly labelled
"ASSUMPTION, NOT MEASURED" (`budget.h:123-138`). If the measurement contradicts
it, correct it and write the measurement next to it — the way
`KF_DISPLAY_SPI_HZ` already carries "MEASURED 2026-08-08". If the measurement
merely makes a target uncomfortable, leave it alone.

- [ ] **Step 1: Flash the creature pack**

```bash
cat > /tmp/idfbig.sh <<'EOF'
. $HOME/esp/esp-idf/export.sh > /dev/null 2>&1
cd /Users/chris/Projects/kamiframe/.claude/worktrees/pixellab-mcp-server-960c5f/ports/esp32
idf.py -DKF_PANEL=st7789 \
  -DKF_ESP_ASSET_PACK=/Users/chris/Projects/kamiframe/.claude/worktrees/pixellab-mcp-server-960c5f/examples/creature_demo/assets.kfpack \
  build
idf.py flash
EOF
bash /tmp/idfbig.sh
```

Absolute path, for the reason Task 2's comment gives. Confirm the flash output
names `creature_demo/assets.kfpack` at `0x320000` before it writes.

- [ ] **Step 2: Verify 556,488 bytes through the mapping**

```bash
cat > /tmp/packbig.sh <<'EOF'
. $HOME/esp/esp-idf/export.sh > /dev/null 2>&1
cd /Users/chris/Projects/kamiframe/.claude/worktrees/pixellab-mcp-server-960c5f
python3 tools/kf_debug.py pack --verify examples/creature_demo/assets.kfpack
EOF
bash /tmp/packbig.sh
```

Expected: match, over **556,488** bytes, with `entry_count` **94**. This is
485x more mapped data than Task 5 proved and is the scale question ADR 0033
actually raised. A mismatch here after a match in Task 5 is a *size*-dependent
mapping fault, which is a completely different diagnosis from "mmap doesn't
work" — record which it is.

- [ ] **Step 3: Confirm the instrument reads**

```bash
python3 tools/kf_debug.py state --json
```

`keyed_px` must be non-zero. If it is zero, **stop and go back to Task 1.**

- [ ] **Step 4: Take the readings**

```bash
python3 tools/kf_debug.py watch --interval 1.0
```

Let it run 60 seconds with the creature idle, then 60 seconds walking (the
wander moves it on its own; `KFDBG MULT 8` will make walking more frequent
without touching the creature's own animation clock). Record, for both:

`cpu_us`, `post_us`, `draw_us`, `transfer_us`, `keyed_px`, `opaque_px`,
`dirty_rects`, `dirty_pct`, `worst_us`, `p99_us`, `over_budget_frames`.

- [ ] **Step 5: Judge it against thresholds decided in advance**

Decided here, before the data, so the data cannot move them:

| Check | Threshold | If it fails |
|---|---|---|
| **Hard** | `cpu_us + post_us` stays under **33,333 µs** on every sampled frame with the creature idle | The game cannot hold 30fps at rest. Real problem; find which term dominates before proposing anything. |
| **Hard** | `dirty_rects` is **1** idle and **2** walking | Something is marking rectangles nobody planned. Past 8 the framebuffer collapses to a full-screen box and re-transfers ~31 ms. |
| **Soft** | `post_us` within **3x** of `draw_us` | `KF_DRAW_KEYED_PX_PER_US` is wrong enough to matter. Re-derive it: rate = `keyed_px / post_us`, allowing that `post_us` also carries the pet session, LVGL and Lua. Correct the constant with the measurement and date beside it. |
| **Soft** | `cpu_us` within **2x** of `transfer_us` | The SPI estimate is off — either `KF_DISPLAY_SPI_HZ` (remember Task 4 may have returned the ST7789 to the IOMUX fast path, which the 40 MHz figure was measured on) or `KF_DISPLAY_RECT_OVERHEAD_BYTES`, which still carries its assumption banner. |
| **Context** | `worst_us` and `p99_us` | Fine to exceed budget occasionally; a p99 over budget is not. |

For scale: at the assumed rate the creature is **2,304 keyed px ≈ 92 µs** to
draw plus **2,304 opaque px ≈ 23 µs** to erase — about 0.35% of the frame. Even
ten times worse it is 3.5%. **The indexed blit is very unlikely to be the
problem, and the transfer very likely is** (`docs/frame-budget.md`: a full
frame is 30.7 ms of wire at 40 MHz against a 33.3 ms budget). Say so in the ADR
whichever way the data falls, because "we measured it and it was fine" is a
result worth writing down.

- [ ] **Step 6: Record it**

Add a "Measured on silicon" section to `docs/frame-budget.md` with the real
numbers beside that document's existing desktop estimates — it currently
publishes 31 fps from a desktop model and deserves the comparison.

`docs/architecture/adr-0042-measured-frame-budget.md`: idle and walking
figures, every threshold above and whether it passed, any `budget.h` constant
corrected and the arithmetic that corrected it, and the 556 KB mapping result.

- [ ] **Step 7: Desktop suite still green, then commit**

```
ctest --test-dir build
```
Expected: unmoved from whatever count this task started with (see the Global
Constraints note — "38/38" is stale). A `budget.h` change can move a test
that asserts against a budget constant — if one moves, that is a real
conversation about whether the test encoded the assumption, not a licence to
edit the test.

```bash
git add -u
git add docs/architecture/adr-0042-measured-frame-budget.md
git commit -m "The frame budget, measured on the board instead of assumed"
```

**How you would know it worked:** `keyed_px` non-zero, `pack --verify` matching
over 556,488 bytes, and a `cpu_us + post_us` comfortably under 33,333 µs.
**How you would know it didn't:** `keyed_px` of 0 (instrument broken, not
hardware), a mismatch at 556 KB but not at 1 KB (size-dependent mapping), or
`dirty_rects` above 2 (something is drawing more than it claims).

---

### Task 8: The care loop, the buttons, and a pet that ages while off

**Needs the board, the panel, seven buttons and the DS3231 with its CR2032.**
The task that makes "the game works" true.

**Files:**
- Modify: `ports/esp32/main/kf_dbg_bridge.cpp` (`KFDBG WALL <epoch>`)
- Modify: `ports/esp32/main/kf_dbg_bridge.h` (document it)
- Modify: `tools/kf_debug.py` (`wall` subcommand)
- Modify: `tools/kf_debug_selftest.py`
- Modify: `ports/esp32/hal/esp_time.cpp` (the stale "NOT yet run against real
  hardware" header at `:39-41`)
- Modify: `docs/hardware-bringup.md` (the power-cut procedure for the firmware,
  distinct from the diagnostic's)
- Create: `docs/architecture/adr-00NN-care-loop-on-hardware.md`, where `NN` is
  the next free ADR number — 0043 through 0046 are already taken; confirm
  with `ls docs/architecture/` at dispatch.

**Interfaces:**
- Consumes: `kf_time_set_wall()` (`hakoniwaos/include/kf/hal/time.h`), the
  eleven mutate-tier KFDBG commands, `kf_pet_load_and_advance()`
  (`hakoniwaos/src/pet.cpp:1182-1231`) via `kf_pet_session_init()`
  (`kf_pet_session.cpp:203`).
- Produces: wire command `KFDBG WALL <epoch_seconds>` (**mutate tier** — it
  moves time, which moves the pet) and host subcommand
  `python3 tools/kf_debug.py wall [--now | EPOCH]`.

**Design note — the trap is that offline ageing cannot work yet and fails
silently.**

`esp_time.cpp` reads the DS3231's oscillator-stop flag and, if it is set,
**deliberately leaves the wall clock unset** (`:250-262`) rather than
auto-seeding — the opposite of what the bring-up diagnostic does, and correct:
a pet that ages by a wrong amount is worse than one that does not age. A fresh
CR2032 in a chip that has never been set has OSF set. So `kf_time_wall()`
returns `{valid=false}`, and `kf_pet_load_and_advance()` logs
`"wall clock not set yet -- skipping offline fast-forward this call, will try
again once it is"` (`pet.cpp:1201`) and returns.

And **`kf_time_set_wall()` has no production caller anywhere** —
`ports/esp32/README.md:400-427` lists it as a known gap. So there is currently
no way to set the clock on a device, which means offline ageing can never
start, which means the power-cut test can never pass. `KFDBG WALL` is the
missing piece, and it is three lines of handler plus a host subcommand.

It is mutate-tier, not observe-tier. Setting the clock forward is
indistinguishable from `ADVANCE` in its effect on the pet, and ADR 0035's whole
argument is that a serial cable must not be able to cheat the pet on a build
that says it cannot.

Second trap: buttons are **active-low with internal pull-ups**
(`esp_input.cpp:60-82`), so each switch wires GPIO to **GND** with no resistor.
A 4-pin tactile switch must be wired across **diagonally opposite** corners —
the other pairing is permanently shorted and the button will read as
permanently pressed.

Third: `kf_debug.py press` defaults to `--hold-ms 120`, and that default is
load-bearing. Core debounces at `kDebounceUs = 8000` and polls roughly every
33 ms, so a zero-length injected press can never be seen. `--hold-ms 0` will
silently do nothing.

Fourth: the power-cut test needs the USB cable **physically unplugged**, not
`RST`. `RST` does not remove power from the board, so the RTC's backup path is
never exercised and the test passes while proving nothing.

- [ ] **Step 1: Add `KFDBG WALL`**

In `kf_dbg_bridge.cpp`, `handle_wall()` behind `require_mutate_enabled()`, in
the same chain as `FEED`/`JUMP`. Parse one decimal epoch-seconds argument, call
`kf_time_set_wall()`, and `ack` with the value that was set **read back from
`kf_time_wall()`** rather than echoing the argument — the same discipline
`handle_jump()` already uses, and the thing that catches a DS3231 that accepted
a write and did not keep it.

Reject a zero or obviously-wrong epoch (anything before 2020-01-01, i.e.
1577836800) with a sentence saying so. A mistyped value here silently
fast-forwards the pet by decades.

- [ ] **Step 2: Add the host subcommand and self-test**

```
python3 tools/kf_debug.py wall --now
python3 tools/kf_debug.py wall 1786060800
```

`--now` sends the host's **local** wall time, not UTC — the RTC
(`hakoniwaos/include/kf/clock.h`, `kf/hal/time.h`) holds local time directly
with no timezone or UTC offset, so `int(time.time())` (UTC) would give the
pet the wrong bedtime for anyone outside GMT. Send
`int(time.mktime(time.localtime()))` instead. Extend
`tools/kf_debug_selftest.py` with a
`FakeLink` covering the success reply, the too-early rejection, and the
mutate-gated rejection (the pattern
`test_mutate_gate_rejection_is_actionable()` already established).

```
python3 tools/kf_debug_selftest.py
```
Expected: `all checks passed`.

- [ ] **Step 3: Wire the buttons and the RTC, board unpowered**

Seven switches, each GPIO to GND, wired across diagonally opposite corners:
UP **4**, DOWN **5**, LEFT **15**, RIGHT **16**, A **17**, B **18**,
MENU **21** (`kf_esp_pins.h:140-146`).

DS3231: VCC 3V3, GND, SDA **13**, SCL **14** (`:160-161`). **Remove the
resistor marked `201`** (or the diode) on the module before fitting a CR2032 —
`docs/hardware-bringup.md:170` — otherwise the trickle charger tries to charge
a non-rechargeable cell.

- [ ] **Step 4: Flash, and confirm the RTC is seen**

```bash
bash /tmp/idfbig.sh
bash /tmp/idfmon.sh
```

Expected in the log: the DS3231 found at `0x68` and disambiguated from an
MPU-6050 by its temperature register (`esp_time.cpp:220-238`), then either a
valid wall clock or the OSF path leaving it unset. On a fresh cell, unset is
the expected and correct outcome.

- [ ] **Step 5: Exercise the whole care loop over the wire**

```bash
cat > /tmp/care.sh <<'EOF'
. $HOME/esp/esp-idf/export.sh > /dev/null 2>&1
cd /Users/chris/Projects/kamiframe/.claude/worktrees/pixellab-mcp-server-960c5f
python3 tools/kf_debug.py reset
python3 tools/kf_debug.py state
python3 tools/kf_debug.py jump child
python3 tools/kf_debug.py care feed 0
python3 tools/kf_debug.py care play 1
python3 tools/kf_debug.py care rest 2
python3 tools/kf_debug.py care bath 0
python3 tools/kf_debug.py care 5
python3 tools/kf_debug.py advance 1d
python3 tools/kf_debug.py mult 64
python3 tools/kf_debug.py state
python3 tools/kf_debug.py mult 1
python3 tools/kf_debug.py press A --hold-ms 300
EOF
bash /tmp/care.sh
```

For each: the `ack` says what it did, `state` moves the way it should, **and
the panel changes.** All three. A care action that acks and changes state but
draws nothing is a real defect — that boundary is precisely where the
creature-on-screen plan's "each piece correct alone, broken where they meet"
failures lived.

The four care acks report a reaction (0 liked / 1 neutral / 2 disliked), which
ADR 0034 lists as never having been cross-checked against a real creature's
config on hardware. Check one against the desktop simulator with the same seed
and the same variation.

- [ ] **Step 6: Every button, one at a time**

Press each of the seven physically and confirm the firmware sees it. Feed, play,
rest, bath and flush are `KF_BTN_A/UP/DOWN/LEFT/RIGHT`
(`kf_creature_screen.cpp`'s `handle_care_buttons()`); MENU navigates screens.

Watch specifically for a button that reads as permanently pressed — that is the
diagonal-corner wiring mistake, and it presents as the game doing something
continuously rather than as a button that does nothing.

Note whether the on-screen guide's `1:FEED` labels match what the buttons
actually do. **Report it; do not fix it.** That is Chris's open question.

- [ ] **Step 7: The power-cut test**

1. `python3 tools/kf_debug.py wall --now`, then `state`, and write down
   `pet_age_s` and the three needs.
2. Let the pet run a minute so a save exists.
3. **Physically unplug the USB cable.** Not `RST`.
4. Wait **ten minutes** by a real clock. Long enough that the decay is
   unmistakable and short enough to sit through; the hunger curve moves at
   roughly 1,042 millipoints/hour (ADR 0025), so ten minutes is ~174 mp — well
   clear of any rounding.
5. Plug back in and read `state`.

Expected: `pet_age_s` advanced by approximately the wall time elapsed, and
hunger/happiness/energy decayed accordingly. In the boot log,
`kf_pet_load_and_advance()` fast-forwards instead of logging the
wall-clock-not-set warning.

**This is the proof that the DS3231, the save, the load path and the offline
fast-forward are one working chain.** Every one of them has been built and
none of them has been observed working together.

- [ ] **Step 8: Update the stale header and the doc**

`esp_time.cpp:39-41` says this driver has not run against real hardware. It
just did. Replace it with what was observed and the date.

Add a "Power-off ageing, firmware" section to `docs/hardware-bringup.md` —
distinct from the diagnostic's own RTC test at `:392-422`, which tests a
different program.

- [ ] **Step 9: ADR and commit**

`docs/architecture/adr-00NN-care-loop-on-hardware.md` (next free number —
0043 through 0046 are taken; confirm with `ls docs/architecture/` at
dispatch): `KFDBG WALL` and why it is mutate-tier; every care command's
observed behaviour on device; the button results; the power-cut numbers
before and after; and the `1:FEED` question restated as an open decision
with what the device actually showed. Then sweep ADRs 0026, 0028, 0030,
0031, 0034 and 0035 — each carries a "Not verified: nothing in this ADR has
run against real hardware" line, and after this task several of them are
simply false.

```bash
git add -u
git add docs/architecture/adr-00NN-care-loop-on-hardware.md
git commit -m "The pet takes care over the wire, and ages while the power is off"
```

**How you would know it worked:** a pet that visibly reacts to a `care feed`
sent from the Mac, seven buttons that each do their own thing, and a pet
measurably older after ten minutes with the cable out. **How you would know it
didn't:** an ack with no visual change (the boundary bug), a permanently-pressed
button (wiring), or `pet_age_s` unchanged after the power cut (RTC, or the wall
clock was never set).

---

## What this plan deliberately does not do

Each of these is separable, and most are named here specifically so nobody
mistakes them for oversights.

- **It does not answer the `1:FEED` question.** Four options with costs are
  above; the decision is Chris's. Tasks 6 and 8 report what the device showed
  and change nothing.

- **It does not chase tearing.** ADR 0032 measured the ILI9341's scanline
  counter at ~96 Hz, found the read costs 49-77 µs against a ~64 µs scan step,
  shipped the wait, and found it changed nothing on real hardware. The ST7789
  has no read line at all — Task 4 makes `SCANLINE` say so rather than return
  floating-input noise. Flicker on changing content is accepted. Flicker on a
  *static* screen is a different bug and Task 6 looks for it.

- **It does not add a KFDBG transport for the desktop simulator.**
  `tools/kf_panel.py:131-155` raises on `--target sim:` today. It would be
  genuinely useful — one host tool driving both builds is exactly this
  project's architecture — but nothing in bring-up needs it, and the desktop
  already has a debug window that does more.

- **It does not touch `tools/kf_panel.py`.** Same boundary ADR 0031, 0034 and
  0035 each drew for their own new commands. The GUI inherits `PACK` and `WALL`
  for free through `kf_debug.py`'s functions whenever someone wires them up.

- **It does not fix `kf_hal_assets_size()` returning the partition size.** The
  consequence — the budget assert can never fire on device, and offsets are
  bounded by 12 MB rather than by the pack — is documented above and is why
  Task 2 exists. The honest fix is for the ESP32 backend to narrow the reported
  size to the pack's real extent after the header is read, which means the HAL
  learning something about the format it deliberately does not know. That is an
  ADR-sized argument, not a bring-up task.

- **It does not wire the microSD card.** Six wires, its own SPI3 bus, and
  ADR 0024's finding that SDIO probing breaks plain SD cards
  (`CONFIG_SD_ENABLE_SDIO_SUPPORT=n`, which the ADR says belongs in
  `ports/esp32/sdkconfig.defaults` and is not there). Nothing in the game reads
  it — the save is 70 bytes in NVS.

- **It does not touch the deferred peripherals.** IMU, ambient light sensor,
  mic, I2S amp and speaker, haptic, buzzer, BME280, TP4056, LiPo. The I2S pins
  are reserved in `kf_esp_pins.h:203-206` and have never been wired to
  anything. `docs/hardware-bringup.md:16-34` already scopes the MVP to four
  parts.

- **It does not re-run the SPI clock sweep.** ADR 0024 measured 40 MHz on the
  ILI9341 with the bus on the **IOMUX** fast path; `esp_display.cpp:300-317`
  notes that reserving GPIO6 as MISO drops the whole bus to the GPIO matrix and
  that 40 MHz was never re-measured there. Task 4 gives the ST7789 the IOMUX
  path back, which means there may be headroom above 40 MHz — and
  `docs/frame-budget.md` says 80 MHz would take the full-screen ceiling from
  32 fps to 65. Worth a sweep once the panel is trusted. Not while it is the
  variable under test.

- **It does not add OTA.** `ota_0`/`ota_1` exist in the partition table so the
  option stays open (ADR 0033). No client code, and none needed for a board on
  a cable.

- **It does not generate animation art.** No longer true as a blanket claim —
  18 of `examples/creature_demo/assets.kfpack`'s 94 entries now carry nine
  frames each (verified against the pack directory), so some idles genuinely
  animate on hardware. The remaining 76 entries are still single-frame, so
  this line's spirit — most of the roster still needs generation spend, not a
  code gap — still holds; it is a shrinking gap, not a closed one.

---

### Task 9: The stats band — the player can see how the pet is doing

Added 2026-08-11, after the game ran on hardware. The owner's words: *"I also
can't see the pet's stats anywhere now like how hungry/tired etc."*

He is right, and it is a regression in what he can see rather than an omission.
The old LVGL pet screen (`simulator/src/lvgl/kf_pet_screen.cpp`) had bars for
hunger, happiness and energy. Task 4 of the creature-on-screen plan routed Home
to the custom-drawn creature screen and made that LVGL screen unreachable,
reserving rows 260-319 for a replacement band and drawing nothing in it. That
plan named the gap in its own "deliberately does not do" section. This closes it.

**Files:**
- Modify: `simulator/src/pet/kf_creature_screen.cpp` (draw the band)
- Modify: `simulator/src/pet/kf_creature_screen.h` if a debug accessor helps a test
- Test: `simulator/src/headless/headless_main.cpp`
- Modify: `simulator/CMakeLists.txt`

**Interfaces:**
- Consumes: `kf_pet_state`'s need values (read `kf/pet.h` for their real names,
  ranges and units — do not assume percentages), `kf_text_draw()` /
  `kf_text_width()` from `kf/font.h` (ADR 0010), `kf_fill_rect()`.
- Produces: no new public API beyond any test accessor.

**Design note — the trap is the dirty-rect budget, and it is the same trap
Task 5 of the creature plan already sprang once.**

The creature costs 1 rectangle standing still and 2 walking, out of
`KF_MAX_DIRTY_RECTS` = 8. Three bars plus their labels redrawn every frame
would add rectangles on every frame forever, and past 8 the framebuffer
collapses to one screen-sized box and re-transfers ~31ms against a 33.3ms
budget. On desktop that is invisible; on the device it is the whole budget.

Needs decay over minutes, so the *displayed* value changes far more slowly than
the frame rate. **Redraw a bar only when its rendered appearance would
actually differ** — quantise the need to the bar's pixel width first and
compare that, not the underlying value. A bar 60px wide changes at most 60
times over a full decay. `g_drawn_poops` no longer exists: the mess now uses
individual retained-scene box objects (`g_poop_id[]` in
`kf_creature_screen.cpp`), one per poop, with `kf_scene_commit()`'s differ
(ADR 0040) doing the "draw only if changed" work automatically once a bar is
declared as a scene object. Prefer that same approach here — declare each bar
as a scene object and let the differ skip unchanged frames — rather than
hand-rolling a dirty flag.

**The band already has an occupant.** The care guide sits at y=286
(`kGuideTextY`). Stats and guide must coexist in 240x60, or the guide moves.
Note the guide is *more* useful on hardware than on desktop — the board's seven
buttons are unlabelled tactile switches — so do not simply delete it. If both
genuinely do not fit legibly, say so and propose rather than silently dropping
one.

**Entry repaint.** `kf_creature_screen_enter()` repaints the whole 240x320
panel, which wipes the band. Whatever tracks "what is currently drawn" must
reset there, exactly as the mess tracking had to. That bug has been made twice
on this branch already.

- [x] **Step 1: Write the failing test** — assert the band is drawn, that a
      steady frame with unchanged needs costs no extra rectangles, and that a
      changed need does redraw. A test that only checks "something was drawn"
      will pass against code that redraws every frame, which is the failure
      this task must prevent.
- [x] **Step 2: Confirm it fails for the reason you expect.**
- [x] **Step 3: Implement**, quantising before comparing.
- [x] **Step 4:** `cmake --build build -j8 && ctest --test-dir build` — must not
      regress, and `run_creature_screen_check()`'s existing `worst_rects <= 2`
      assertion must still pass **unchanged**.
- [x] **Step 5:** Verify the ESP32 target still builds (`-DKF_PANEL=ili9341`).
- [x] **Step 6:** Commit. Done: `fe06c8b`, `4fbc9fa`.

**How you would know it worked on hardware:** `python3 tools/kf_panel.py`
shows live hunger/happiness/energy in its readout; the band on the panel should
agree with it. A KFDBG `shot` gives a PNG to compare against the simulator's
own band — they are the same code, so they should be pixel-identical for the
same pet state.
