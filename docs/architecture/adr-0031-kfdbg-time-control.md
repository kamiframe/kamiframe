# ADR 0031: Time control over KFDBG, and splitting the pet session's debug flag in two

**Status:** Accepted
**Date:** 2026-08-08

## Context

Measured against this codebase's default config: an egg lasts 1 hour and
deliberately does not decay at all; after hatching, hunger decays at 1042
mp/hour (**four real days** from full to empty), happiness six days,
energy eight. Correct for a shipped pet, useless for anyone watching a
real device -- ADR 0030's `KFDBG STATE` can already report hunger dropping
by a few millipercent every ten seconds, but nobody is going to sit at a
serial monitor for four days to see it reach zero, or a full week to see
every life stage. The desktop simulator has never had this problem: `simulator/
src/sdl/sdl_debug_window.cpp` has had time multipliers (1x-256x), skip-
hour/day/week buttons, and a reset button since before there was hardware
to compare against. Hardware had nothing until this ADR.

The functions those buttons call already existed and already had exactly
the right shape for this: `simulator/src/pet/kf_pet_session.h` declares
`kf_pet_session_debug_advance()`, `_reset()`, `_age_seconds()`, and
`_seek()`, all four gated behind one macro,
`KF_PET_SESSION_ENABLE_DEBUG_TOOLS`, which `ports/esp32/main/CMakeLists.txt`
set to 0. That header gave two reasons: not called by the ESP32 build
(true when written, and no longer true after this ADR), and the
2048-entry snapshot ring backing `_seek()` costs 200KB+ of static RAM
(still true, and still a real reason to keep `_seek()` off a device with
512KB of internal SRAM). Reason (b) applies only to `_seek()` -- `_advance()`,
`_reset()`, and `_age_seconds()` are thin wrappers over
`kf_pet_advance()`/`kf_pet_init()`, functions the gameplay path already
links in, with no static memory of their own.

## Decision

### 1. Split `KF_PET_SESSION_ENABLE_DEBUG_TOOLS` into two flags, not widen it

`simulator/src/pet/kf_pet_session.{h,cpp}` now define two macros:

- **`KF_PET_SESSION_ENABLE_DEBUG_CONTROLS`** gates `_debug_advance()`,
  `_debug_reset()`, `_debug_age_seconds()` -- the cheap trio.
- **`KF_PET_SESSION_ENABLE_DEBUG_TOOLS`** keeps its existing name and, now,
  a narrower meaning: ONLY the scrubbable-timeline snapshot ring and
  `_debug_seek()`, the genuinely expensive part.

Both default to 1 (desktop/headless get everything, unchanged).
`ports/esp32/main/CMakeLists.txt` now sets `KF_PET_SESSION_ENABLE_DEBUG_
CONTROLS=1` (new) alongside the pre-existing `KF_PET_SESSION_ENABLE_DEBUG_
TOOLS=0` (unchanged). Every comment in both files that explained the old
single-flag reasoning ("not called by the ESP32 build", "these four") was
rewritten -- leaving them as-is would have made them lies about a header
that now ships a real ESP32 caller for three of its four DEBUG ONLY
functions.

Widening the single flag instead of splitting it was the alternative
considered and rejected: it would have dragged the 200KB+ ring onto the
device to get three cheap functions, exactly the tradeoff `ports/esp32/
main/CMakeLists.txt`'s original comment was written to avoid. A build
config with the ring on and the cheap trio off was never useful (nothing
reads the ring without also wanting `_age_seconds()`), so the split is
two independent flags rather than a tri-state single one -- simpler, and
every existing single-flag call site (`#if KF_PET_SESSION_ENABLE_DEBUG_
TOOLS`) still means exactly what it always meant for the ring/`_seek()`
half, only now not for the other three.

### 2. Three new KFDBG commands, same shape as every existing one

`ports/esp32/main/kf_dbg_bridge.cpp` adds three command handlers,
following the exact pattern ADR 0030 established (parse in
`process_command_line()`, handler builds decoded content, hand it to
`kf_dbg_enqueue_reply()`):

- **`KFDBG ADVANCE <seconds>`** -> `ack`. Calls
  `kf_pet_session_debug_advance()` directly -- the same bounded-loop
  `kf_pet_advance()` call offline fast-forward already relies on (ADR
  0021), so this is not a new, less-tested code path.
- **`KFDBG RESET`** -> `ack`. Calls `kf_pet_session_debug_reset()` -- a
  fresh egg in place, without touching whatever is on the device's NVS
  until the next normal checkpoint overwrites it.
- **`KFDBG MULT <n>`** -> `ack`, or `err` if `n` is outside `1..256` --
  the same range `sdl_debug_window.cpp`'s multiplier buttons cover (1x
  through 256x). Sets a multiplier applied to the pet session's per-frame
  delta only (see #3).

`KFDBG STATE`'s JSON reply gains two fields: `time_multiplier` (the
current `KFDBG MULT` value) and `pet_age_s` (`kf_pet_session_debug_age_
seconds()` -- the pet's own lifetime clock, not `stage_elapsed_s`, which
resets at every stage transition), so a client can display both without a
separate round trip.

