# ADR 0034: KFDBG parity for care actions and life-stage jump

**Status:** Accepted
**Date:** 2026-08-09

## Context

ADR 0030 (the serial debug bridge) and ADR 0031 (time control) brought the
wire up to PING/SHOT/STATE/SCANLINE/VSYNC/BTN/BTNHOLD/ADVANCE/RESET/MULT,
but two things the desktop simulator has had for a while still had no
KFDBG equivalent by the time this ADR starts:

- **The five care actions** -- feed, play, rest, bath, flush
  (`simulator/src/pet/kf_pet_session.h`). On desktop these are bound to
  number keys 1-5 in that order (`simulator/src/sdl/sdl_input.cpp`) and
  dispatched by `kf_creature_screen.cpp`'s `handle_care_buttons()`. Four of
  the five take a `variation` in `[0, KF_PET_CARE_VARIATION_COUNT)` --
  currently 3 -- which is what actually decides whether the creature likes
  the care (`kf/pet.h`); flush takes none.
- **Stage jump** -- `kf_pet_session_debug_jump_to_stage()` puts the pet at
  the start of a chosen life stage, alive and fully fed, with an optional
  form index. It exists because an uncared-for pet dies of neglect during
  the child stage and a dead pet is frozen permanently, making every later
  stage unreachable for inspection without it. Its only caller before this
  ADR was `sdl_debug_window.cpp`'s desktop-only stage-jump buttons; the
  project owner needs it on hardware specifically to look at sprites per
  stage.

Both functions already existed with exactly the right shape for this. The
gate they ride, `KF_PET_SESSION_ENABLE_DEBUG_CONTROLS`, was already `1` in
`ports/esp32/main/CMakeLists.txt` (set there by ADR 0031 for
ADVANCE/RESET), so `kf_pet_session_debug_jump_to_stage()` was already
linked into the ESP32 build with no caller -- confirmed by re-reading that
file rather than assumed, per this task's own instruction. The four care
wrappers (`kf_pet_session_feed()` etc.) are not gated at all: they are part
of the main gameplay surface `kf_creature_screen.cpp` already calls on
every backend, ESP32 included.

## Decision

### 1. Six new KFDBG commands, exactly the shape ADR 0030/0031 established

`ports/esp32/main/kf_dbg_bridge.cpp` adds six command handlers, one `else
if` branch each in `process_command_line()`, each handler building decoded
content and handing it to the existing `kf_dbg_enqueue_reply()` -- no
change to the framing/base64/CRC machinery, matching this codebase's own
documented "cost to change" for a new command (ADR 0030):

- **`KFDBG FEED <variation>`**, **`KFDBG PLAY <variation>`**,
  **`KFDBG REST <variation>`**, **`KFDBG BATH <variation>`** -> `ack`.
  Call `kf_pet_session_feed()`/`_play()`/`_rest()`/`_bath()` directly.
  `variation` is validated against `KF_PET_CARE_VARIATION_COUNT` (kf/pet.h)
  before any handler runs, replying `err` otherwise. The ack reports
  `pet->last_reaction` as a raw number (`0`=liked, `1`=neutral,
  `2`=disliked), the same convention `KFDBG STATE`'s `stage`/`base_trait`
  fields already use.
- **`KFDBG FLUSH`** -> `ack`. Calls `kf_pet_session_flush()`. No variation
  argument (`kf_pet_flush()` takes none) and no reaction reported --
  `kf_pet_flush()` deliberately leaves `last_reaction` as the previous real
  care action left it (hakoniwaos/src/pet.cpp), so echoing it here would
  misattribute someone else's reaction to a chore that has none of its
  own, the same reasoning `handle_care_buttons()`'s own FLUSH log line
  already follows.
- **`KFDBG JUMP <stage> [teen_form] [adult_branch]`** -> `ack`. Calls
  `kf_pet_session_debug_jump_to_stage()` directly. `stage` is the raw
  `kf_pet_stage` enum value (0=egg..4=adult); `teen_form`/`adult_branch`
  are optional and both default to `0` when omitted, exactly the session
  function's own "unset" default. The ack reports the state that actually
  resulted (`pet->stage`/`teen_form`/`adult_branch`, read back after the
  call) rather than an echo of the request, since out-of-range
  `teen_form`/`adult_branch` silently fall back to `0` inside the session
  function rather than erroring -- reading back is what lets a scriptable
  client see exactly what landed without a separate `KFDBG STATE` round
  trip.

### 2. Care commands bypass `KFDBG BTN` on purpose, not by oversight

