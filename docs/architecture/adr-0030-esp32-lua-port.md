# ADR 0030: Lua on ESP-IDF, and the demo creature driving the pet for real

**Status:** Accepted
**Date:** 2026-08-08

## Context

ADR 0014 embedded Lua and ADR 0016 built the `pet.*` binding, both
deliberately scoped to the desktop build: "`ports/esp32/` stays an empty
skeleton, nothing here claims otherwise." ADR 0025 named porting Lua onto
ESP-IDF as still-open work, expecting it to need "its own
`EXTRA_COMPONENT_DIRS` entry and its own ADR" -- the same shape ADR 0025
itself used for the pet session, and the same shape this slice uses for Lua.

Since ADR 0025, the pet session has been genuinely alive on real ESP-IDF
builds: ticking, saving to and loading from NVS, reachable only through a
10-second `KF_LOGI` summary because nothing else can read it. `kf_lua_port.h`
and the `pet.*` binding (`simulator/src/lua/`) already exist, are already
proven on desktop (ADR 0014, ADR 0016), and already know how to read and act
on exactly that pet session. The gap this slice closes is narrow: get Lua
itself building for ESP-IDF, wire the same `kf_lua_port_init()` /
`kf_lua_port_frame()` / `kf_lua_port_shutdown()` calls `sdl_main.cpp` already
makes into `app_main.cpp`, and load the demo creature script (ADR 0018) --
not write a single new line of Lua-facing C++.

LVGL is explicitly **not** part of this slice, same scoping ADR 0025 used:
it is separate work, running concurrently against this same tree, and
nothing here depends on it or blocks it. This ADR's own number reflects
that: 0027-0029 were live at the time this slice started (two mid-repair for
a duplicate-file problem elsewhere in this range, one claimed by the LVGL
slice), so this file is 0030 -- picked deliberately above every number in
use or spoken for, not the next-free slot by habit.

## Decision

### 1. A real ESP-IDF component for Lua itself, `ports/esp32/components/lua`

Unlike `kf_pet_session.cpp` (ADR 0025: "one file with one dependency
\[hakoniwaos\]... no third-party library of its own"), Lua is a real
external dependency, so it earns its own component the way `hakoniwaos`
does, per ADR 0025's own prediction. `ports/esp32/CMakeLists.txt`'s
`EXTRA_COMPONENT_DIRS` gained exactly one line pointing at it.

That component does not hand-duplicate `cmake/fetch_lua.cmake`'s file list.
It `include()`s that file directly -- CMake's `include()` runs in the
caller's own scope, not a fresh one, so `KAMIFRAME_LUA_SRCS` (Lua's
CORE_O+AUX_O+LIB_O list, already assembled with full paths by the time
`fetch_lua.cmake` finishes) and the `FetchContent_Declare`/`MakeAvailable`
call that fetched the exact pinned `v5.5.0` tag desktop already builds are
both still visible afterward. One canonical file list, one canonical tag,
same discipline this port already uses for `kf_pet_session.cpp` and every
HAL backend before it.

What is deliberately NOT reused: `fetch_lua.cmake`'s own
`add_library(lua_core ...)` target. See "`idf_component_register()`, not
`lua_core`" below for why a second, ESP-IDF-native compile of the same file
list is the correct outcome here, not an accident -- and why the leftover
`lua_core` target is marked `EXCLUDE_FROM_ALL` so its sources are not
compiled twice for nothing.

### 2. `idf_component_register()`, not `lua_core` linked in via `target_link_libraries`

The first version of this component registered an otherwise-empty component
and exposed `lua_core` to `main` via
`target_link_libraries(${COMPONENT_LIB} INTERFACE lua_core)`. That built
clean but failed to LINK, found the hard way, not guessed: `ld` reported
`undefined reference to __wrap_longjmp` from inside Lua's own `ldo.c`
(`luaD_throw`, which calls `longjmp` as part of Lua's ordinary
protected-call error handling).

