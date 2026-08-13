# ADR 0058: One shared frame loop, not two hand-maintained copies

**Status:** Accepted
**Date:** 2026-08-13

## Context

`simulator/src/sdl/sdl_main.cpp` (desktop) and `ports/esp32/main/app_main.cpp`
(device) each independently implemented the same per-frame sequence: compute
this frame's real elapsed time, apply the debug time multiplier, tick the
pet session, tick screen nav, pump LVGL, run one Lua frame, commit the
scene. About forty ordering-sensitive lines, written twice, maintained by
hand.

**That duplication already produced a real, expensive bug.** On 2026-08-12
"the multiplier must drag the wall clock with it, not just the pet's own
delta" was fixed — in `sdl_main.cpp` only. `app_main.cpp` kept scaling just
the pet delta, and its own comment *claimed* it mirrored `sdl_main.cpp`
while doing something different. The symptom did not appear during the
multiplied session; it appeared one power cycle later, as an offline
fast-forward that under-credited elapsed time: `kf_pet_advance()` had
carried `last_advanced` ahead of the DS3231 by `(multiplier − 1) × real
time`, so the next boot's `now − last_advanced` computed too little. Chris
measured a 3h31m unplugged gap ageing the pet only 2h54m, and it read as a
broken fast-forward when the fast-forward was fine. Fixed in `d1ab8a7`, and
`app_main.cpp`'s own header comment on the multiplier now names the defect
explicitly rather than letting a future reader trust the old claim.

CLAUDE.md's own operator rules diagnose the general shape of this class of
mistake: a comment asserting two files match, verified against neither.
This task's brief goes further and asks the harder question directly —
*kill the class of bug*, not only this instance of it.

## Decision

### Where the boundary sits, and why here rather than a smaller cut

The smallest fix that would have caught the exact bug above is extracting
only the time handling — the sub-second carry, the wall-clock drag, the
multiplied pet delta. That is real, low-risk, and sufficient for the one
defect already found.

**This task draws the boundary wider: the entire post-`kf_app_frame()`
sequence** — time handling, `kf_pet_session_frame()`, `kf_screen_nav_frame()`,
the conditional `kf_lvgl_port_pump()`, `kf_lua_port_frame()`, and the
guarded `kf_scene_commit()` — lives in one new function,
`kf_frame_loop_run()` (`simulator/src/pet/kf_frame_loop.h`/`.cpp`).

The reasoning for the wider cut: every one of those calls is
**ordering-sensitive**, and both original files carried near-identical
comments explaining why — the pet session must apply this frame's elapsed
time before the active screen reads it; `kf_scene_commit()` must follow
Lua's own frame so a script's draw calls land before the scene is diffed;
the multiplier must not leak into `kf_screen_nav_frame()`'s or
`kf_lua_port_frame()`'s real-time delta. A narrower extraction leaves that
whole ordering contract sitting in two files, each free to drift on its
own — exactly the shape that produced the bug this ADR exists to fix, just
with a smaller blast radius per drift. Chasing "the time bug" alone would
have left the *next* ordering mistake (a stray reorder of
`kf_screen_nav_frame()` and `kf_lvgl_port_pump()`, say) just as invisible
until it, too, cost a hardware power cycle to notice.

### What stayed port-specific, and why

Four things are real differences, not drift, and moving them into the
shared function would have forced `#ifdef` soup into code that has none
today:

- **`kf_dbg_bridge_frame()`** (device only) runs *before* `kf_app_frame()`,
  specifically so a queued `KFDBG BTN` command affects the input poll
  `kf_app_frame()` does first thing — earlier than `kf_frame_loop_run()` is
  ever reached. It stays in `app_main.cpp`'s own loop, outside the call.
- **The debug window's own frame** (`kf_sdl_debug_window_frame()`, desktop
  only) is threaded through as one hook, `kf_frame_loop_hooks::
  after_pet_session`, called in the exact slot it always ran from — after
  `kf_pet_session_frame()`, before `kf_screen_nav_frame()`. The device
  passes `nullptr`: it has nothing to plug into that slot.
