# ADR 0008: Fixed arenas, no heap in core

**Status:** Accepted, 2026-08-04
**Reversal cost:** Very high.

## Why the obvious approach fails silently

The ESP32-S3 has two pools with genuinely different properties:

- **~512KB internal SRAM.** Fast, DMA-capable, and where the framebuffer and
  DMA buffers must live. Scarce.
- **8MB external PSRAM** over octal SPI. Roomy, noticeably slower, real
  restrictions on DMA.

A desktop has one undifferentiated, effectively infinite heap. So if core
calls `malloc()`, nothing on desktop will ever tell you which pool a buffer
would have landed in, that the small fast pool is exhausted, or that a buffer
needing DMA would not have been capable of it.

This is the "desktop lies" problem applied to memory, and it is worse than the
speed version: a slow frame is visible, and a memory-topology mistake is
invisible until bring-up.

## Decision

**Named, fixed-size arenas declared in `kf/budget.h`. No `malloc` in core.**

```
KF_ARENA_FRAMEBUFFER   internal, DMA-capable   153,600 B (240*320*2)
KF_ARENA_SCRATCH       internal                 48 KB, reset every frame
KF_ARENA_LUA           PSRAM                     1 MB   (unused until Lua)
KF_ARENA_ASSETS        PSRAM                     2 MB
```

The HAL hands core one block per pool, once, at startup
(`kf/hal/memory.h`). Everything above that (bump allocation, high-water
tracking, hard failure) is core, so it behaves identically on both targets.

The desktop backend implements the pools as **static arrays**, not `malloc`.
That is the point: the only way desktop can tell you that you have outgrown
the small fast pool is to give you exactly as much as the device has.

`kf_arena_alloc()` **never returns NULL.** Exhaustion panics with the arena
name, the request, the capacity and the shortfall. A caller that can handle
failure is a caller that will quietly do less on the device than on desktop,
and then the two stop matching.

## No free

Two lifetimes exist and neither needs one. Permanent arenas live until power
off, because a device running for months must not fragment. The scratch arena
is reset at the top of every frame: allocation is a pointer bump, freeing is
free, and a leak is impossible because nothing survives the frame.

## Lua

`lua_newstate` takes an allocator function, so the Lua heap cap is exact
rather than advisory. That is the mechanism for the PSRAM cap when the Lua
slice lands.

## Cost

More friction than `malloc`. You must decide up front which arena something
belongs in, and sometimes you will be wrong. Being forced to think about it is
the point, and being wrong is visible immediately rather than in month eight.