The real cause: `lua_core` is a plain CMake target, not an ESP-IDF
component, so it is invisible to the machinery that decides the final link
line's archive order (the same category of machinery `../../main/
CMakeLists.txt`'s `WHOLE_ARCHIVE` comment already documents for the
`hakoniwaos`/`main` circular case). CMake's own usage-requirement
propagation instead tacked `liblua_core.a` onto the very END of the actual
link command -- after even ESP-IDF's `-Wl,--wrap=longjmp` and the
`libcxx.a` that defines `__wrap_longjmp` for it. `ld`'s archive scan is a
single left-to-right pass: by the time `ldo.c.obj` asked for
`__wrap_longjmp`, `libcxx.a` had already been scanned with nothing yet
requiring that symbol, so it was never pulled in. A real symbol, in a real
archive, simply scanned before anything needed it.

The fix: compile `KAMIFRAME_LUA_SRCS` straight into the `lua` component's
own `SRCS`, so `idf_component_register()` produces a real `liblua.a`. Then
`main REQUIRES lua` (`../../main/CMakeLists.txt`) is an ordinary ESP-IDF
component dependency, positioned by the exact same machinery that already
places `hakoniwaos` and every HAL-owning component correctly -- no special
casing needed, and none added.

### 3. `LUA_32BITS`, found by a second real build failure

Compiling `kf_lua_port.cpp` (C++) against the same `lua.h` that compiles
fine for every one of Lua's own `.c` files failed with `'lua_Integer' does
not name a type`, tracing back to `luaconf.h`'s own `#error "Compiler does
not support 'long long'..."`.

Root cause, confirmed directly rather than assumed: `luaconf.h` gates
`LUA_INTEGER` on `long long` being available, using `defined(LLONG_MAX)` as
its C99-compliance proxy. This toolchain's `<limits.h>` chain (ESP-IDF's
picolibc `limits.h` defers to GCC's own bundled fallback via
`#include_next`, because `__GNUC__` is defined) only defines `LLONG_MAX`
inside `#if defined(__STDC_VERSION__)` -- a C-only macro, never set for a
C++ translation unit. A one-line `#ifdef LLONG_MAX` probe, same flags, same
toolchain, compiles clean under `-x c` and fails under `-x c++` -- this is
a real gap in this specific toolchain/libc pairing for C++, not a Lua bug,
and not something to patch in vendored or system headers.

`luaconf.h`'s own `#error` names the documented way out: `-DLUA_32BITS`
(32-bit `lua_Integer` and `lua_Number`, sidestepping the `long long` path
that needs `LLONG_MAX` at all). This is a genuine fit for the target, not
just a workaround -- the same reasoning that already justified
`LUA_FLOAT_TYPE=LUA_FLOAT_FLOAT` for `lua_Number` (ADR 0014: matching the
S3's single-precision FPU) applies to `lua_Integer` matching this CPU's
32-bit native word size.

Applied to BOTH `lua_core`'s replacement (`${COMPONENT_LIB}` inside
`ports/esp32/components/lua/CMakeLists.txt`) and `main` (`../../main/
CMakeLists.txt`, covering `kf_lua_port.cpp`/`kf_lua_alloc.cpp`) -- not
optional symmetry. `LUA_INTEGER` crosses the real API boundary in
`lua_pushinteger()`/`luaL_checkinteger()` calls `kf_lua_port.cpp` makes
directly against Lua's compiled archive; if the two sides disagreed on
`LUA_INTEGER`'s actual width, every one of those calls would be a genuine
ABI mismatch (a 4-byte value passed where an 8-byte one is expected, or the
reverse), not just a source-level inconsistency. Not added to
`cmake/fetch_lua.cmake` itself, which desktop also includes unmodified:
desktop's g++ already defines `LLONG_MAX` correctly from a plain
`<limits.h>` include, so it has no reason to trade away 64-bit Lua
integers, and this option is a per-target width decision, not a shared one.

### 4. Two portability bugs in already-shipped, already-verified code

`-Wall -Wextra -Werror` on this toolchain caught two `-Werror=format=`
violations in `kf_lua_port.cpp` and `kf_lua_alloc.cpp` that desktop's build
never could: `%u` format specifiers paired with `uint32_t` arguments passed
unadorned. `unsigned int` and `uint32_t` are the same type on x86-64 Linux
(desktop), so this was invisible there; on this xtensa-esp-elf toolchain
`uint32_t` resolves to `long unsigned int`, a genuine mismatch. Fixed with
`static_cast<unsigned>(...)` at each call site -- the same pattern the Lua
allocator's own file already used two lines above each fix (for
`KF_ARENA_LUA_BYTES`), not a new convention invented for this slice. Three
call sites: `kf_lua_port.cpp`'s shutdown log line, and two in
`kf_lua_alloc.cpp` (the arena-ready log and the out-of-heap-on-grow
warning). No behaviour change on desktop -- `static_cast<unsigned>` on an
already-32-bit value is a no-op there.

### 5. `CMAKE_BUILD_EARLY_EXPANSION`, found by a configure-time failure

