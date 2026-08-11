# ADR 0018: The demo creature script

**Status:** Accepted, 2026-08-04
**Reversal cost:** Low. `kf_lua_demo_creature_script.h` is a single self-
contained Lua string plus a chunk-name constant; `sdl_main.cpp`'s only
dependency on it is the one `kf_lua_port_init()` call. Swapping it for a
different script, or nothing at all, touches two lines.

## Requirement

Chris, after the pet screen slice: *"Let's move onto the next part. What
would you recommend?"*, offered four options (a real Lua pet script,
evolution/life stages, a low-need warning colour, something else) and
picked the first. The reasoning behind recommending it: ADR 0016 built the
`pet.*` Lua binding and ADR 0017 built a screen to watch it on, but nothing
in the interactive build had ever actually loaded a script that used
`pet.*` for anything -- `sdl_main.cpp` still loaded ADR 0014's proof
script, which predates the pet framework entirely and doesn't touch it.
README.md's own description of the SDK -- *"Write creatures in Lua against
a small, documented API. Package them into a cartridge file. No C
required"* -- had never actually been demonstrated. This slice is that
demonstration: the first script in this repository that expresses real
creature behaviour, not a mechanism proof.

## Decision: reactive, not autonomous; text, not art

**The script reads three needs and announces band crossings, nothing
more.** `kf_lua_demo_creature_script.h`'s `on_frame` classifies
`pet.hunger()`/`happiness()`/`energy()` into four bands (critical / low /
ok / full) every frame and calls `kf.log()` with a short, in-character
line only on the frame a need actually crosses a boundary -- not every
frame it happens to sit in one, which at 30fps would be constant noise.
The "ok" band is silent in both directions on purpose: it's the pet's
normal resting state, and messaging every small drift either side of the
low/full line would drown out the two bands actually meant to prompt a
person to do something.

**No care logic, no autonomy, no `pet.save()` calls -- the script only
reads.** Two things kept this deliberately narrow. First, `kf_pet_session_
shutdown()` (ADR 0016) already calls `kf_pet_session_save()` on the way
out, so a script-driven autosave would be solving an already-solved
problem, not filling a gap. Second, and more basic: a script that also
*acted* on the pet's needs (auto-feeding itself when hungry, say) would
make the pet screen's Feed/Play/Rest buttons pointless the moment this
script is running -- the whole point of ADR 0017's buttons is that a
person does the caring. This script narrates; it does not play.

**Messages go to `kf.log()`, which means the console, not the screen --
and that gap is real, not glossed over.** There is no sprite, no audio,
and no second screen yet (ADR 0017's "Later" section covers the first two;
the pet screen is a stat display, not a message log). A person watching
`kamiframe-sim`'s window sees the bars move but has to also be watching
the terminal to see *why* the creature is unhappy, in its own words. That
is a genuine, current limitation of what the platform can express, not a
choice this script is making -- when a message channel exists on the
device itself (a toast, a status line, anything visual), this script is
exactly what should feed it, unchanged.

**The ADR 0014 proof script is not deleted or repurposed -- it stays
exactly what it always was.** `kf_lua_proof_script.h`'s own header comment
already says what it's for: proving the allocator survives real
allocation/free churn, checked by `kamiframe-headless --verify-lua`
against an exact arithmetic invariant (`total == 32 * frames`) that has
nothing to do with any pet. Repurposing it, or changing what it proves,
would either break that check or quietly stop testing what it has always
tested -- the same "add a new, narrowly-scoped thing; leave the old,
already-verified one alone" reasoning ADR 0016 and ADR 0017 both already
applied to their own predecessor scripts and screens. Only `sdl_main.cpp`'s
default changed, from the proof script to the demo creature; `headless_
main.cpp`'s `run_lua_check()` still loads the proof script, unchanged.

## What this slice actually builds

- `simulator/src/lua/kf_lua_demo_creature_script.h` -- the script itself,
  `kKfLuaDemoCreatureScriptSource` and its chunk name, following the exact
  embedding pattern `kf_lua_proof_script.h` and `kf_lua_pet_proof_script.h`
  already established (an `inline constexpr` `R"lua(...)lua"` string, no
  new build machinery).
