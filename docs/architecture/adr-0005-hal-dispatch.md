# ADR 0005: Compile-time backend selection

**Status:** Accepted, 2026-08-04
**Reversal cost:** Medium.

## Decision

**Functions are chosen at link time; capabilities are queried at runtime.**

There is one set of HAL headers. Which backend implements them is decided by
which sources a CMake target links. A missing implementation is a link error,
which is exactly the right failure.

Things that legitimately vary at runtime (screen size, pixel format, whether
partial update works, is there a battery) live in a struct you query.

Considered and rejected: a runtime vtable of function pointers (blocks
inlining, and no device ever needs two display backends at once); CRTP or
templates (forces everything into headers and breaks the C-compatible boundary
from ADR 0001); weak symbols with no-op defaults (a silently absent backend is
the worst possible failure mode); `dlopen` plugins (solves a problem we do not
have).

## The testability objection, and the answer

You cannot have a mock backend and the real backend in one binary. You do not
need to. The test binary is a separate CMake target linking different sources,
using the identical mechanism.

`kamiframe-headless` is exactly this, and it is why it exists: it proves the
swap mechanism works *before* the ESP32 backend depends on it. Both binaries
link the same `hakoniwaos` library, with the same arenas, the same blitter and
the same frame loop. Only the bottom layer differs.