ESP-IDF evaluates every component's `CMakeLists.txt` TWICE: once in a
lightweight "requirements expansion" pass that only wants to know a
component's `REQUIRES`/`PRIV_REQUIRES`, and again for real.
`FetchContent_Declare`'s modern implementation calls `define_property()`,
which CMake refuses to run in the restricted script-mode context that first
pass uses (`"define_property command is not scriptable"`). `fetch_lua.cmake`
's `include()`, and the `target_compile_definitions`/`idf_component_register(
SRCS ...)` calls that depend on what it defines, are all guarded behind
`if(NOT CMAKE_BUILD_EARLY_EXPANSION)` -- the variable ESP-IDF sets
specifically so a component can tell the two passes apart.
`idf_component_register()` itself is always called unguarded (an empty call
during the early pass, a real one otherwise) -- it is what that first pass
is trying to read in the first place.

### 6. `app_main.cpp`: same four-call shape `sdl_main.cpp` uses, minus LVGL

`kf_lua_port_init(kKfLuaDemoCreatureScriptSource, ...)` is called once,
after `kf_pet_session_init()` and before the frame loop -- the same
ordering `sdl_main.cpp` uses and the same reason (ADR 0016's own comment on
that call): the allocator's one block comes from `KF_ARENA_LUA`, already
carved out by `kf_app_init()`'s `kf_arena_init_all()`, and the `pet.*`
binding needs the pet session ready. `kf_lua_port_frame(0)` runs once per
frame, after `kf_pet_session_frame(0)` and inside the existing
`while (kf_app_frame())` loop -- `0` meaning "track real elapsed time
yourself," the only option this backend has, same as the pet session's own
call. No LVGL step in between, unlike `sdl_main.cpp`: there is still no
screen on this build for LVGL to own, so there is nothing to sequence
against. `kf_lua_port_shutdown()` is called before `kf_pet_session_shutdown()`
, the exact reverse of init order, matching `sdl_main.cpp`'s own shutdown
sequence.

This is genuinely the demo creature script (ADR 0018,
`kf_lua_demo_creature_script.h`), not a proof script -- the same file
`sdl_main.cpp` already loads, unchanged, unforked.

## What this slice does NOT reach

- **No screen.** The demo creature's only output channel remains
  `kf.log()` -> `KF_LOGI`, same limitation ADR 0018 itself named for
  desktop before a pet screen existed there. LVGL is a separate,
  concurrently-running slice; nothing here depends on or blocks it.
- **No frame-budget accounting for Lua's own time**, same gap ADR 0014 left
  open on desktop -- unchanged by porting to a second target.
- **No hardware verification.** Everything below is `idf.py build` and
  static reasoning about the toolchain and the arena budget. Nothing in
  this slice has run on a real board.

## Verified

- Full clean rebuild (`idf.py fullclean && idf.py build`, esp32s3 target,
  ESP-IDF v6.0.2) succeeds with the standard `-Wall -Wextra -Werror` bar,
  zero warnings, confirmed by grepping the complete build log for
  `warning:` after a genuine from-scratch build (not an incremental one).
- The demo creature script (ADR 0018) is the script loaded --
  `kKfLuaDemoCreatureScriptSource`/`kKfLuaDemoCreatureScriptChunkName`, the
  exact same constants `sdl_main.cpp` uses, included by relative path, not
  copied.
- `KF_PET_SESSION_ENABLE_DEBUG_TOOLS=0` and the rest of ADR 0025's ESP32
  build configuration are untouched -- this slice only added to `main`'s
  `SRCS`/`INCLUDE_DIRS`/`REQUIRES` and one `target_compile_definitions`
  call, never removed or restructured anything there.
- `kf/budget.h` was not edited. `KF_ARENA_LUA_BYTES` (1MB, PSRAM pool) is
  the same figure the desktop build already budgets against, and
  `kf_arena_init_all()`'s compile-time-checked pool-capacity assertions
  (already unconditional core code, unchanged by this slice) passed as
  part of every one of the clean builds above.

**Not verified: anything on real hardware.** No board has run this binary.
Whether the demo creature's `kf.log()` lines actually appear over serial,
whether `pet.feed()`/`pet.play()`/`pet.rest()` genuinely reach the same live
`kf_pet_state` the periodic `KF_LOGI` summary already prints, and whether
30fps holds with Lua's frame cost folded in -- all real questions, none
answered by this slice. The next real-hardware run (after ADR 0024/ADR 0025
's own bring-up work) is what answers them.

## Cost estimate: flash and RAM

Measured via `idf.py size` (esp32s3, `-O2`-equivalent ESP-IDF release
flags), before this slice (ADR 0026's tree, HEAD at the start of this
slice) and after, both from a genuine full clean rebuild:

| | Before (no Lua) | After (Lua + demo creature) | Delta |
|---|---|---|---|
| Total image size | 286,787 B | 445,615 B | **+158,828 B (~155KB)** |
| Flash `.text` | 137,706 B | 272,794 B | +135,088 B |
| Flash `.rodata`/`.appdesc`/`.tdata` | 64,392 B | 88,132 B | +23,740 B |
| DIRAM (internal SRAM) total | 72,909 B (21.33%) | 73,101 B (21.39%) | **+192 B** |
| DIRAM `.text` | 53,923 B | 53,923 B | +0 |
| DIRAM `.data` | 14,346 B | 14,346 B | +0 |
| DIRAM `.bss` | 4,640 B | 4,832 B | +192 B |
| App partition free | 73% (0xb9f40 / 0x100000) | 57% (0x932e0 / 0x100000) | -16 pts |

The flash cost (~155KB: the Lua interpreter, its compiled-in standard
library implementations -- including the sandboxed-out `io`/`os`/`debug`/
`package`, unreachable but still compiled per ADR 0014's own "Accepted
cost" -- and the two port glue files) executes in place from flash via the
cache, the normal ESP32 model for ordinary (non-ISR) code; it is not copied
into internal SRAM. That is exactly what the DIRAM row shows: **+192 bytes**
of new internal SRAM, not +155KB -- two small file-scope state structs
(`kf_lua_port.cpp`'s `State g`, `kf_lua_alloc.cpp`'s `g_base`/`g_end`/
`g_active`), nothing else. The 1MB Lua VM heap itself lives in PSRAM via
`KF_ARENA_LUA`, not in this table's DIRAM figures at all.

**Free heap at runtime: no measured change, and the static reasoning for
why not.** `kf_arena_init_all()` (`hakoniwaos/src/arena.cpp`, shared core
code, unconditional on every target) has carved out all five arenas -- the
1MB `KF_ARENA_LUA` block included -- from `KF_POOL_EXTERNAL` (PSRAM) on
every ESP32 build since ADR 0020 first called `kf_app_init()` on this
target, whether or not anything used them. This slice changes what runs
INSIDE that already-reserved 1MB block; it does not reserve a single
additional byte of PSRAM. `kf/budget.h` was not touched, and its
`KF_ARENA_LUA_BYTES + KF_ARENA_ASSETS_BYTES + KF_ARENA_LVGL_BYTES <=
KF_POOL_PSRAM_BYTES` compile-time assertion -- unconditional, already
checked on every build -- passed identically before and after. The 592KB
app-partition headroom this slice's own build consumed (73% -> 57% free)
is flash, a completely separate budget from the 8MB PSRAM pool this
paragraph is about.

## Accepted cost

- **`LUA_32BITS` diverges ESP32's `lua_Integer`/`lua_Number` width from
  desktop's** (32-bit vs. desktop's 64-bit `lua_Integer`, both already
  32-bit `lua_Number` per ADR 0014). Every value `pet.*` and `kf.*`
  currently hand across the boundary (millipercent needs, life-stage
  indices, frame deltas, `kf.report()`'s single integer) comfortably fits
  in 32 bits, so nothing observable changes today. A future script that
  genuinely needs a 64-bit Lua integer on ESP32 would silently truncate
  instead of erroring -- worth a real bounds check in `pet.*`/`kf.*` if
  that ever becomes a real requirement, not before.
- **`lua_core`'s sources compile twice in a from-scratch ESP-IDF configure**
  -- once as the excluded-from-build `lua_core` target `fetch_lua.cmake`
  still creates (never actually compiled, since `EXCLUDE_FROM_ALL`), and
  once for real as the `lua` component's own archive. The excluded target
  costs nothing at build time; it exists only because `include()`ing
  `fetch_lua.cmake` for its `KAMIFRAME_LUA_SRCS` variable and
  `FetchContent` call unavoidably runs its `add_library()` line too, and
  editing that file to make the two halves separable was judged not worth
  touching a file the desktop build depends on unmodified, for a cost that
  is zero at actual compile time.
- **~155KB more flash**, none of it avoidable without dropping standard
  library pieces this project has already decided to keep unreached-but-
  compiled (ADR 0014's own "Accepted cost"). Against a 1MB app partition
  with 57% still free, not a binding constraint today.

## Later

- LVGL's pet screen (concurrent slice, not this one) is what turns
  `kf.log()` into something visible on-device instead of serial-only.
- Real hardware verification, once bring-up (ADR 0024) and this slice's own
  firmware meet on Chris's board.
- Folding `kf_lua_port_frame()`'s own time into `kf_app_frame()`'s budget
  report -- unchanged from ADR 0014's own "Later" list, now doubly true
  with a second target actually running it.
- Deciding whether the `LUA_32BITS` divergence ever needs a real bounds
  check at the `pet.*`/`kf.*` boundary, if a script ever needs a value
  outside 32-bit range.
