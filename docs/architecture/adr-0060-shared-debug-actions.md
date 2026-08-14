# ADR 0060: One table of debug actions, and a checker that keeps both surfaces honest

Status: Accepted
Date: 2026-08-13

## Context

Kamiframe has two debug surfaces: the desktop debug window
(`simulator/src/sdl/sdl_debug_window.cpp`, a grid of clickable buttons) and
the KFDBG serial bridge (`ports/esp32/main/kf_dbg_bridge.cpp`, text verbs over
USB). They are meant to let a developer do the same things to a running pet
whichever build they are driving.

The underlying operations were already shared. `kf_pet_session.h`'s
`kf_pet_session_debug_*()` family is compiled into both builds, and both
callers already went through it — so this was never two implementations of one
behaviour.

What was *not* shared was the two **dispatchers** on top: an if/else chain of
verb strings on one side, a button array and a `switch` on the other. Two
hand-maintained lists over one shared API, with nothing connecting them.

The failure mode was silence. Add a capability to one side, forget the other,
and there is no build error and no failing test — the feature simply is not
there. By the time this was written the gap had already opened three times:

| Desktop had | Device had |
|---|---|
| Save Now | *nothing* |
| Jump to next stage | only absolute `JUMP <stage>` |
| Next Screen | only `BTN`, which fires a real MENU edge and also toggles Core's HUD |

None of the three was noticed by a person. They were found by inventorying the
two dispatchers side by side, on purpose, because Chris asked for parity — not
by anything that would have caught them on its own.

## Decision

**One table of portable debug actions**, in
`simulator/src/pet/kf_debug_actions.{h,cpp}`, compiled into both builds
through the same relative-path mechanism `kf_pet_session.cpp` and
`kf_frame_loop.cpp` already use.

Both dispatchers drive it:

- KFDBG's verb chain falls through to `dispatch_shared_action()`, which looks
  the verb up in the table, parses arguments according to the entry's declared
  argument kind, enforces the mutate tier (ADR 0035) from the entry's own
  `mutates` flag, and runs it. A verb exists on the device if and only if it
  is in the table or has its own declared-exception branch.
- The desktop window keeps its **layout** — rectangles, labels, which fixed
  argument each button supplies — because that is presentation the serial
  bridge has no use for. Its **behaviour** now goes through `run_shared()`.

This is ADR 0058's fix applied one layer up. That one merged two
hand-maintained copies of the per-frame sequence *after* they drifted into a
real hardware bug (the offline-ageing multiplier, wrong by 17%). This one
merges two hand-maintained copies of the debug surface before they can do
the equivalent.

Seven actions are in the table: `ADVANCE`, `CLOCK`, `JUMP`, `NEXTSTAGE`,
`RESET`, `SAVE`, `SCREEN`. The last three are new on the device and close the
three gaps above.

### The exclusions are data, not prose

A parity mechanism that demanded a desktop `RTC` button would be demanding a
lie. So the table is deliberately not universal, and what is left out is
declared in the same file as two arrays a checker can read:

- **`kDeviceOnlyVerbs`** — `RTC`, `SCANLINE`, `VSYNC`, `SHOT`. Each reads
  hardware a desktop does not have: the DS3231 over I2C, the panel's scanline
  counter over SPI, that counter's wrap, and a framebuffer shipped over a
  serial link to a machine that already has a window.
- **`kOtherMeansVerbs`** — care actions (on desktop you press the real
  on-screen care buttons, which exercises *more* of the stack), `BTN`/
  `BTNHOLD` (on desktop, an actual keyboard), `PING` (liveness for a link the
  desktop does not have), `STATE` (the desktop window shows it continuously
  rather than on request), and `MULT`.

`MULT` is the one worth naming. Both sides have it, so it is not a gap — but
`kf_frame_loop.h` explicitly documents that each bridge owns its own
multiplier and passes it to `kf_frame_loop_run()`. Folding that into this
table would overturn a deliberate decision as a side effect of tidying, which
is not a thing a refactor gets to do quietly.

## There are three surfaces, not two

