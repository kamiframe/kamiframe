# Contributing to Kamiframe

Thanks for reading this before opening a PR. It's short on purpose — read
[LICENSING.md](LICENSING.md) too, it's short as well, and it answers "what
am I allowed to build with this" more completely than anything below.

## What you may and may not reuse

This is the one section to get right before anything else, because getting
it wrong is worse than not contributing at all.

- **HakoniwaOS, the SDK, the simulator, the tools, and the demo creature's
  *code*** are all Apache 2.0. Read them, copy from them, build on them,
  ship a commercial closed-source creature using none of this repo's code
  visible to a player. Nobody needs to ask.
- **The demo creature's characters, artwork, animations, names, and
  designs are copyrighted and not licensed for reuse.** Its code is a
  worked example; its cast is not a free asset pack. Don't ship them in
  your own creature, don't use them as your project's mascot. If you want
  a creature, draw one — see LICENSING.md's "same structure id Software
  used with Doom" line for why that split exists.
- **Hardware** (schematics, PCB, BOM, enclosure) lives in a *separate*
  `kamiframe/hardware` repository under CERN-OHL-W v2, specifically so a
  software PR and a hardware PR never blur the two licenses together. If
  you're contributing hardware changes, that's the repo, not this one.
- **Games and creatures you write with the SDK are entirely yours.** Open,
  closed, free, paid — no obligation back to this project. That's not
  charity, it's the point: a platform where commercial use is a grey area
  isn't a platform.

If you're not sure which side of a line something falls on, open an issue
and ask before you build on it. LICENSING.md says the same thing and means
it.

## Proposing a change

Small, well-scoped PRs (a bugfix, a doc fix, a new example creature) can
just be a PR — open one, explain what and why in a sentence or two.

Anything that changes an interface, a data format, a build/CI rule, or "how
something in HakoniwaOS works" should start as an issue or a discussion
before code, especially if it touches:

- the HAL surface (`hakoniwaos/include/kf/hal/*.h`) — the boundary between
  Core and a backend
- the save format, or anything `kf_pet_session` persists
- the Lua API surface (`pet.*`, `kf.*`)
- build system layout or CI

For anything in that list, expect to be asked to write an **ADR** (below)
either before or alongside the code. That's not bureaucracy for its own
sake — this project has, more than once, shipped a defect because a
decision lived only in someone's head and the next task guessed wrong. An
ADR is cheaper than that guess.

## The ADR convention

`docs/architecture/` holds one Markdown file per decision,
`adr-00NN-short-name.md`, indexed in `docs/architecture/README.md`. Read a
few before writing one — [ADR
0008](docs/architecture/adr-0008-memory-model.md) and [ADR
0030](docs/architecture/adr-0030-serial-debug-bridge.md) are good
examples of the shape, one older and simpler, one newer and fuller.

The shape isn't rigid, but it consistently covers:

- **Status and date** at the top (`Accepted`, and when).
- **Context** — what problem forced this decision, in terms of what the
  project needed, not just "we decided to."
- **Decision** — what was actually chosen, and *what else was considered
  and rejected, with the reason*. An ADR that only describes what
  happened, with no rejected alternative, usually means the alternatives
  weren't actually weighed.
- **Consequences** — what this costs, what it forecloses, what got
  simpler.
- **Verified** / **Not verified** — stated separately and honestly. If
  something wasn't tested against real hardware because no hardware was
  reachable from that task's own environment, the ADR says exactly that,
  not "should work." See ADR 0030's "Not verified" section for what this
  looks like when a later pass has to reconcile it against another ADR
  that turned out to describe something different.
- **Cost to change** — roughly how expensive reversing this would be
  later. This is what lets a future contributor tell "settled" from
  "settled until something better shows up."

**When you land an ADR, add its row to `docs/architecture/README.md` in
the same commit.** That index has drifted out of date and been caught and
fixed three separate times already — see that file's own closing note. A
missing row isn't cosmetic on this project: task briefs get generated from
these documents, so a stale or missing reference gets copied forward into
whatever reads it next. `tools/check_doc_links.py` catches dangling links
and ADR numbers mentioned but never written; it isn't wired into CI yet,
but running it (`python3 tools/check_doc_links.py .`) before you open a
doc-touching PR is cheap insurance.

## The testing bar

Tests here have to prove something, not just pass. `ctest --test-dir
build --output-on-failure` needs to stay green, but a green run is the
start of the bar, not the whole of it — this project has shipped tests
that quietly stopped testing anything, including one that passed with the
entire drawing path deleted.

**Before you consider a new test done, break the thing it's testing on
purpose and watch the assertion actually fail.** Comment out the fix,
delete the call, whatever makes the bug real again — if the test still
goes green, it isn't testing what you think it's testing. This is not
optional polish; it's the actual bar. Say what you broke and what the
failure looked like in the PR description if it's not obvious from the
diff.

Run `bash dev.sh test` before opening a PR — it runs `ctest`, both
constraint scanners below, and a couple of other checks. See
[BUILDING.md](BUILDING.md) for the full build/test story, including
Windows.

## No heap, no floats, in Core

`hakoniwaos/` (HakoniwaOS itself — the display driver, sprite engine,
pet simulation framework, everything under `hakoniwaos/src` and
`hakoniwaos/include`) has two hard constraints that apply to any PR
touching it:

- **No heap.** Core allocates only from the fixed arenas in
  `kf/arena.h`, sized from `kf/budget.h`. `#pragma GCC poison` catches
  most of it at compile time under GCC/Clang (the ESP32 toolchain);
  `tools/check_no_heap.py` catches the rest (`operator new`/`delete`,
  and anything MSVC's build wouldn't poison) and runs in CI.
- **No floats.** Core is fixed-point / integer arithmetic only — the
  ESP32-S3 has a single-precision FPU, but `double` is
  software-emulated, and a stray one in a per-frame path is a silent
  frame-budget cost. `tools/check_no_float.py` enforces this the same
  way `check_no_heap.py` does, and runs alongside it in CI and in
  `dev.sh test`.

Both scripts have a documented, narrow escape hatch (`kf-allow-heap` /
`kf-allow-float` on the specific line) for a genuine, reviewed exception.
Reach for it rarely, and explain why in the PR when you do.

Backends (`simulator/`, `ports/*`) are not under either constraint —
allocating and doing float math is their job. The line is
`hakoniwaos/src` and `hakoniwaos/include`, nothing else.

## Before you open the PR

- `ctest --test-dir build --output-on-failure` is green.
- `python3 tools/check_no_heap.py .` and `python3 tools/check_no_float.py
  .` both pass (or `bash dev.sh test`, which runs both).
- If you touched anything the ESP32 build compiles (`hakoniwaos/`,
  `sdk/`, or a HAL header), it still builds for `ports/esp32` — see
  [BUILDING.md](BUILDING.md) if you don't have the ESP-IDF toolchain set
  up; say so in the PR if you couldn't check this yourself, rather than
  assuming it's fine.
- Any new test can be watched failing against the bug it exists to catch,
  per "The testing bar" above.
- Any ADR-worthy decision has an ADR, indexed in
  `docs/architecture/README.md`, in the same PR.

That's the whole list. If something here is unclear or wrong, that's a bug
in this document — open an issue and it'll get fixed, the same offer
LICENSING.md makes.