- `sdl_main.cpp` now loads it instead of the ADR 0014 proof script.
- A new headless check, `kamiframe-headless --verify-lua-creature`
  (`headless_main.cpp`'s `run_lua_creature_check()`) and its matching
  `lua_creature_check` ctest target: proves the script survives a full,
  realistic lifecycle -- fresh and full, decayed via ten simulated days
  down through every band to genuinely empty, cared back up to full --
  without ever raising a Lua runtime error in either direction. Not a
  check on exact log text: `kf.log` (`lua_kf_log` in `kf_lua_port.cpp`)
  writes straight to `kf_log`/stderr with no capture hook, so there is
  nothing to assert the literal strings against without adding one. What
  it checks instead: `kf_lua_port_frame_count()` matches the exact number
  of frames run (proves `on_frame` never errored and got disabled
  partway through -- see `kf_lua_port_frame()`'s own "disable further
  calls" fallback), and the live `kf_pet_session_state()` read directly
  from C++ at both ends of the journey (proves the pet genuinely reached
  rock bottom before recovering, and genuinely reached max after). The
  same category of check as `lua_pet_binding_check` (ADR 0016) and for the
  same reason: this is a logic/robustness question, not a rendering one,
  so it gets an exact-arithmetic-style check, not a checksum.

## Found while building the check, not the script

The check's first version called `kf_pet_session_feed()`/`play()`/`rest()`
once each after driving every need to zero, expecting all three back at
max, and failed: `kCareBoostMp` (`kf/pet.cpp`) is `25000` -- a flat quarter
of the full `0..100000` range per call, calibrated in ADR 0015 to exceed a
single hour's decay, not to refill an empty need in one call. Coming from
genuine zero, one call only reaches 25%, still inside the "low" band --
which is exactly what the log showed: a second round of "starting to get
hungry" / "could use some playtime" / "getting a little tired" instead of
the expected "fully rested" style messages, briefly looking like a script
bug before the actual arithmetic (`4 * 25000 == KF_PET_MILLIPERCENT_MAX`)
made the real cause obvious. Fixed by calling `feed()`/`play()`/`rest()`
five times each in the check (four needed, one call of margin -- `kf_pet_
feed()` and friends clamp at max regardless, so extra calls cost nothing
and drift nothing). Nothing about the demo creature script itself, or the
pet framework it reads, needed to change; this was purely a test written
against a wrong assumption about how much one care action restores,
caught by the same discipline this whole session has used throughout:
run the actual check before believing it, not just before shipping it.

A second, much smaller thing caught before it ever reached a compiler:
this file's own first draft included the literal text `pet.*/kf.*` inside
a block comment, describing the two binding surfaces the script reads.
`*/` inside a `/* */` comment closes it early -- the exact class of bug
`kf/pet.h` hit and fixed earlier this same session (a `"/*" within
comment` warning from a doc-comment referencing `kf/hal/*.h`). Caught on
inspection immediately after writing it, before the first build, not by
the compiler -- worth naming since the earlier instance of this exact
mistake didn't prevent a second one; the fix is the same in both cases,
reword the doc comment so the literal glob character is never adjacent to
a slash.

## Verified

- Full clean rebuild (target `clean`), GCC 13, the same strict warning set
  as every prior slice, clean.
- `tools/check_no_heap.py`: clean, unmodified -- this slice is entirely
  simulator-side.
- The new `lua_creature_check` ctest target, deterministic across 4
  repeated direct runs (`kamiframe-headless --verify-lua-creature`) before
  being folded into the suite -- no golden checksum to lock in here (see
  "What this slice actually builds" above for why), so determinism means
  "PASS every time," not "the same hash every time."
- All 10 `ctest` targets (this one included) pass together on the clean
  rebuild above.
- `kamiframe-sim` under the dummy SDL video driver, `--frames 90`: the
  demo creature loads (`the creature stirs`, `Lua 5.5.0 ready, script
  '=demo_creature' loaded`) and runs the full 90 frames with no error and
  a clean shutdown log line (`shutting down after 90 frame(s)`). No band-
  crossing messages appear in this specific run, correctly -- 90 frames
  at 30fps is 3 real seconds, nowhere near enough elapsed time for any
  need to visibly decay; the headless check above is what actually proves
  the band-crossing logic across a realistic multi-day timescale.

## Later

- A real, on-device expression channel for these messages -- a toast, a
  status line on the pet screen itself, anything visual -- once one
  exists. The band-crossing logic itself would not need to change; only
  where `announce()`'s messages go.
- Sound. Multiple messages already lend themselves to a chime or cue
  (`"hunger is critical"`, `"fully rested!"`) once an audio HAL exists.
- Care logic living in a script rather than only C++ buttons, if a future
  slice wants the SDK to expose *acting* on the pet, not just reading it --
  deliberately not this slice's job; see "Decision" above for why reading-
  only was the right scope here.
- Evolution/life stages (the option not picked this round) would give this
  script actual state to read beyond the three needs -- worth revisiting
  once that exists.

## Superseded in part

**"Text, not art"** (Decision heading) and **"There is no sprite, no audio,
and no second screen yet"** described this slice's own scope honestly at the
time, but the gap they named as real and current is closed now. The demo
creature (`examples/creature_demo/creature.lua`, successor to
`kf_lua_demo_creature_script.h`) draws a real sprite via `kf.sprite()`,
animates it, and the platform has multiple screens (`kf.screen("home")`,
`kf.screen("info")`, ADR 0044). Sound and haptics remain genuinely absent —
that part of the gap is still accurate. The reasoning for why this
particular script stayed reactive/text-only at the time is unaffected;
only "no sprite... yet" has since been answered elsewhere.