The first version of this ADR, and of the checker, counted two. That was
wrong, and it was caught the only way it could have been: Chris ran the
command this ADR told him to run, and it did not exist.

`tools/kf_debug.py` is the third surface. Nobody types raw KFDBG verbs down a
serial line — the host tool is how a human actually drives the firmware — so a
verb the firmware understands and the tool has no subcommand for is, in
practice, a verb that does not exist. The checker passed clean in exactly that
state: the two surfaces it knew about agreed with each other, and the one
people use had been left behind.

`SAVE`, `NEXTSTAGE` and `SCREEN` gained subcommands, and rule 5 below now
covers the tool. **The mechanism built to catch this class of bug shipped with
an instance of the bug inside it**, which is worth recording plainly: the
blind spot was not in the code being checked, it was in the checker's idea of
how many places needed checking.

## The enforcement

`tools/check_debug_parity.py`, wired into CI and `dev.sh` beside
`check_no_heap.py` and `check_no_float.py`.

It is a source-level checker rather than a ctest case for a reason that is not
a preference: `kf_dbg_bridge.cpp` is device-only code that no desktop test
binary links, and never will — it talks to a UART. The only thing that can see
both dispatchers at once is something that reads the source.

Four rules:

1. Every KFDBG branch verb must be a declared exception. A new device verb
   with no table entry and no declaration fails.
2. No branch verb may also be in the table — a branch that shadows the table
   is two behaviours again.
3. Every table verb must be reachable from a desktop button. A portable action
   the desktop cannot reach is a device-only action in disguise.
4. Every desktop verb must be in the table, which catches a typo'd verb string
   — otherwise a dead button that logs at runtime and nowhere else.
5. `kf_debug.py` must reach every verb the firmware understands — the table
   plus the device-only ones, which have no desktop button by definition — and
   must not send a verb no firmware branch handles.

**All five were proven by breaking them and watching the specific message
appear**, then restoring. Rule 5's probe reproduced the real failure exactly:
delete the `KFDBG SAVE` string from `kf_debug.py` and the checker names it.
Two of the first probes were themselves bad — one renamed a function in a way
the regex still matched, another tripped a different rule than intended — and
were redone with realistic breakages. That
is the point of the exercise: a checker nobody has watched fail is a checker
nobody has evidence about, and this project has shipped three tests that
quietly stopped testing anything, including one that passed with the entire
drawing path deleted.

The script also refuses to report success if it parses either table as empty,
rather than passing vacuously on a file it cannot read.

### What it deliberately does not enforce

That the exception lists are *morally* correct. Someone can silence this by
adding a verb to `kOtherMeansVerbs` with no justification.

That is acceptable. The goal is not to make the wrong thing impossible — it is
to make it a deliberate, reviewable edit in a file whose header explains what
those lists mean, rather than an omission nobody ever sees.

## Consequences

The ack text for `ADVANCE`, `RESET`, `CLOCK` and `JUMP` changed. Every table
verb now acks with the same shape — the verb, plus the resulting stage and pet
age — instead of echoing its own arguments back. This generalises what
`JUMP`'s handler already did deliberately, and for the better reason: after
`JUMP` or `NEXTSTAGE`, what a human at the other end of a serial link wants to
know is where the pet *ended up*, which an echo cannot tell them.

Safe because `tools/kf_debug.py` matches on the reply **type** (`ack`), never
on the text. That was checked in the source before the change was made, not
assumed.

## Alternatives considered

**Port the three missing features and stop.** Rejected: it fixes today's three
gaps and does nothing about the fourth, and the whole reason Chris raised this
was drift, not a feature list.

**Make the desktop window generate its buttons from the table.** Rejected as
over-reach. Button geometry, grouping and the multiplier/form/branch pickers
are presentation, and the pickers in particular hold selection state with no
serial equivalent — their KFDBG counterpart is passing arguments to `JUMP`.
Forcing a shared layout would have meant contorting one surface to match the
other's input model.

**A C++ test instead of a Python checker.** Not possible, as above. Worth
recording so the next person does not spend an afternoon discovering it.
