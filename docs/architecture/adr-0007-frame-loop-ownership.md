# ADR 0007: The backend owns the loop, not the core

**Status:** Accepted, 2026-08-04
**Reversal cost:** Very high if wrong; free if right on day one.

## Problem

The obvious thing to write is:

```cpp
int main() { init(); while (running) { frame(); } shutdown(); }
```

It works on desktop. It does not work in a browser. Emscripten cannot run a
blocking loop without ASYNCIFY, which bloats and slows the build; it needs
`emscripten_set_main_loop`. On the device the natural shape is a FreeRTOS
task, which is a third arrangement again.

By the time a WASM build is attempted, a core-owned loop will own state, and
inverting it means unpicking all of it.

## Decision

Core exposes `kf_app_init()`, `kf_app_frame()`, `kf_app_shutdown()`. There is
no `while` anywhere in `hakoniwaos/`. Each backend drives it:

```
desktop   while (kf_app_frame()) { }
WASM      emscripten_set_main_loop(kf_app_frame, 0, 1);
ESP32     a FreeRTOS task calling kf_app_frame()
CI        for (i = 0; i < n; i++) kf_app_frame();
```

`kf_app_frame()` returns false when the app should stop.

## Consequences

- Deterministic testing falls out for free: the headless runner calls it a
  fixed number of times.
- Frame pacing is core's decision (it calls `kf_time_delay_us()` with whatever
  is left of the budget), but the headless backend switches the delay off so a
  300-frame run takes milliseconds. The pacing *policy* stays in core; only
  the sleeping is backend behaviour.
- Under Emscripten `kf_time_delay_us()` must become a no-op, because the
  browser owns the schedule. Noted in `kf/hal/time.h`.
