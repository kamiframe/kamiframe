# ADR 0035: Splitting KFDBG by observe vs. mutate

**Status:** Accepted
**Date:** 2026-08-09

## Context

ADR 0030 introduced two compile-time flags for the KFDBG serial debug
bridge: `KF_DBG_BRIDGE_ENABLE`, the whole channel on or off, and
`KF_DBG_INPUT_INJECT_ENABLE`, a narrower flag that turned off only
`BTN`/`BTNHOLD` -- button injection -- while `PING`/`SHOT`/`STATE` kept
working. At the time that was the whole story: those three read-only
commands and two button-injection commands were the entire protocol.

ADR 0031 added `ADVANCE`/`RESET`/`MULT` (time control) and ADR 0034 added
`FEED`/`PLAY`/`REST`/`BATH`/`FLUSH`/`JUMP` (care actions and stage jump).
Both ADRs explicitly decided NOT to gate their new commands behind
`KF_DBG_INPUT_INJECT_ENABLE` -- correctly, by that flag's own definition:
none of the nine are button injection. But neither ADR introduced a new
gate for them either, reasoning (ADR 0034's words) that "`RESET` is
already a far more drastic state mutation than any single care action...
so gating care/jump more tightly than `RESET` while leaving `RESET` itself
ungated would be an inconsistent line to draw." True as stated, but it
side-stepped the actual question: **all nine of those commands were
ungated**, reachable with nothing more than `KF_DBG_BRIDGE_ENABLE=1`,
regardless of `KF_DBG_INPUT_INJECT_ENABLE`'s setting.

The project owner named the consequence directly: switching off button
injection alone gave false assurance. A build with
`KF_DBG_INPUT_INJECT_ENABLE=0` could not press a button remotely, but a
serial cable could still call `KFDBG FEED`/`KFDBG JUMP` and refill a
neglected pet's needs, or jump it straight to adult -- for a virtual pet
whose entire premise is that time and care are real, that is the ultimate
cheat. The two-flag boundary drawn in ADR 0030 (button injection vs.
everything else) did not match where the actual risk was (state mutation
vs. observation).

## Decision

### 1. A third flag, `KF_DBG_MUTATE_ENABLE`, gating every command that changes the pet or the simulation

The owner's own framing, adopted directly: split by observe vs. mutate.

- **`KF_DBG_BRIDGE_ENABLE`** now gates the read-only commands specifically:
  `PING`, `SHOT`, `STATE`, `SCANLINE`, `VSYNC` (plus `WATCH`, which is not
  its own wire command -- `tools/kf_debug.py`'s `watch` is repeated
  `STATE` polling). It remains the master switch too: with it off, nothing
  below matters, same as ADR 0030.
- **`KF_DBG_MUTATE_ENABLE`** (new) gates every command that changes the pet
  or the simulation: `FEED`, `PLAY`, `REST`, `BATH`, `FLUSH`, `JUMP`,
  `ADVANCE`, `RESET`, `MULT`, `BTN`, `BTNHOLD`. Off, each replies `err`
  instead of running -- see "Decision" #3.
- **`KF_DBG_INPUT_INJECT_ENABLE`** keeps its name and its job (narrowing
  `BTN`/`BTNHOLD` specifically) but now nests inside
  `KF_DBG_MUTATE_ENABLE` rather than the bridge flag directly -- see
  "Decision" #2 for why it survives at all.

The owner's shorthand for the new flag, `KF_DBG_MUTATE_ENABLE`, is used
as-is: it names exactly what it gates (state mutation), matches this
file's own existing naming (`KF_DBG_BRIDGE_ENABLE`, `KF_DBG_INPUT_
INJECT_ENABLE` both name their gated surface, not an implementation
detail), and no alternative surfaced during implementation that said
anything more precisely.

All three default `1`. `KF_DBG_MUTATE_ENABLE`'s default expression is
`KF_DBG_BRIDGE_ENABLE` (tracks the master switch, same pattern
`KF_DBG_INPUT_INJECT_ENABLE` already used); `KF_DBG_INPUT_INJECT_ENABLE`'s
default expression changes from `KF_DBG_BRIDGE_ENABLE` to
`KF_DBG_MUTATE_ENABLE`, so the `#ifndef` chain in `kf_dbg_bridge.h` has to
define `KF_DBG_MUTATE_ENABLE` before `KF_DBG_INPUT_INJECT_ENABLE` reads
it as a default.

### 2. `KF_DBG_INPUT_INJECT_ENABLE` is kept, not removed -- nested one level deeper

Two shapes were open once mutation as a whole had its own flag: fold
`KF_DBG_INPUT_INJECT_ENABLE` away entirely (BTN/BTNHOLD are now just two
of the eleven commands `KF_DBG_MUTATE_ENABLE` already covers), or keep it
as a narrower switch nested inside the new one.

**Kept, nested inside `KF_DBG_MUTATE_ENABLE`.** Two reasons:

1. **Removing it is a breaking change to a documented, supported
   configuration.** ADR 0030's own "Shipping a build with this off"
   section, and `ports/esp32/main/CMakeLists.txt`'s comment, both name
   `KF_DBG_INPUT_INJECT_ENABLE=0` as a deliberate, supported build for
   "PING/SHOT/STATE without remote button control." Anyone who set that
   macro explicitly would see it silently stop being read (or fail to
   compile, if this file no longer defined it) with no error pointing at
   why. Keeping the name and giving it a `#ifndef` default that still
   resolves to something meaningful costs nothing and breaks nothing.
2. **Button injection is a strictly larger attack surface than every other
   mutating command, even now that they share a gate.** `FEED`/`PLAY`/
   `REST`/`BATH`/`FLUSH`/`JUMP`/`ADVANCE`/`RESET`/`MULT` each has one
   bounded, enumerable effect on the pet -- reading `kf_dbg_bridge.cpp`
   tells you the complete list of what any of them can do. A `BTN` mask
   can drive **any** UI flow reachable by button presses, including menu
   screens this file has never heard of and never will need to, because
   `kf_dbg_input_mask()` is OR'd into the real GPIO read
   (`esp_input.cpp`) upstream of every screen's own input handling, not
   routed through a fixed set of session functions the way the other ten
   commands are. A build that wants a support technician to be able to
   feed a pet or fast-forward its clock remotely, without also handing a
   serial connection the ability to navigate to any screen a physical
   button could reach, is a real, narrower configuration -- and it is
   exactly what `KF_DBG_MUTATE_ENABLE=1` + `KF_DBG_INPUT_INJECT_ENABLE=0`
   now expresses precisely.

The alternative -- one flag, no nesting -- was rejected because it throws
away that second configuration for no benefit: nothing about merging the
two flags makes the code simpler (the `#if`/`#else` split inside
`handle_btn()`/`handle_btnhold()` still has to exist either way, since
`g_inject_mask` et al. are declared under `#if KF_DBG_INPUT_INJECT_ENABLE`
specifically to avoid unused-variable warnings when button state has no
reader or writer -- see that block's own comment), and merging removes a
capability without anyone asking for it removed.

### 3. Enforced at dispatch, with a reply the host side can act on

`kf_dbg_bridge.cpp` adds `require_mutate_enabled(line)`, called as the
first line of each of the eleven mutating branches in
`process_command_line()`: `if (!require_mutate_enabled(line)) { return; }`.
When `KF_DBG_MUTATE_ENABLE` is `0`, it replies `err` naming the flag
(`"mutating KFDBG commands are disabled on this build -- set
KF_DBG_MUTATE_ENABLE=1 to re-enable..."`) and returns `false`, so the
caller bails before parsing arguments or touching the pet. This was
chosen over the alternative BTN/BTNHOLD already used at the
`KF_DBG_INPUT_INJECT_ENABLE` layer -- an `ack` reply with a "(disabled)"
note baked into otherwise-normal-looking success text -- because a
rejected mutation should read as a rejection, not a success with an easy-
to-miss caveat; `err` is also what every other invalid-input path in this
file already replies (`parse_care_variation()`'s own `reply_err()` calls,
the `MULT` range check), so mutate-gate rejection matches the shape a
`kf_debug.py` user already knows to expect from a malformed command.

`tools/kf_debug.py`'s `_expect()` (shared by every `cmd_*()` that can send
a mutating command) already turns any `err` reply into `KfDebugError(f"device
rejected \`{command}\`: {payload}")` -- no host-side code change was
needed to make the rejection comprehensible, only a docstring on
`_expect()` explaining why, and a new `tools/kf_debug_selftest.py` check
(`test_mutate_gate_rejection_is_actionable()`) proving a canned `err`
reply naming `KF_DBG_MUTATE_ENABLE` surfaces as a `KfDebugError` carrying
that exact text, for both `cmd_care()` and `cmd_jump()` -- confirming the
mechanism is generic across commands, not something each `cmd_*()` has to
implement for itself.

`kf_dbg_bridge_init()`'s startup log line grows a `mutate %s` field
alongside the existing `input inject %s`, so a device's serial log states
both flags' settings at boot, not just one.

### 4. The gate is a runtime check on a compile-time constant, not an `#if`

`require_mutate_enabled()` reads `kMutateEnabled`, a `constexpr bool`
initialized from `KF_DBG_MUTATE_ENABLE` -- a compile-time constant, but
checked with a plain `if`, not wrapped in `#if`/`#else` at each of the
eleven call sites the way `KF_DBG_INPUT_INJECT_ENABLE` wraps
`handle_btn()`/`handle_btnhold()`'s bodies. This does NOT make the eleven
`handle_*()` functions this flag guards vanish from the binary the way
`KF_DBG_BRIDGE_ENABLE=0` makes the whole file vanish -- confirmed by `nm`
against a real `KF_DBG_MUTATE_ENABLE=0` build (see "Verified"):
`handle_feed()`/`handle_advance()`/etc. are all still present as local
symbols in the compiled object file, because `process_command_line()`'s
eleven-way `if`/`else` chain is one large function and the compiler does
not fold the constant far enough to prove each individual handler call
unreachable at this optimization level. Functionally this makes no
difference -- every one of those calls is unconditionally preceded by the
early return -- but it is worth being honest that this is "unreachable,
still linked in," the same shape `KF_DBG_INPUT_INJECT_ENABLE=0` already
has for `BTN`/`BTNHOLD` (those two functions stay linked too; only their
`#if`/`#else`-swapped bodies change), not "removed" the way the whole-file
`#if` achieves. A single runtime check on a compile-time constant was
chosen over duplicating `#if`/`#else` at eleven call sites for the same
reason ADR 0031 gave for not widening a flag instead of splitting one:
less code to keep in sync, at a small, measured cost in flash (+~250
bytes total for this whole ADR -- see "Verified") rather than zero.

## What this slice does NOT reach

- **`hakoniwaos/` is untouched.** Nothing about this gate needed to reach
  Core: every mutating KFDBG command already calls a `kf_pet_session_*()`
  wrapper that Core itself does not know or care is gated; the gate lives
  entirely in `kf_dbg_bridge.cpp`'s dispatch, one layer above.
- **`tools/kf_panel.py` is untouched** -- same boundary ADR 0031/0034 both
  named for their own new commands. A GUI built on top of `kf_debug.py`
  inherits the comprehensible-rejection behaviour for free through
  whichever `cmd_*()` functions it calls, same as any other host-side
  caller.
- **No change to which commands exist, their arguments, or their wire
  syntax.** This ADR only changes whether eleven already-existing commands
  run, never what they do when they do run.

## Verified

- **Desktop build.** `cmake --build build -j8` clean, zero new warnings.
  `ctest --test-dir build`: **37/37** (unchanged baseline -- this slice
  does not touch anything the desktop target compiles).
- **`tools/kf_debug_selftest.py`**, extended with
  `test_mutate_gate_rejection_is_actionable()`: a `FakeLink` returning a
  canned `err` reply naming `KF_DBG_MUTATE_ENABLE`, fed through both
  `cmd_care()` and `cmd_jump()`, confirms `_expect()` raises `KfDebugError`
  carrying that exact text -- not a timeout, not a bare `ProtocolError`.
  All checks pass (existing checks plus five new ones).
- **`idf.py build` clean for esp32s3 against the real ESP-IDF v6.0.2
  install, zero warnings under this project's existing `-Wall -Wextra
  -Werror` build, in two configurations, each a full `set-target esp32s3`
  + `build`:**
  1. Default (`KF_DBG_BRIDGE_ENABLE=1`, `KF_DBG_MUTATE_ENABLE=1`,
     `KF_DBG_INPUT_INJECT_ENABLE=1`). `kamiframe-firmware.bin`:
     **660,336 bytes** (0xa1370), 58% of the app partition free -- up from
     ADR 0034's 660,112-byte baseline by +224 bytes, all in flash (eleven
     new `require_mutate_enabled()` call sites, the function itself, its
     error string, and the new `mutate %s` log field).
  2. `KF_DBG_MUTATE_ENABLE=0` (added temporarily to
     `ports/esp32/main/CMakeLists.txt`'s `target_compile_definitions`,
     built, then removed -- not a permanent change, since a dev build
     keeps all three flags on by default). Builds clean, zero warnings.
     `kamiframe-firmware.bin`: **660,368 bytes** (0xa1390), +32 bytes over
     the default build above, NOT smaller -- see "Decision" #4 for why
     that is expected (the guarded handlers stay linked, just
     unreachable) and confirmed by `nm` against
     `kf_dbg_bridge.cpp.obj` from that build: `handle_feed`,
     `handle_advance`, `handle_reset`, `handle_mult`, `handle_btn`,
     `handle_btnhold`, `handle_play`, `handle_rest`, `handle_bath`,
     `handle_flush`, `handle_jump`, and `require_mutate_enabled` itself
     are all still present as local (`t`) symbols in the object file.
  - Both configurations were built from a full `fullclean` +
    `set-target esp32s3` + `build`, per this project's own standard for
    verifying a compile-time gate compiles in both states (a gate only
    ever compiled in one state is half-tested).

## Not verified

- **Nothing in this ADR has run against real hardware.** No board is
  reachable from this environment (CLAUDE.md's "no hardware yet" rule).
  Every claim above is a clean cross-compile, an `nm` symbol check, or a
  host-side unit test against a fake transport -- never a `KFDBG FEED`
  actually sent over a physical UART to a device that then replied `err`.
- **`KF_DBG_INPUT_INJECT_ENABLE=0` nested under `KF_DBG_MUTATE_ENABLE=1`**
  (care/time control on, button injection off) was not built as its own
  third configuration -- only the two configurations "Verified" lists
  (both flags on; mutate off) were actually compiled. The nesting itself
  is unchanged code (`handle_btn()`/`handle_btnhold()`'s own `#if
  KF_DBG_INPUT_INJECT_ENABLE` bodies, confirmed compiling clean at that
  flag's off setting by ADR 0030's own "Verified" section), so this is a
  low-risk gap, not a zero-risk one.
- **`tools/kf_panel.py` does not yet expose whether mutation is gated** --
  out of scope for this task, same as ADR 0031/0034's identical notes
  about their own new commands.

## Cost to change

**Adding a twelfth mutating command:** identical shape to the eleven
above -- one `else if` in `process_command_line()`, `if (!require_mutate_
enabled(line)) { return; }` as its first line, then the handler's normal
argument parsing and dispatch.

**Un-splitting `KF_DBG_MUTATE_ENABLE` back into `KF_DBG_BRIDGE_ENABLE`:**
would reintroduce exactly the gap this ADR closes -- a serial cable able
to cheat the pet's care/time economy whenever the bridge is on at all,
regardless of any narrower flag's setting. Not a one-line revert with no
cost: it is un-deciding the reason this ADR exists.

**Removing `KF_DBG_INPUT_INJECT_ENABLE` entirely, folding BTN/BTNHOLD
fully under `KF_DBG_MUTATE_ENABLE`:** one `#ifndef` block deleted from
`kf_dbg_bridge.h`, the `#if KF_DBG_INPUT_INJECT_ENABLE` bodies in
`handle_btn()`/`handle_btnhold()` collapsed to their enabled form, the
`g_inject_mask` et al. globals un-guarded. Cheap mechanically, but see
"Decision" #2 for why this was rejected: it is a breaking change to a
documented configuration for no code-simplicity gain.
