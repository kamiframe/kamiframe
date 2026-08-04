# ADR 0014: Embedding Lua

## Requirement

The SDK promise, from day one of the planning docs: write a creature in
Lua against a documented API, no C required. That means a real Lua VM,
running inside the same constraint budget as everything else in
`kf/budget.h` -- not "Lua, eventually, on a machine with more memory than
the device will ever have."

The spec sheet's own build order (`02-min-spec-sheet.md`) puts the Lua game
API third, right after the display HAL and sprite/UI layer, and before the
pet simulation framework that gets built on top of it. LVGL for menus
(ADR 0013) closed out the sprite/UI slot. This is that next slot.

## Decision: embed Lua 5.5.0, sandboxed, on an arena-backed allocator

- **Lua 5.5.0**, not 5.4. `02-min-spec-sheet.md` and
  `08-phase1-slice1-decisions.md` both framed this as "5.4 vs 5.3," because
  5.5 had not shipped when they were written -- it went stable in December
  2025. Checked before writing any code, not assumed: `lua.org/download.html`
  and the official `lua/lua` GitHub mirror both confirm 5.5.0 is current,
  with 5.4.8 (June 2025) as the last 5.4.x patch. Two things in 5.5 make it
  the better fit here, not just the newer one: `luaL_openselectedlibs`,
  which is exactly the "load only these standard libraries" primitive the
  sandboxing below needs (5.4 embedders hand-roll the equivalent loop over
  `luaL_requiref`), and "more compact arrays," which matters directly
  against a 1MB heap cap. LuaJIT stays ruled out, as already flagged: it
  does not target Xtensa or RISC-V, so using it on desktop would create
  the exact two-codebase failure this whole architecture exists to
  prevent.
- **`LUA_FLOAT_TYPE=LUA_FLOAT_FLOAT`** (single-precision `lua_Number`),
  matching the S3's single-precision FPU -- the same reasoning already
  applied to the simulation's own maths (`08-phase1-slice1-decisions.md`'s
  "fixed-point vs float" note: doubles are emulated in software there and
  slow). Set via `luaconf.h`'s documented override mechanism, a
  `target_compile_definitions` line in `cmake/fetch_lua.cmake`, not a
  source edit.
- **Sandboxed via `luaL_openselectedlibs`**: `base`, `coroutine`, `math`,
  `string`, `table`, `utf8`. Deliberately NOT `io`, `os`, `package`
  (`LUA_LOADLIBK`, i.e. `require`) or `debug` -- see "What the sandbox
  excludes, and why" below.
- **A custom allocator over one arena block**, not `kf_arena_alloc()`
  directly -- see "The allocator" below for why a bump allocator cannot
  work here at all, unlike every other arena in `kf/budget.h`.
- **Lua's hash seed comes from the entropy HAL**, not `luaL_makeseed()`'s
  own default -- see "The hash-seed trap" below.
- **Simulator-only in this slice**, the same scoping ADR 0013 already used
  for LVGL: not wired into `hakoniwaos/` (core) or `ports/esp32/`. See
  "What this slice actually builds."

## The allocator

`kf_arena_alloc()` (`kf/arena.h`) is a bump allocator with no free, correct
for the framebuffer and wrong for Lua. Lua's garbage collector allocates
and frees constantly as ordinary operation -- every short-lived string,
every table that grows -- so handing `lua_newstate` a `lua_Alloc` built
directly on `kf_arena_alloc()` would exhaust `KF_ARENA_LUA_BYTES` within a
few hundred frames of any script that so much as concatenates a string.
This was not a risk discovered by accident: `lua_Alloc`'s job description
(malloc-or-realloc-or-free depending on its arguments) makes the mismatch
obvious on inspection, before writing a line of the allocator.

The fix is the same shape LVGL already uses for `KF_ARENA_LVGL`
(`kf_lvgl_pool.cpp`): call `kf_arena_alloc()` exactly ONCE, for the whole
1MB, and run a real allocator inside that one block.
`simulator/src/lua/kf_lua_alloc.cpp` is that allocator: a boundary-tag
(header + footer) first-fit free list with immediate bidirectional
coalescing on free, plus an in-place grow path that extends into a free
right neighbour before falling back to allocate-copy-free. LVGL brings its
own (TLSF, vendored inside LVGL itself); Lua does not ship one, so this one
is hand-rolled rather than vendoring a fourth dependency for roughly 250
lines of well-understood, textbook allocator logic.

**The contract that had to be gotten right, not just "probably right":**
the Lua manual's description of `lua_Alloc` guarantees the allocator
"must not fail" when the new size is not larger than the old one --
shrinking or same-size reallocations are assumed to always succeed. An
allocator that always does allocate-new + copy + free (the obvious naive
implementation) would violate that the moment the arena is fragmented or
full, even for a SHRINKING request that needs no new memory at all --
exactly the kind of bug that would not show up in this slice's own tests,
only later as an intermittent VM corruption under real memory pressure.
`kf_lua_alloc()` therefore special-cases `want <= current_payload_bytes`
as an in-place operation over memory it already owns, which by
construction cannot fail; only the genuine-growth path can return `NULL`.
See the comment on that branch in `kf_lua_alloc.cpp` for the exact
reasoning, and `kf_lua_alloc.h`'s header comment for the contract as a
whole.