A real button press already reaches all four care functions through
`kf_creature_screen.cpp`'s `handle_care_buttons()`, and `KFDBG BTN
<mask>` already injects a button press. Routing `KFDBG FEED` etc. through
BTN injection (`KFDBG BTN 16` for feed, say) was the first shape
considered and rejected, for two concrete reasons:

1. `handle_care_buttons()`'s `variation` is an implicit, per-action
   cycling counter (`0 -> 1 -> 2 -> 0 -> ...`) private to that file, there
   purely as a keyboard-binding convenience -- not part of the session
   API. Driving care through BTN injection would force a scripted client
   to reconstruct that hidden counter (how many times has this action
   fired since the pet was last reset or jumped?) just to land on a
   specific variation. Over a scriptable serial link, taking `variation`
   as an explicit argument is strictly more useful than hiding it behind
   press-count state the caller cannot see or reset independently of the
   pet itself -- the brief's own instruction to "decide... and justify it"
   resolves this way because a serial client's whole point is
   reproducibility, and hidden per-process state defeats that.
2. A one-shot `KFDBG BTN` mask does not reliably register at all: Core's
   debounce (`kDebounceUs`, `hakoniwaos/src/app.cpp`) requires the SAME
   mask across consecutive ~33ms-apart polls before it produces a press
   edge, so BTN's single-poll injection is silently dropped unless the
   host holds it for >=120ms via `BTNHOLD` -- a real bug `tools/
   kf_debug.py`'s own `press --hold-ms` comment already documents, found
   on hardware. Calling `kf_pet_session_*()` directly sidesteps debounce
   entirely, which is correct here: these commands exist to exercise the
   care action's own effect, not whether a simulated button press clears
   Core's debounce filter.

### 3. Not gated behind `KF_DBG_INPUT_INJECT_ENABLE`

`KF_DBG_INPUT_INJECT_ENABLE` (ADR 0030) narrows the bridge to keep
PING/SHOT/STATE's read-only introspection while refusing remote *button*
control. FEED/PLAY/REST/BATH/FLUSH/JUMP are not button control -- they
call session functions directly, same as ADVANCE/RESET/MULT, which ADR
0031 already decided stay available whenever the bridge as a whole is on,
un-narrowed by that flag. This ADR follows that precedent rather than
inventing a third tier: `RESET` is already a far more drastic state
mutation than any single care action (a full pet reset vs. one need bumped
a little), so gating care/jump more tightly than `RESET` while leaving
`RESET` itself ungated would be an inconsistent line to draw, not a more
careful one. `KF_DBG_BRIDGE_ENABLE=0` still removes all six, same as
everything else in the file.

### 4. `KFDBG JUMP` accepts `teen_form == KF_PET_TEEN_FORM_DUST`, does not clamp it away

`KF_PET_TEEN_FORM_DUST` is deliberately equal to `KF_PET_TEEN_FORM_COUNT`
(4), one past the four named forms -- a real, reachable form (an
uncared-for teen), not an error value; the desktop's own stage-jump picker
already exposes it. The device handler validates `teen_form` is decimal
and nothing more, leaving range-checking (accept `[0,
KF_PET_TEEN_FORM_DUST]`, fall back to `0` outside that) to
`kf_pet_session_debug_jump_to_stage()` itself, which already implements
exactly that contract -- duplicating it in the handler would just be a
second place for the two to drift apart. `tools/kf_debug.py`'s `jump
--teen-form` mirrors the same inclusive range host-side, so a value of `4`
is accepted and sent, not rejected before it ever reaches the wire.

### 5. Host side: `care` and `jump` subcommands in `tools/kf_debug.py`

`care <1-5|feed|play|rest|bath|flush> [--variation N]` accepts either the
digit or the name -- the digit form is the "keymaps for 1, 2, 3, 4, 5
functions" the task asked for, mapped in the same order the desktop binds
its number keys (`CARE_ACTION_ALIASES`, mirroring `sdl_input.cpp`'s
`kBindings` table), so a script that has memorised "1=feed" from the
keyboard binding works unchanged here. `--variation` defaults to `0`, not
a cycling counter -- a fresh `kf_debug.py` process every invocation has no
memory of a prior run's press count, so cycling state would not even be
meaningful host-side the way it is for a long-lived desktop keyboard
session.

`jump <stage> [--teen-form N] [--adult-branch N]` accepts a stage name
(`egg`/`baby`/`child`/`teen`/`adult`) or the raw `0-4`, more forgiving to
type at a prompt than the wire's own raw-enum-only syntax.

## What this slice does NOT reach

- **`tools/kf_panel.py` is untouched** -- owned by a separate, concurrent
  task per ADR 0031's own boundary; this ADR only adds the wire syntax and
  `kf_debug.py` CLI surface for that task to wire up later, the same
  relationship ADR 0031 established for ADVANCE/RESET/MULT.
- **`hakoniwaos/` and `simulator/src/headless/` are untouched**, per this
  task's explicit boundary (concurrent work owns the indexed-sprite
  blitter there).
- **No new gating.** `KF_PET_SESSION_ENABLE_DEBUG_CONTROLS` already
  covered `_jump_to_stage()`; this ADR gives it a second caller
  (`kf_dbg_bridge.cpp`, ESP32) alongside `sdl_debug_window.cpp` (desktop),
  not a new flag.

## Verified

- **Desktop build.** `cmake --build build -j8` clean, zero new warnings.
  `ctest --test-dir build`: all tests pass (36/36 at time of writing --
  concurrent work on `simulator/src/headless/` added one test since this
  branch's earlier 35/35 baseline; none of the new failures, because there
  are none, are from this slice).
- **`tools/kf_debug_selftest.py`**, extended in the same style as its
  existing checks: `parse_stage()` against every stage name plus raw
  numbers and rejection cases; `CARE_ACTION_ALIASES` against the
  desktop's 1-5 order; and, using a new `FakeLink` test double (records
  sent commands, returns a canned reply -- the same shape `FakeSerial`
  already gives `read_frame()`), the exact wire string `cmd_care()`/
  `cmd_jump()` build for representative inputs, including the
  `KF_PET_TEEN_FORM_DUST` case and every rejection path (bad action,
  out-of-range variation, `--variation` on `flush`, `--teen-form` past the
  dust form). All pass.
- **`idf.py build` clean for esp32s3 against the real ESP-IDF v6.0.2
  install** (default config, `KF_DBG_BRIDGE_ENABLE`/`KF_DBG_INPUT_INJECT_
  ENABLE`/`KF_PET_SESSION_ENABLE_DEBUG_CONTROLS` all at their existing
  defaults, unchanged by this ADR): `kf_dbg_bridge.cpp` compiles with no
  warnings under this project's existing `-Wall -Wextra -Werror` build.
  `kamiframe-firmware.bin`: 660,112 bytes total image (0xa1290), 58% of
  the 0x180000-byte app partition free -- up from ADR 0031's 648,903-byte
  baseline (+11,209 bytes, all in flash, for six new command handlers and
  their string literals; no static-RAM comparison was run for this ADR,
  unlike ADR 0030/0031's own byte-exact deltas, since nothing here adds a
  new global of consequence -- `parse_care_variation()`'s buffers and each
  handler's `content[]` are all on-stack, freed every call, same as every
  existing handler in this file).

## Not verified

- **Nothing in this ADR has run against real hardware.** No board is
  reachable from this environment (CLAUDE.md's "no hardware yet" rule).
  Every claim above is a desktop test run, a host-side unit test against a
  fake transport, or a cross-compile -- never a `KFDBG FEED` or `KFDBG
  JUMP` actually sent over a physical UART to a device actually mutating a
  live pet.
- **The reaction reported by FEED/PLAY/REST/BATH's ack has not been
  cross-checked against a real creature's config on real hardware** --
  the value is read straight from `kf_pet_state::last_reaction` immediately
  after the call, the same field `KFDBG STATE` already exposes, so this is
  a low-risk claim, but "low-risk" is not "verified" by this project's own
  standard.
- **`tools/kf_panel.py` does not yet expose these six commands** -- out of
  scope for this task, same as ADR 0031's identical note about
  ADVANCE/RESET/MULT.

## Cost to change

**Adding a seventh care-adjacent KFDBG command:** identical shape to the
six above -- one `else if` in `process_command_line()`, one `handle_*()`
following the existing pattern, calling a `kf_pet_session_*()` wrapper
directly rather than routing through `KFDBG BTN`.

**Un-deciding "explicit variation, not BTN injection":** would mean
routing FEED/PLAY/REST/BATH through `KFDBG BTN`/`BTNHOLD` instead, which
reintroduces both problems "Decision" #2 above describes -- hidden
per-process cycling state a scripted client cannot see, and debounce
timing a one-shot injection cannot reliably clear. Not a one-line change;
would need either a stateful cycling counter exposed over the wire (a new
kind of state this protocol has not needed before) or accepting that care
actions become flaky over KFDBG the same way undersized `press --hold-ms`
already is.