None of the three are gated behind `KF_DBG_INPUT_INJECT_ENABLE` (ADR
0030's narrower flag for button-injection specifically) -- they are
time-scale and reset controls, not button injection, so they stay
available whenever the bridge as a whole is on, the same as the existing
read-only `PING`/`SHOT`/`STATE`. A build that sets `KF_DBG_BRIDGE_ENABLE=0`
loses all three along with everything else the bridge does; `KF_PET_
SESSION_ENABLE_DEBUG_CONTROLS` staying on in that configuration costs
nothing by itself, it simply has no caller left.

### 3. `MULT` scales only the pet session's delta, mirroring `sdl_main.cpp` exactly

`app_main.cpp`'s frame loop used to call `kf_pet_session_frame(0)` --
0 meaning "track real elapsed time yourself." That convention still holds
for `kf_lvgl_port_pump(0)` and `kf_lua_port_frame(0)`, but the pet session
call now computes its own `real_dt_ms` (via `kf_time_mono_us()`, same
technique `sdl_main.cpp` already used) and passes `real_dt_ms *
kf_dbg_time_multiplier()` instead. This is a straight port of
`sdl_main.cpp`'s existing `kf_pet_session_frame(real_dt_ms * multiplier)`
call, including the correctness trap its comment already documents and
this port had to avoid identically: `kf_pet_session_frame()` only updates
its *internal* last-call timestamp on the `dt_ms == 0` path, so
interleaving 0 and non-zero calls across frames would leave that
timestamp stale and double-count the next 0-argument call. Passing a
computed, non-zero value every frame (never falling back to 0) sidesteps
that entirely, on both backends.

Deliberately NOT scaled: LVGL's tick and Lua's frame delta. Scaling
either would make animation playback speed or script frame-rate semantics
change with the pet-time multiplier, which is not what `MULT` means --
see `sdl_main.cpp`'s own comment on this, which this ADR's implementation
follows rather than re-derives.

`kf_dbg_time_multiplier()` (new, `kf_dbg_bridge.{h,cpp}`) is a plain,
unlocked global read once per frame by `app_main.cpp`'s loop, written by
`handle_mult()` inside `kf_dbg_bridge_frame()`, both on the main
frame-loop thread, the same single-thread reasoning `kf_dbg_input_mask()`
already established for the button-injection state (ADR 0030). It is not
gated behind `KF_DBG_INPUT_INJECT_ENABLE` for the same reason `handle_
mult()` above is not. When the bridge as a whole is off
(`KF_DBG_BRIDGE_ENABLE=0`), `kf_dbg_time_multiplier()` returns 1 (real
time, unscaled) rather than 0 -- "behaves exactly as if this feature did
not exist," not "the pet freezes."

### Superseded in part

The claim in "Decision" #2 that `ADVANCE`/`RESET`/`MULT` "stay available
whenever the bridge as a whole is on" was true when written and is no
longer true as of ADR 0035: all three are now gated by `KF_DBG_MUTATE_
ENABLE`, a new flag nested inside `KF_DBG_BRIDGE_ENABLE` that gates every
KFDBG command changing the pet or the simulation, not only these three.
The reasoning that led here was sound and is not itself wrong -- these
commands genuinely are not button injection, so `KF_DBG_INPUT_INJECT_
ENABLE` was correctly judged the wrong flag for them -- but stopping at
"therefore ungated" left them reachable with nothing more than the
whole-bridge switch, which is exactly the gap ADR 0035 closes. The claim
in "Decision" #3 that `kf_dbg_time_multiplier()` "is not gated behind
`KF_DBG_INPUT_INJECT_ENABLE` for the same reason `handle_mult()` above is
not" is still accurate as far as it goes (it remains true that neither is
gated by that specific flag); what changed is that both are now gated by
`KF_DBG_MUTATE_ENABLE` instead.

## What this slice does NOT reach

- **No GUI.** `tools/kf_panel.py` is owned by a separate, concurrent task;
  this ADR only fixes the wire syntax (`KFDBG ADVANCE <seconds>`, `KFDBG
  RESET`, `KFDBG MULT <n>`) for that task to wire up later.
- **`_debug_seek()` and the scrubbable timeline remain desktop/headless
  only.** Nothing in this ADR changes that; see "Decision" #1.
- **`ports/esp32/hal/esp_display.cpp`, `kf_panel_profile.h`, and
  `simulator/src/lvgl/` are untouched**, per this task's explicit
  boundary -- concurrent work owns those.

## Verified

- **`idf.py build` clean for esp32s3 against ESP-IDF v6.0.2, zero warnings
  under `-Wall -Wextra -Werror`**, in two configurations, each a full
  `set-target esp32s3` + `build`:
  1. Default. `kamiframe-firmware.bin`: **648,903 bytes** total image
     (0x9e740 bin, 649,024 bytes, 38% of the 1MB app partition free).
     Internal RAM (DIRAM): **88,213 bytes** (25.81%), identical down to
     the byte to the pre-change baseline below -- see next bullet.
  2. `KF_DBG_BRIDGE_ENABLE=0` (the whole bridge compiled out, including
     the three new commands). Builds clean, zero warnings.