**Verified, not just reasoned about:** `kamiframe-headless --verify-lua`
(the `lua_determinism_check` ctest target) runs the proof script
(`kf_lua_proof_script.h`) for 300 frames, each of which builds and
discards a 32-entry table of fresh string concatenations, with a forced
full collection every 30th frame. A bump-only allocator would have failed
this well under 2,000 frames; manually running the same check at 5,000
frames (`kamiframe-headless --verify-lua --frames 5000`) completed clean,
with the deterministic result (`total == 32 * frames`, checked exactly,
not against a golden hash -- see the comment in
`simulator/CMakeLists.txt`) matching both a fresh run and a rerun.

## What the sandbox excludes, and why

`luaL_openselectedlibs` loads `base | coroutine | math | string | table |
utf8`. Explicitly not loaded:

| Library | Why not |
|---|---|
| `io` | Unrestricted file access. A cartridge has no filesystem of its own to see -- save state goes through `kf/hal/storage.h` when that binding exists, never raw files. |
| `os` | `os.execute`, `os.remove`, `os.getenv`, and a wall clock that bypasses `kf/hal/time.h`'s two-clock split (ADR 0004) -- every one of those is either a sandbox hole or a way for a script to silently disagree with the HAL about what time it is. |
| `package` (`LUA_LOADLIBK`, i.e. `require`) | There is no module system yet. When there is one, it will be the SDK's cartridge format, not the host filesystem `require` searches by default. |
| `debug` | Can inspect and rewrite arbitrary stack frames and upvalues belonging to OTHER functions -- precisely what a sandbox exists to prevent. Revisit only if in-cartridge tooling deliberately needs it, later, as its own decision. |

Their C implementations are still compiled into `lua_core` (Lua's own
`linit.c` references every `luaopen_*` function unconditionally, whether
or not `luaL_openselectedlibs` is told to load it), so this is not a
smaller binary, but it is a real sandbox: nothing a script does can reach
`os.execute` or `io.open` if the symbol table those functions would have
populated was never populated. `os_tmpname` in particular shows up as an
unused-`tmpnam`-is-dangerous linker warning on the Linux build -- harmless,
because nothing ever calls it; noted so a future reader does not go
looking for a bug that is not there.

## The hash-seed trap

Lua 5.5 changed `lua_newstate`'s signature to take an explicit hash seed
(`lua_newstate(lua_Alloc f, void *ud, unsigned seed)`), where earlier
versions generated one internally. `lauxlib.c`'s own convenience wrapper,
`luaL_newstate()`, seeds via `luaL_makeseed(NULL)`, whose implementation
mixes in `time(NULL)` and a local variable's stack address -- checked by
reading `lauxlib.c` directly rather than assumed from the name. That seed
affects Lua's string hash table, which affects the iteration order of
`pairs()`/`next()` over any table with string keys. Using it here would
have made that order non-deterministic run to run, in a project whose
headless tests exist specifically to catch exactly that category of bug in
everything else.

`kf_lua_port_init()` calls `kf_entropy()` (`kf/hal/entropy.h`) instead --
the identical mechanism `kf_app_init()` already uses to seed `kf/rng.h`,
for the identical reason: it can be pinned for headless/CI determinism
(`kf_host_entropy_pin()`) and left genuinely random for interactive play,
and every other source of randomness in this project already goes through
it. This was not in the original plan for this slice; it surfaced only
because upgrading to 5.5 changed `lua_newstate`'s signature and the build
broke on the missing argument, which is exactly the kind of thing worth
recording rather than quietly patching around.

## What this slice actually builds

Lives entirely under `simulator/src/lua/` and `cmake/fetch_lua.cmake`, the
same shape ADR 0013 used for LVGL and for the identical reason: this slice
does not claim Lua works on the ESP32 build. `hakoniwaos/` (core) is
untouched except for `kf/budget.h` and `kf/arena.h`'s comments, which now
say the Lua arena is in use rather than reserved for later.

What exists:

- `cmake/fetch_lua.cmake` -- vendors Lua 5.5.0 via `FetchContent`, builds
  it as a plain static library from an explicit file list (Lua ships a
  Makefile, not a CMakeLists.txt, so there is no subdirectory for
  `FetchContent_MakeAvailable` to add; that is supported, documented
  behaviour, not a workaround).
- `kf_lua_alloc.{h,cpp}` -- the allocator described above.
- `kf_lua_port.{h,cpp}` -- VM lifecycle (`kf_lua_port_init` /
  `kf_lua_port_frame` / `kf_lua_port_shutdown`), the sandboxed library set,
  and two bindings: `kf.log(msg)` (routes through `kf_log`, since the
  sandbox has no `print()` destination -- deliberately: a device with no
  console attached should not have a script-reachable escape hatch to C's
  stdio) and `kf.report(n)` (hands one integer back to C, currently
  consumed only by the determinism check).
