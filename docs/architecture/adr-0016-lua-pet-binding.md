# ADR 0016: The `pet.*` Lua binding, and a pet session to bind it to

**Status:** Accepted, 2026-08-04
**Reversal cost:** Low. `pet.*` is seven functions with no state of their
own; deleting the binding deletes a table registration and a header/cpp
pair. `kf_pet_session` is a thin, replaceable orchestration layer -- see
"Accepted cost" for the one design trade it locks in.

## Requirement

ADR 0014 (Lua embedding) named this explicitly as its own next step: *"The
real Lua `pet.*` (and `save.*`, `draw.*`, `time.*`) API surface -- more
buildable now that there is a real pet state to bind to, rather than
guessed at when only the VM mechanism existed."* ADR 0015 built that real
pet state. This slice is the connective tissue between the two: the piece
that turns "Lua can run a sandboxed script" and "Core can compute a pet's
needs" into "a script can read and act on a pet."

Chris confirmed this as the next slice over two other reasonable
candidates (a pet UI, and evolution/life-stage content) specifically
because both of those want something to bind to or write against first --
a UI needs live values to display, and this project has been writing game
logic in the scripting layer rather than hard-coding it in C++, so
evolution rules are the kind of thing that wants `pet.*` to exist before
they get written, not before.

## Decision: a `pet` global table, and a session to own what it reads

**`pet.hunger()`, `pet.happiness()`, `pet.energy()`** return the live
need, as an integer millipercent, straight from Core. **`pet.feed()`,
`pet.play()`, `pet.rest()`** call the matching care action. **`pet.save()`**
persists now, for a script that wants to checkpoint after a care action
rather than waiting for shutdown. Registered unconditionally in
`kf_lua_port.cpp`, the same as `kf.log`/`kf.report` -- an unused binding
costs nothing, and this keeps one file the single place every global a
script can see gets wired up.

