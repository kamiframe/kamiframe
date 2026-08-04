# ADR 0006: Where the device's limits are enforced

**Status:** Accepted, 2026-08-04
**Reversal cost:** Medium, and rising every month it is deferred.

## Requirement

The device's real constraints must hold in the desktop build from the first
commit, somewhere they cannot be accidentally bypassed. Desktop is fast and
roomy and will otherwise lie about every one of them.

That requirement rules out most of the obvious answers: documentation and code
review are bypassed by forgetting, and `assert()` under `NDEBUG` is bypassed by
building release, which is precisely when it stops being noticed.

## Decision: five layers, with the allocator as the spine

**1. One file holds every number.** `kf/budget.h`. Screen dimensions, pixel
format, pool sizes, arena sizes, the Lua cap, the asset budget, target frame
time, and the assumed SPI clock. Both builds compile against it. Changing a
limit is a one-line diff in a file whose header comment says not to, visible
in review forever.

**2. Memory is the enforcement.** Core never calls `malloc`. It allocates from
fixed arenas whose sizes come from `budget.h`, and exhaustion is a hard,
loud, immediate failure on both targets. See ADR 0008.
`#pragma GCC poison malloc calloc realloc free` in `kf/poison.h`, included last
in every core source file, makes accidental use a compile error under GCC and
Clang. MSVC ignores it, which is the main reason CI has a Linux GCC job.
`tools/check_no_heap.py` covers what the pragma cannot see (global
`operator new`, and MSVC) and runs in CI.

**3. The framebuffer's type is the constraint.** Core owns a single buffer
sized from `budget.h`, and `present()` accepts exactly that. There is no path
to a larger surface because the type for one does not exist.

**4. Compile-time arithmetic.** `static_assert` in `budget.h` fails the build
if the arenas do not fit the pools. A budget that cannot exist on hardware
does not compile.

**5. Frame time measured every frame, in every build configuration.** Not a
debug feature. Exceeding budget increments a counter and logs; CI can make it
fatal.

## The layer that makes the rest stick

**There is no relaxed mode, and there will not be one.** No
`--simulator-unlimited` flag, no build option that raises a limit. To
experiment beyond the device, edit `budget.h`, and the diff is in the history.
Explicit and visible beats configurable and forgotten.

## Verified

Each layer was tested by deliberately breaking it before this was accepted:

- Shrinking the scratch arena produced a panic naming the arena, the request,
  the capacity and the shortfall, exiting non-zero.
- Shrinking the internal pool below the arena total failed the build with the
  `static_assert` message.
- Adding a `malloc` call to a core source file failed to compile with
  "attempt to use poisoned malloc".

## Accepted cost

Hard enforcement is occasionally a nuisance: an oversized test asset, a debug
build that wants more room. The friction is the feature and the escape hatch
is one tracked edit away. If it starts costing hours rather than minutes, the
fix is better error messages, not a bypass.

## Later

A constraint HUD in the simulator (frame ms, arena high-water marks, Lua heap,
dirty-rect percentage as an overlay) needs font rendering, so it is a later
slice. Until then the numbers go to the console once a second and into the
window title every 15 frames.