- `kf_lua_proof_script.h` -- placeholder content, same spirit as
  `kf/demo.h` and `kf_lvgl_proof_screen.h`: proves the mechanism, is not a
  pet, gets deleted once a real cartridge format exists.
- `kamiframe-headless --verify-lua` and the `lua_determinism_check` ctest
  target.
- Wired into both desktop backends: `kf_lua_port_init()` /
  `kf_lua_port_frame(0)` (real time) / `kf_lua_port_shutdown()` in
  `sdl_main.cpp`, and the synthetic-clock equivalent in
  `headless_main.cpp`'s `run_lua_check()`.

What deliberately is not built, on purpose, same discipline as every
scoped-down slice before this one:

- **No cartridge/script-loading format.** The proof script is a compiled-in
  string. Loading a `.lua` file from `kf/hal/storage.h` or a future asset
  partition, and the hot-reload the spec sheet names, are a later slice's
  job, once there is a real format to load.
- **No real API surface.** `kf.log` and `kf.report` exist to prove the
  binding mechanism works in both directions, not as the start of
  `pet.on_button()` / `draw.sprite()` / `save.set()`. Those want the pet
  simulation framework and the sprite/save HAL bindings to exist first, so
  the API is designed against something real rather than guessed at twice.
- **No ESP32 wiring.** Same reasoning as LVGL: `ports/esp32/` stays an
  empty skeleton, nothing here claims otherwise.
- **No frame-budget accounting for Lua's own time.** `kf_app_frame()`'s
  budget report does not yet include time spent in `kf_lua_port_frame()`
  the way it accounts for draw and transfer cost. Worth doing before a
  real pet script runs every frame; not needed to prove this slice's
  mechanism.

## Verified

- Full rebuild clean under `-Werror`.
- All six `ctest` targets pass, including the new `lua_determinism_check`.
- `tools/check_no_heap.py` clean -- Lua's own allocation never touches the
  system heap; every byte comes from `kf_arena_alloc(KF_ARENA_LUA, ...)`
  exactly once, then this file's own suballocator.
- `kamiframe-headless --verify-lua --frames 5000` (run manually, well
  past the 300 frames the ctest target uses) stays deterministic and
  never exhausts the arena -- the allocator's free/realloc path, not just
  its alloc path, genuinely works under sustained churn.
- `kamiframe-sim` (SDL backend, dummy video/audio drivers, so it runs
  headless in this sandbox) brings Lua up, runs 40 frames on the real-time
  path, and shuts down clean.
- Re-running `kamiframe-headless --verify-lua` twice in a row produces the
  identical `total` -- confirms the entropy-HAL hash-seed fix actually
  fixed the thing it was meant to fix, not just that the build succeeded.

**Not verified: an actual MSVC compile.** This sandbox has no Windows
toolchain, the same limit every ADR before this one has been honest about.
Lua's own `luaconf.h` does the safe thing by default here (`LUA_API`
resolves to plain `extern` unless `LUA_BUILD_AS_DLL` is defined, which
nothing in `cmake/fetch_lua.cmake` does), so this should not repeat
ADR 0013's `LV_ATTRIBUTE_EXTERN_DATA` saga -- but "should not, by reading
the source" is exactly the confidence level that saga started at too. This
gets pushed and watched on real CI before being called done, same as
everything else.

## Accepted cost

- **A hand-rolled allocator is more code to maintain than a vendored one**,
  and more code this project is fully on the hook for if it has a bug.
  Accepted because the alternative (a fourth vendored dependency) is
  worse for something this small and this well-understood, and because
  getting the shrink-never-fails contract right matters enough to want it
  written and reviewed in this codebase's own style rather than imported
  and trusted.
- **`io`/`os`/`debug`/`package` code ships in the binary unreachable.** A
  few kilobytes, accepted rather than forking Lua's own build file list to
  trim them -- the same "vendored code is not ours to hold to our own bar"
  reasoning `cmake/fetch_lvgl.cmake` and `fetch_lua.cmake`'s `-w` line
  already apply to warnings.
- **Lua's own time-in-frame is not yet in the budget report.** Noted above
  under what is not built; a script that is slow enough to matter will
  currently only show up as a general frame-time regression, not a
  Lua-specific line item.

## Later

- A real cartridge/script-loading format and hot reload from the
  simulator (the spec sheet names both explicitly).
- The actual `pet.*` / `draw.*` / `save.*` / `time.*` API surface, once
  the pet simulation framework and the relevant HAL bindings exist to
  design it against.
- Folding `kf_lua_port_frame()`'s own cost into `kf_app_frame()`'s budget
  report, the same way LVGL's pump cost should eventually be accounted
  for too.
- Wiring `kf_lua_alloc_get_stats()` into the constraint HUD (ADR 0010)
  alongside the other arenas -- today the HUD would only ever show the Lua
  arena at a flat 100%, since that is what `kf_arena_alloc()` itself
  tracks; this suballocator's own live-bytes figure is what would actually
  move.
- Deciding whether `ports/esp32/` gets a real Lua wiring in the same slice
  that gives it a real build at all, or whether Lua needs its own
  ESP32-specific pass first (RAM layout on that target is not identical
  to "give it a PSRAM block," the way this slice got to assume on
  desktop).