- **Timing the post-frame segment** (`kf_app_post_frame_us()`, device only)
  is measured from *outside* the shared call — `app_main.cpp` brackets its
  own `kf_frame_loop_run()` call with two `kf_time_mono_us()` reads. Nothing
  inside the shared function needs to know that measurement exists.
- **`--frames N`, the window title, and shutdown ordering** (desktop only)
  are not part of the per-frame sequence at all and were never touched.

`KF_ENABLE_LVGL`'s conditional pump call moved into the shared function
as-is: it is not a *difference* between the two backends, it is identical
code guarded by the identical macro in both, and consolidating it removes
duplication rather than adding a special case.

### Where the shared code lives

`simulator/src/pet/kf_frame_loop.cpp`, following the precedent
`kf_pet_session.cpp` and `kf_screen_nav.cpp` already set rather than
inventing a new mechanism: compiled into the desktop build as part of the
`kamiframe_screen_port` library (`simulator/CMakeLists.txt`), and compiled
a second time, by relative path, straight into `ports/esp32/main`'s own
`SRCS` list (`ports/esp32/main/CMakeLists.txt`) — one canonical source, two
translation units, no fork. The directory name is a known awkwardness
(`simulator/` compiled directly into the ESP32 firmware) that CLAUDE.md
already accepts for this pattern; `kf_frame_loop.h`'s own header comment
repeats the pointer so the next reader is not confused by a new file
appearing there.