- **Flash/RAM delta against the pre-this-slice baseline**, measured by
  `git stash`-ing every change in this slice, rebuilding, then `git stash
  pop` and rebuilding again (same build tree, same toolchain invocation):

  | | Before | After | Delta |
  |---|---|---|---|
  | Total image size | 647,839 B | 648,903 B | **+1,064 B (+0.16%)** |
  | Flash Code (`.text`) | 436,682 B | 437,314 B | +632 B |
  | Flash Data (`.rodata` etc.) | 123,500 B | 123,932 B | +432 B |
  | DIRAM (static RAM) | 88,213 B | 88,213 B | **+0 B** |
  | &nbsp;&nbsp;`.bss` | 16,976 B | 16,976 B | +0 B |
  | &nbsp;&nbsp;`.data` | 14,394 B | 14,394 B | +0 B |
  | IRAM | 16,384 B (100%) | 16,384 B (100%) | +0 B |

  Static RAM did not grow at all -- measured, not assumed, per this
  task's own instruction. `nm --size-sort -S` on the linked ELF confirms
  why: the only new global, `g_time_multiplier` (4 bytes, `.data`), fit
  inside existing section alignment padding, and grepping the same ELF
  for `debug_snapshot` finds nothing -- the 200KB+ ring stays absent from
  the device build, exactly as `KF_PET_SESSION_ENABLE_DEBUG_TOOLS=0`
  intends. All +1,064 bytes landed in flash (new command-handling code
  and string literals), none in RAM.
- **Desktop build unaffected.** From the repo root: `cmake -B
  build-desktop -DCMAKE_BUILD_TYPE=RelWithDebInfo
  -DKAMIFRAME_WARNINGS_AS_ERRORS=ON`, then `cmake --build build-desktop
  --target kamiframe-headless --parallel` -- zero compiler warnings (one
  pre-existing, unrelated linker note about duplicate static libraries,
  present before this slice too). `ctest --test-dir build-desktop
  --output-on-failure`: **13/13 tests pass**, including the ones that
  exercise `kf_pet_session_debug_*()` indirectly via `kamiframe_lvgl_port`
  (`pet_screen_check`, `screen_nav_check`), confirming the flag split did
  not disturb the desktop path where both flags still default to 1.
  `kamiframe-sim` (the SDL target) also built clean as a bonus check,
  though it is not the pass/fail bar per this task's own instruction.

## Not verified

- **Nothing in this ADR has run against real hardware** -- no board is
  reachable from this environment (CLAUDE.md's "no hardware yet"
  architecture rule, and this task's own instruction to say so
  explicitly). Every claim above is a clean cross-compile, a static
  symbol-table check, or a desktop test run -- never a `KFDBG ADVANCE`
  actually sent over a physical UART to a device actually fast-forwarding
  a pet.
- **The exact wire behaviour of `KFDBG MULT` at the extremes (1 and 256)
  has not been watched end to end on a device** -- the range check and
  the multiplication itself are simple enough that a bug seems unlikely,
  but "seems unlikely" is not the same standard the rest of this ADR
  holds itself to for the parts that could be checked without hardware.
- **`tools/kf_panel.py` does not yet expose these three commands.** That
  is explicitly out of scope for this task (a concurrent task owns that
  file) -- see "What this slice does NOT reach."

## Cost to change

**Adding a fourth `KFDBG` time-control command:** identical shape to
`ADVANCE`/`RESET`/`MULT` above -- one `else if` in `process_command_line()`,
one `handle_*()` following the existing pattern. If it needs a new
`kf_pet_session_debug_*()` function, add it to the `KF_PET_SESSION_
ENABLE_DEBUG_CONTROLS` block in `kf_pet_session.{h,cpp}` if it is cheap
(no new static memory), or the `KF_PET_SESSION_ENABLE_DEBUG_TOOLS` block
if it needs the snapshot ring -- the split in this ADR is exactly the
seam to extend along.

**Un-splitting the two flags back into one:** would mean either dragging
the 200KB+ ring onto every backend that wants the cheap trio (reintroducing
the exact cost this ADR avoids), or dropping `_debug_seek()`/the timeline
scrubber from desktop -- neither is a one-line change, both are real
regressions for one backend or the other. Cost to change is therefore
"don't," not "cheap."

**Scaling LVGL's tick or Lua's frame delta by `MULT` too:** one-line
change at each of the two `(0)` call sites in `app_main.cpp`'s loop (swap
in `real_dt_ms * multiplier`), but see "Decision" #3 for why this was
deliberately not done -- doing it anyway makes animation playback speed
and script frame-rate semantics scale with pet time, which is a different
feature than the one requested here.