**Something has to own the ONE live `kf_pet_state` these functions read
and mutate.** `kf/pet.h`'s functions are deliberately pure -- explicit
`kf_pet_state*`, no notion of "the" pet, exactly what makes them
trivially unit-testable with no HAL in the picture (ADR 0015). A Lua
binding needs the opposite: a `pet.hunger()` call takes no arguments and
has to read *something*. `kf_pet_session` (`simulator/src/pet/`) is that
something: `kf_pet_session_init()` calls `kf_pet_load_and_advance()` once
at boot (the "aged while switched off" half), `kf_pet_session_frame()`
advances it by live elapsed time every frame (the "ages while you are
playing" half, see the next section), and the Lua binding, and later a UI,
both read `kf_pet_session_state()` rather than each inventing their own
copy. Lives in `simulator/`, not `hakoniwaos/`, for the same reason
`kf_lvgl_port` and `kf_lua_port` do: this does not claim the ESP32 build
has a wired-up pet session, only that the desktop and headless backends
do.

**Live decay has to be batched, not applied every frame, and this was
found by the check that proves it, not by inspection.** The first version
of `kf_pet_session_frame()` accumulated milliseconds and called
`kf_pet_advance()` every time a whole second crossed. That is a genuine
bug, not a style choice: `kf_pet_advance()`'s decay formula is
`rate_mp_per_hour * elapsed_seconds / 3600`, integer division, and for
every rate this project configures (all under 3600 mp/hour --
`kf_pet_default_config()`'s slowest, energy, is 521) a single call with
`elapsed_seconds` under roughly 4-7 (rate-dependent) truncates to EXACTLY
ZERO. Flushing once per second means every call independently computes a
fresh zero and throws it away, with no memory of what the previous call
discarded -- a live-ticking pet would never age at all, no matter how long
the session ran, while `run_lua_pet_check()`'s own first run demonstrated
this directly: 300 frames of accumulated live time produced no hunger
movement whatsoever. The fix is `KF_PET_SESSION_FLUSH_SECONDS` (30):
`kf_pet_session_frame()` accumulates elapsed milliseconds across many
calls and only actually invokes `kf_pet_advance()` once at least 30
seconds have built up, batching a much larger `elapsed_seconds` into far
fewer calls. This is a deliberate, bounded trade against
`kf_pet_advance()`'s own exactness, not a bug fixed by hiding it: a live
session slightly UNDER-decays relative to true continuous-time decay, by
at most one flush interval's worth of truncation per flush, always in the
generous direction (never negative, never compounding unboundedly across
flushes, since each flush's remainder is discarded independently rather
than carried forward as debt). Offline fast-forward
(`kf_pet_load_and_advance()`, called once at boot with however many real
seconds actually passed) is exact and unaffected -- this trade is specific
to the per-frame live path, where "genuinely real-time" was never the
requirement to begin with; the actual requirement (see ADR 0015) is no
drift over weeks of *offline* uptime, and this does not touch that.

**A second bug, also found by writing this check, sat in already-shipped,
already-verified code from ADR 0014.** `kf_lua_port.h`'s own documentation
promises a caller can retry `kf_lua_port_init()` with different source
after a `kf_lua_port_shutdown()` -- exactly what proving the pet binding
against two different scripts back to back needed. That path had never
actually been exercised before (every prior Lua check calls
`kf_lua_port_init()` exactly once per process), and it panicked the
instant this test exercised it: `kf_lua_alloc_init()` asserts it is never
called twice, and `kf_lua_port_shutdown()` never reset that guard. The
deeper reason it could not simply be reset is `kf_lua_alloc_init()`
acquires its one block from `kf_arena_alloc()`, a bump allocator with no
free (ADR 0014) -- calling it again would try to carve a second
`KF_ARENA_LUA_BYTES` block out of an arena sized for exactly one,
exhausting it. The fix separates "has this allocator ever acquired its
block" (`g_base`, set once, ever) from "is it currently active"
(`g_active`, toggled by init/shutdown): the underlying block is acquired
exactly once for the process's lifetime, and every later re-init just
resets the free list over that same block, rather than asking the arena
for a second one. `kf_lua_port_shutdown()`, and both of `kf_lua_port_init`
's own failure paths (a script that fails to compile or whose top-level
code errors), now call the new `kf_lua_alloc_shutdown()` to clear that
flag, making the "retry with different source" contract `kf_lua_port.h`
already documented actually true rather than aspirational.

## Verified

- Full clean rebuild (every object file, including all vendored
  dependencies, recompiled from source, not an incremental build), GCC
  13, the same strict warning set as every prior slice, clean.
- `tools/check_no_heap.py`: clean.
- A new `lua_pet_binding_check` `ctest` target
  (`kamiframe-headless --verify-lua-pet`), running two proof scripts back
  to back against the SAME continuing `kf_pet_session` (see
  `kf_lua_pet_proof_script.h`): a decay-and-read phase (1200 live frames,
  no care calls, comfortably crossing one `KF_PET_SESSION_FLUSH_SECONDS`
  boundary) followed by a care-and-mutate phase (30 frames, calling
  `pet.feed()`/`pet.play()`/`pet.rest()` every frame). It checks: the last
  value `pet.hunger()` reported through Lua matches
  `kf_pet_session_state()->hunger_mp` read directly from C++ (the FFI
  marshaling is exact, not approximately right); hunger is strictly below
  max after the decay phase (proves a genuinely live, ticking read, not a
  frozen snapshot); and after the care phase, all three needs -- not just
  hunger -- read back at exactly max from the live C++ state independent
  of what the script itself reported (proves `pet.play()` and
  `pet.rest()` are each correctly wired to their own care action, not
  aliased to `pet.feed()` by a copy-paste mistake that a hunger-only check
  would miss).
- All 8 `ctest` targets, this one and `pet_offline_ageing_check` included,
  pass consistently across the full-suite runs performed during this
  slice, and `--verify-lua-pet` alone passes across 8 repeated runs.
- `kamiframe-sim` runs 90 frames cleanly under the dummy SDL video driver
  with the full boot order (`kf_app_init` -> LVGL -> pet session -> Lua)
  and full frame order (LVGL pump -> pet session frame -> Lua frame) wired
  in, confirming the ordering requirements documented in `sdl_main.cpp`
  hold in practice, not just in the headless check.

## Accepted cost

Live decay is approximate, bounded by `KF_PET_SESSION_FLUSH_SECONDS` (30)
-- see "Decision" above for exactly what that trade is and why it is
deliberate rather than a shortcut. It is not tunable per caller yet;
changing it means editing the constant, not passing a parameter, the same
scope discipline every illustrative default in this project accepts (see
ADR 0015's `kf_pet_default_config()`) until there is a real reason to make
it configurable.

`kf_pet_session` is a single global instance -- one pet, not a roster.
That already matches everything built so far (`KF_PET_SAVE_KEY` is one
fixed key, not per-pet), and revisiting it is a save-format and API
question for whenever multiple pets are actually a requirement, not
before.

No cartridge or script-loading format still. The two proof scripts here
are exactly that: proof the binding works, not anything meant to survive
past this slice, same as `kf_lua_proof_script.h` before them.

## Later

- A real `pet.*` consumer: an actual script that reacts to needs crossing
  thresholds, once there is a reason to write one (a UI to react visibly,
  or evolution logic to trigger against).
- `save.*`, `draw.*`, `time.*` -- the rest of ADR 0014's named API surface,
  each wanting its own real consumer before being designed rather than
  guessed at.
- A UI reading `kf_pet_session_state()` directly, the non-Lua consumer
  this layering was built to support without inventing a second pet
  instance.
- Making `KF_PET_SESSION_FLUSH_SECONDS` configurable, if a real caller
  ever needs finer- or coarser-grained live ticking than 30 seconds
  buys.