The one piece of CMake wiring this needed that the precedent files did not:
`kamiframe_screen_port` did not previously depend on `kamiframe_lvgl_port`
(desktop's `#ifdef KF_ENABLE_LVGL` block calls `kf_lvgl_port_pump()`, which
needs that target's real `INCLUDE_DIRS` — `kf_lvgl_port.h` pulls in
`<lvgl.h>` for its `lv_group_t` return type, not just the symbol at final
link time). `simulator/CMakeLists.txt` now links `kamiframe_lvgl_port` into
`kamiframe_screen_port` under `if(KF_ENABLE_LVGL)`, mirroring
`ports/esp32/main/CMakeLists.txt`'s own conditional `REQUIRES
kamiframe_lvgl_port`, added years earlier for the identical reason on the
device side. Not circular: `kamiframe_lvgl_port` depends on
`kamiframe_pet_port`, never on `kamiframe_screen_port`.

### Is `headless_main.cpp` a third copy? No.

`simulator/src/headless/headless_main.cpp` calls `kf_pet_session_frame()`/
`kf_screen_nav_frame()`/`kf_lua_port_frame()`/`kf_scene_commit()` directly
in dozens of places, but reading those call sites (not just their names)
shows they are not a third implementation of the same sequence:

- They never include the debug-multiplier wall-clock-drag logic at all —
  headless pins the wall clock (`kf_host_time_set_wall_fixed()`) and
  advances it explicitly where a test needs to, rather than living inside a
  running per-frame loop.
- Every dt is an explicit, synthetic, fixed value
  (`kf_pet_session_frame(kFixedDtMs)`), never `real_dt_ms` computed from
  `kf_time_mono_us()` — headless exists specifically so a check takes
  milliseconds, not real minutes (`kf_host_time_set_realtime(false)`).
- Different checks call different SUBSETS of the sequence, deliberately —
  some drive only `kf_pet_session_frame()` in a loop with no screen or Lua
  involved at all; others drive `kf_pet_session_frame()` +
  `kf_screen_nav_frame()` with no scene commit; the golden-checksum checks
  (`headless_determinism`, `headless_fullscreen`, `screen_parity_check`)
  drive the fuller sequence but through `kf_app_frame()`'s own loop
  (`main()`'s default path), not a hand-rolled restatement of it.
- The bug this ADR exists to fix — a multiplier or a bridge command
  reaching one clock and not another — has no headless equivalent to drift
  from: there is no multiplier and no bridge in a deterministic test
  harness.

`headless_main.cpp` is many small, deliberately-varying test drivers, not
one port's real frame loop. Forcing it onto `kf_frame_loop_run()` would
mean adding parameters or hooks to make a test-only function general enough
for every one of those variations — the "genuinely different, not drift"
case this task's brief explicitly warns against flattening. It was left
alone.

### A drift already found, and fixed in passing

While reading both originals to write `kf_frame_loop_run()`, one more
divergence surfaced beyond the multiplier bug this task was scoped to fix:
`sdl_main.cpp`'s wall-clock-set never checked `wall.valid` before calling
`kf_time_set_wall()`; `app_main.cpp`'s did. Harmless on desktop today —
`simulator/src/host/host_time.cpp`'s `kf_time_init()` always leaves the
simulated wall clock valid before the frame loop can run a single
iteration — but it was still a latent gap: had that ever not been true,
`sdl_main.cpp` would have computed a wall-clock value from a garbage `0`
`epoch_seconds`. The shared implementation standardises on the device's
more defensive check, which costs desktop nothing and removes the gap
everywhere at once rather than leaving it for the next reader to notice.

## Consequences

- The exact bug this ADR describes — one backend's multiplier reaching the
  pet's clock but not the wall clock — is now structurally impossible to
  reintroduce in only one of two copies, because there is only one copy.
  A future ordering mistake in the shared sequence (the wider cut this ADR
  chose to also cover) is likewise a single edit, not two independently
  drifting ones.
- `kf_frame_loop_run()` owns two pieces of per-process state (the previous
  frame's monotonic timestamp, and the wall-clock carry) that used to be a
  local `static` inside each port's `main()`/`app_main()`. Correct for
  exactly the reason those statics existed: one frame loop runs per
  process.
- `simulator/CMakeLists.txt`'s `kamiframe_screen_port` now conditionally
  depends on `kamiframe_lvgl_port` under `-DKF_ENABLE_LVGL=ON`, a new edge
  in the desktop build graph that did not exist before this task (mirroring
  one the ESP32 side already had).
- The next reader extending either port's loop has one function to read,
  with the ordering reasoning attached to it, instead of two files whose
  comments claim to agree.

## What was verified before writing this, and what was not

- **Verified:** `ctest --test-dir build` was 53/53 at `14d350c` before any
  change in this task, confirmed by running it. Both Core scanners
  (`check_no_heap.py`, `check_no_float.py`) report non-zero file counts
  after this task (39 and 40 files respectively) — Core itself is
  untouched, as expected for a port-layer refactor. The full suite is
  54/54 after this task (53 + the one new check below), including the
  three checksum-comparing checks this task leaned on as its strongest
  evidence (`headless_determinism`, `headless_fullscreen`,
  `screen_parity_check`) — all green, golden checksums unchanged, meaning
  the extraction changed no observable rendering output. Read, not
  assumed: `headless_main.cpp`'s dozens of direct `kf_pet_session_frame()`/
  `kf_screen_nav_frame()`/`kf_lua_port_frame()` call sites, to reach the
  "not a third copy" verdict above rather than trusting the file's size or
  its superficial resemblance to a frame loop. Read, not assumed:
  `simulator/src/host/host_time.cpp`'s `kf_time_init()`, to confirm the
  `wall.valid` gap found above is real but currently harmless on desktop,
  not merely theorised. The one new assertion
  (`run_frame_loop_multiplier_check()`, `--verify-frame-loop-multiplier`)
  was proven non-vacuous by deliberately disabling the wall-clock-drag
  block (`if (false && multiplier > 1u)`) and confirming it fails with
  exactly the message naming the defect — *"the wall clock advanced by
  roughly real_dt_ms * (multiplier - 1) seconds -- the multiplier drags the
  wall clock, not just the pet's delta"* — then reverting before commit.
  ESP32 compiles clean (`ports/esp32`, `-DKF_PANEL=ili9341`):
  `kamiframe-firmware.bin` is 583,328 bytes (0x8e6a0), 63% of the 1.5MB app
  partition free — no measurable size change from this refactor.
- **Not verified:** this refactor was not flashed to hardware — Chris
  flashes, per this task's own instructions. The device-side behaviour
  rests on the ESP32 build compiling against the identical source the
  desktop build already exercises through `ctest`, not on a bench
  observation.
