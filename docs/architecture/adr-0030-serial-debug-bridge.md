# ADR 0030: The KFDBG serial debug bridge

**Status:** Accepted
**Date:** 2026-08-08

## Context

Everything since ADR 0025 has run on a real ESP32-S3, but "run" has meant
"compiled and linked against real ESP-IDF" far more often than "watched
doing the right thing" -- the pet screen has never been seen on glass, and
every first-flash verification anyone can do today is either photograph the
panel or read `KF_LOGI` lines scrolling past in `idf.py monitor`. Neither
scales to an AI assistant working on this code with no hands and no camera,
and photographing a screen is a bad workflow for a human too.

A parallel task built the host half of a fix for this: `tools/kf_debug.py`,
a small script that talks to the device over the exact same UART
`idf.py monitor` already uses, and can pull a screenshot, read the pet's
live state, and press buttons remotely. It ships with its own selftest
(`tools/kf_debug_selftest.py`, all passing) that proves the host-side
parser, RLE decoder, base64 handling and PNG writer are correct against a
synthetic frame built by hand. What it cannot prove is that the device
speaks the same protocol -- that requires this ADR's half, and until this
lands, "one side is left to debug once hardware exists" (that file's own
words) still means both sides.

This ADR is the device half: `ports/esp32/main/kf_dbg_bridge.{h,cpp}` and
`ports/esp32/main/kf_dbg_codec.{h,cpp}`.

### A wire format already fixed, not designed here

The protocol -- command grammar, the `KFDBG-BEGIN/END` framing, base64 and
CRC32 -- was fixed by the task that started this slice, and by
`tools/kf_debug.py`/`tools/kf_debug_selftest.py`, which existed and passed
before a line of this ADR's code was written. One real ambiguity surfaced
mid-implementation and was resolved by reading the host source directly
rather than guessing: whether base64 wraps every reply type or only `fb`.
`tools/kf_debug_selftest.py`'s own module docstring already documents the
answer and the reasoning -- **every** reply type is base64 inside the
frame, `<length>` is always the base64 character count, and the CRC32
always covers the *decoded* (post-base64) bytes, an RLE stream for `fb`
and plain UTF-8 text for everything else. This ADR's implementation, and
every worked example below, was built and independently re-verified
against that exact reading, not the earlier (single-type) one -- see
"Verified" for how.

## Decision

### 1. Two low-priority FreeRTOS tasks, not a non-blocking poll in the main loop

The spec allowed either shape. Reading alone would have worked fine as a
non-blocking poll inside `app_main.cpp`'s loop. Writing is what forces a
background task: `KFDBG SHOT`'s reply is a framebuffer, RLE-compressed
then base64'd, and even a well-compressed real screen (~2KB base64, see
the worked example below) takes tens of milliseconds to clock out at this
bridge's fixed 115200 baud -- an incompressible one (the protocol's own
explicit worst case) would take **tens of seconds**. `uart_write_bytes()`
blocks once the driver's TX path can't keep up, and calling that inline
from the frame loop would stall every frame after a SHOT command by
however long the transmit took -- exactly the stall the spec forbids, and
for far longer than the read side alone would ever cause.

So the split is not "one task does I/O", it is "the slow half of I/O never
runs on the frame-loop thread, at all":

- **`kf_dbg_bridge_frame()`**, called once per iteration from
  `app_main.cpp`'s loop, does a bounded, non-blocking amount of work: pop
  at most one already-assembled command line off a queue, and if there is
  one, build its reply. This runs on the main thread on purpose -- SHOT
  reads `kf_fb_pixels()` and STATE reads `kf_pet_session_state()`, both of
  which must not be read from a second thread while the frame loop is
  writing them. The finished reply, already fully encoded to bytes, is
  handed to a second queue. No UART write ever happens on this thread.
- **`kf_dbg_rx_task()`** and **`kf_dbg_tx_task()`**, two dedicated tasks at
  `tskIDLE_PRIORITY + 1` -- at or below the default main-task priority,
  never above it -- that own the UART for reading and writing
  respectively. Both block on I/O almost all the time (`uart_read_bytes()`
  with `portMAX_DELAY`, or a queue receive), which is exactly what a
  low-priority task should spend its time doing.

A consequence that fell out of this split rather than being the reason for
it: because command *parsing and execution* runs on the main frame-loop
thread, and the injected-input state (`KFDBG BTN`/`BTNHOLD`) is read by
`kf_input_poll()`, also on the main thread, that shared state needs no
lock at all. See `kf_dbg_bridge.h`'s "WHY A BACKGROUND TASK" comment and
`kf_dbg_input_mask()`'s own comment for the full reasoning.

### 2. `app_main.cpp`'s loop is restructured, not just appended to

`kf_dbg_bridge_frame()` has to run **before** `kf_app_frame()`, not after,
so a `KFDBG BTN`/`BTNHOLD` command already queued affects THIS iteration's
`kf_input_poll()` (called from inside `kf_app_frame()`, first thing) --
"for the next frame," as the protocol spec puts it, means the very next
one. That is incompatible with the previous `while (kf_app_frame()) { ... }`
shape, since the loop's own continuation condition IS the call that needs
something to run before it. The loop became:

```cpp
for (;;) {
    kf_dbg_bridge_frame();
    if (!kf_app_frame()) break;
    kf_pet_session_frame(0);
    ...
}
```

Same behaviour, same exit path (still unreachable on this backend, see
`app_main.cpp`'s own comment), one extra line above the old condition.

### 3. Input injection: OR, not replace, behind its own flag

`KFDBG BTN <mask>` and `BTNHOLD <mask> <ms>` set process-local state that
`kf_dbg_input_mask()` exposes; `esp_input.cpp`'s `kf_input_poll()` does
`mask |= kf_dbg_input_mask();` after reading the real GPIOs, never an
assignment -- a physical button press keeps working exactly as before even
while an injection is live, and core's debounce (`app.cpp`) sees one raw
mask from one source, not two to reconcile.

`BTN` is one-shot: the mask applies for exactly the next `kf_input_poll()`
call, then clears itself. `BTNHOLD` holds a mask until a monotonic
deadline (`kf_time_mono_us() + ms*1000`), checked and cleared lazily on
each poll once the deadline passes. A new `BTN` cancels a running
`BTNHOLD` and vice versa -- there is exactly one injected mask live at a
time, matching the protocol's own shape (it does not define combining two
outstanding holds).

### 4. Two compile-time flags, not one

`KF_DBG_BRIDGE_ENABLE` is the whole-bridge switch the task required: 0
compiles `kf_dbg_bridge_init()`/`_frame()`/`_shutdown()` down to empty
functions and `kf_dbg_input_mask()` down to an unconditional `return 0`,
with **nothing** of the implementation left in the translation unit to be
unused -- see `kf_dbg_bridge.cpp`'s own header comment on why the file is
structured as one large `#if`/`#else` rather than four small ones (a real
mistake caught by actually building the disabled configuration, not
assumed correct -- see "Verified").

`KF_DBG_INPUT_INJECT_ENABLE` (default: tracks the flag above) is narrower:
it turns off only `BTN`/`BTNHOLD`'s effect on the real buttons, while
`PING`/`SHOT`/`STATE` keep working. That split exists because those three
are read-only introspection -- nothing a KFDBG connection could learn that
watching the screen couldn't -- while button injection is remote control
of the device. A build that wants a screenshot tool in the field without
handing a serial connection the ability to drive the pet can keep the
first and drop the second.

**Shipping a build with this off:** add, in
`ports/esp32/main/CMakeLists.txt`, next to the existing
`target_compile_definitions(${COMPONENT_LIB} PRIVATE ...)` block:

```cmake
target_compile_definitions(${COMPONENT_LIB} PRIVATE KF_DBG_BRIDGE_ENABLE=0)
```

(or `KF_DBG_INPUT_INJECT_ENABLE=0` alone, for the narrower cut). Both
default ON in `kf_dbg_bridge.h` today, correct for this pre-release
developer firmware and nothing further downstream depends on that
default -- there is no other file to touch.

### 5. CRC32: IEEE 802.3 / zlib polynomial

`0xEDB88320` (reflected), init `0xFFFFFFFF`, final XOR `0xFFFFFFFF` -- the
exact algorithm `zlib.crc32()` implements, which is what
`tools/kf_debug.py` calls on the decoded payload. Chosen for that
ubiquity, not novelty: the host side needed to write or import nothing
bespoke. `kf_dbg_codec.cpp` builds its lookup table with a `constexpr`
constructor from the bit-at-a-time definition rather than a
hand-transcribed table, on the theory that a transcription error is
exactly the kind of subtly-wrong mistake this whole file exists to avoid.

### 6. `kf_dbg_codec.{h,cpp}` has zero ESP-IDF dependency, on purpose

RLE, base64 and CRC32 are three independent, well-specified algorithms
with an unambiguous right answer, which is exactly the shape of bug that
"looks like a rendering bug" if subtly wrong -- the task's own warning.
Keeping the codec free of `<freertos/...>`/`<driver/...>` headers is what
let it be compiled as an ordinary host program (`g++ -std=c++17 -Wall
-Wextra -Werror`) and checked against known-answer vectors before a single
byte of it ran anywhere near the firmware -- see "Verified".
`kf_dbg_bridge.cpp` is where the ESP-IDF-specific wiring (the UART, the
two tasks, the five command handlers) lives instead.

### 7. Reply buffers come from PSRAM, outside the arena/budget system

`kf/budget.h`'s arenas (framebuffer, scratch, Lua, LVGL, assets) are a
closed, audited set that has to fit the real device -- see ADR 0006 and
ADR 0008. This bridge's reply buffers (`kf_dbg_build_reply()`'s base64
buffer and final frame, `handle_shot()`'s worst-case RLE buffer) are a
debug facility's transient allocations, sized for the true worst case
(`pixel_count * 4` for RLE, `ceil(n/3)*4` for base64) and freed
immediately after the reply is handed off or sent. They use
`heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` directly, deliberately outside
the arena system: this code does not exist in a build with
`KF_DBG_BRIDGE_ENABLE=0`, so it has no business claiming a permanent share
of a budget the rest of the firmware has to live inside forever.

### Superseded in part

ADR 0035 adds a third flag, `KF_DBG_MUTATE_ENABLE`, and moves the boundary
this ADR drew. At the time this ADR was written, `PING`/`SHOT`/`STATE` and
`BTN`/`BTNHOLD` were the entire protocol, so "one flag for the whole
bridge, a narrower one for button injection" was a complete split. ADR
0031 and ADR 0034 then added nine more commands
(`ADVANCE`/`RESET`/`MULT`/`FEED`/`PLAY`/`REST`/`BATH`/`FLUSH`/`JUMP`) that
change the pet or the simulation without adding a gate narrower than this
ADR's whole-bridge switch for any of them, which meant `KF_DBG_INPUT_
INJECT_ENABLE=0` alone no longer gave "read-only" -- it gave "no button
injection, but every other mutation still reachable." `KF_DBG_MUTATE_
ENABLE` (ADR 0035) is the fix: it gates all eleven mutating commands
together, and `KF_DBG_INPUT_INJECT_ENABLE` now nests inside it rather than
inside `KF_DBG_BRIDGE_ENABLE` directly. The reasoning above for why
`KF_DBG_INPUT_INJECT_ENABLE` exists at all -- button injection is a
strictly larger surface than the introspection commands -- still holds and
is not superseded; only which flag it nests inside changed.

## What this slice does NOT reach

- **Never run against a real device.** Every claim below about the wire
  format is verified against the codec running natively and against the
  real `tools/kf_debug.py`/`kf_debug_selftest.py` host code (see
  "Verified") -- not against a UART with a board on the other end, because
  there is no board flashable from this environment. First real exercise
  happens at the next hardware session.
- **No flow control, no retransmission.** A dropped or partial line is a
  discarded command (logged, not replied to) or a CRC mismatch the host
  already knows how to report (`CrcMismatchError`) and retry. Fine for a
  debug facility; not a protocol a production feature should reuse
  unmodified.
- **The reply queue depth (2) assumes a well-behaved host.**
  `tools/kf_debug.py`'s own `SerialLink` never has more than one command
  in flight -- it sends, then blocks reading the full frame before sending
  again. A host that pipelines commands could see replies silently dropped
  (logged as a warning) rather than queued indefinitely.
- **`uart_driver_install()` shares the console UART without reconfiguring
  it** -- see "Not verified" below for the one real risk this leaves open.

## Alternatives considered

**Encode straight into the destination frame buffer, skipping the
intermediate base64 buffer in `kf_dbg_build_reply()`.** Would save one
allocation and one copy per reply. Rejected for this slice: it means
interleaving base64's 3-bytes-in/4-chars-out cadence with the frame's own
76-char line wrapping in one pass, which is a real chance to get the
boundary arithmetic wrong in a way that produces a plausible-looking but
corrupted frame -- precisely the failure mode the task's own warning calls
out. Two buffers and a copy is measurably slower and is not on any
latency-sensitive path (nothing here runs inside the frame budget) --
correctness proven by a straightforward implementation was worth more
than the saved allocation.

**A single task doing both read and write.** Would need to interleave a
blocking UART read with checking a reply queue -- either polling both with
a timeout (adding latency to command echo) or using `uart_read_bytes()`'s
own event-queue mode to multiplex, which is more moving parts for no
benefit: reading and writing this UART are independent directions with
independent blocking behaviour, and two simple tasks are easier to reason
about than one task juggling both.

**Static task/queue allocation (`xTaskCreateStatic`/`StaticQueue_t`)**
instead of the heap-backed `xTaskCreate`/`xQueueCreate` used here. Real
devices that run for months generally prefer static allocation to avoid
long-run heap fragmentation (the same reasoning `kf/hal/memory.h`'s "no
free, ever" pool contract already documents for the core arenas).
Rejected here specifically because this is compile-time removable
(`KF_DBG_BRIDGE_ENABLE=0`) debug scaffolding, allocated exactly once at
boot and never freed for the life of the process either way -- the
fragmentation risk a static allocation avoids does not apply to a
one-time, non-repeating allocation. Worth reconsidering if this bridge
ever stops being removable.

## Worked examples

Built with the actual linked codec (`kf_dbg_codec.cpp`, unmodified),
verified end-to-end against the real `tools/kf_debug.py::read_frame()` and
`decode_rle()` -- see "Verified" for exactly how. `PING`'s build date is a
placeholder (`__DATE__`/`__TIME__` are compiled in, not shown here) and
the panel name is whichever `KF_PANEL_PROFILE` this build was compiled
against (`ILI9341 (HiLetgo 2.8in)` below, the current default per ADR
0029).

**`KFDBG PING`** -> decoded payload `"Aug  8 2026 00:00:00\nILI9341
(HiLetgo 2.8in)"` (two lines, build date then panel name):

```
KFDBG-BEGIN pong 60
QXVnICA4IDIwMjYgMDA6MDA6MDAKSUxJOTM0MSAoSGlMZXRnbyAyLjhpbik=
KFDBG-END f9e31130
```

**`KFDBG STATE`** -> decoded payload (one line, shown here reformatted for
this document only -- the wire payload has no embedded whitespace):

```json
{"stage":2,"hunger_mp":785340,"happiness_mp":912110,"energy_mp":773200,
 "base_trait":3,"stage_elapsed_s":184,"heap_free_internal":142880,
 "heap_free_psram":6291456,"fps":29.8,"frame_us":1830}
```

```
KFDBG-BEGIN json 252
eyJzdGFnZSI6MiwiaHVuZ2VyX21wIjo3ODUzNDAsImhhcHBpbmVzc19tcCI6OTEyMTEwLCJlbmVy
Z3lfbXAiOjc3MzIwMCwiYmFzZV90cmFpdCI6Mywic3RhZ2VfZWxhcHNlZF9zIjoxODQsImhlYXBf
ZnJlZV9pbnRlcm5hbCI6MTQyODgwLCJoZWFwX2ZyZWVfcHNyYW0iOjYyOTE0NTYsImZwcyI6Mjku
OCwiZnJhbWVfdXMiOjE4MzB9
KFDBG-END 2c33684b
```

Field names are a firmware-side choice (the protocol spec does not fix
them, and `tools/kf_debug.py` prints whatever keys arrive): `stage` is the
raw `kf_pet_stage` enum value, `stage_elapsed_s` is
`kf_pet_state::stage_elapsed_seconds` ("time in stage"), `heap_free_*`
come from `heap_caps_get_free_size()`, and `fps`/`frame_us` come from
`kf_app_frame_summary()`/`kf_app_last_frame()->cpu_us` -- the same budget
accounting ADR 0006 already computes every frame, not a new measurement.

**`KFDBG BTN 5`** (mask 5 = `0b101` = `KF_BTN_UP | KF_BTN_LEFT`, per the
`kf_button` enum in `kf/types.h`: `UP=1<<0`, `LEFT=1<<2`) -> decoded
payload `"BTN mask=5"`:

```
KFDBG-BEGIN ack 16
QlROIG1hc2s9NQ==
KFDBG-END b4baa385
```

**`KFDBG FROBNICATE`** (unrecognised) -> decoded payload `"unknown KFDBG
subcommand: KFDBG FROBNICATE"`:

```
KFDBG-BEGIN err 56
dW5rbm93biBLRkRCRyBzdWJjb21tYW5kOiBLRkRCRyBGUk9CTklDQVRF
KFDBG-END 482725b7
```

**`KFDBG SHOT`**, against a synthetic 240x320 HUD-shaped test image (a
black bar for the top 24 rows, a solid rectangle standing in for a
sprite/panel, white everywhere else -- not a real render, but the same
shape of image a real screen produces: mostly a few large solid regions,
not per-pixel noise): raw framebuffer 153,600 bytes -> RLE 1,600 bytes ->
base64 2,136 characters (29 payload lines). Full frame (29 lines omitted
here; verified byte-for-byte through the real RLE decoder, see
"Verified"):

```
KFDBG-BEGIN fb 2136
<29 lines of base64, 76 chars each except the last>
KFDBG-END 1e958dac
```

A genuinely incompressible 240x320 frame (uniform random noise, the
protocol's own stated worst case) would RLE-expand to 307,200 bytes and
base64 to 409,600 characters -- at 115200 baud that is roughly 35 seconds
of wire time, past `tools/kf_debug.py`'s own 30-second `SHOT_TIMEOUT`.
This is a real, known limit, not an oversight: the protocol spec itself
says "UI screens are highly compressible", and every real screen this
firmware draws (HUD text, sprites, solid backgrounds) is far closer to the
worked example above than to noise -- see "Not verified".

## Verified

- **`kf_dbg_codec.{h,cpp}` built and run as a native host program**
  (`g++ -std=c++17 -Wall -Wextra -Werror -O1`, zero warnings), against:
  - The standard CRC-32 known-answer vector, `CRC32("123456789") ==
    0xCBF43926`, plus `CRC32("") == 0` and the init/update/final split
    form matching the one-shot form across an arbitrary byte boundary.
  - Every base64 test vector from RFC 4648 itself (`""`, `"f"`, `"fo"`,
    ..., `"foobar"`), plus an undersized-output-buffer check.
  - RLE round-tripped against an independent reference decoder
    (mirroring `tools/kf_debug.py`'s own `decode_rle()`) on four cases: a
    highly compressible banded image, a worst-case fully-alternating
    1000-pixel buffer (verifies `out_len == pixel_count * 4`, the true
    worst case `handle_shot()` sizes its buffer to), a 70,000-pixel
    single-colour run (verifies the 65535-count cap splits into exactly
    two records rather than ever writing a wider count field), and an
    undersized-output-buffer check.
  - A realistic 240x320 HUD-shaped image end to end: RLE round-trips
    exactly, base64 writes exactly the predicted length, and the result
    (2,136 base64 chars) is confirmed far smaller than the raw 153,600
    bytes.
- **Cross-checked against Python's own stdlib**, independently of the
  native test program above: `zlib.crc32(b"123456789")` and
  `base64.b64encode(b"foobar")` match the C++ output exactly.
- **Every worked example above fed through the real, unmodified
  `tools/kf_debug.py`** (`kf_debug.read_frame()`, plus `decode_rle()` for
  the `fb` case) via a fake `readline` -- the identical technique
  `tools/kf_debug_selftest.py` uses against its own synthetic frames, but
  run here against frames this ADR's own codec produced:
  - `pong`/`json`/`ack`/`err` all decode to exactly the intended text,
    CRC32 verified.
  - `fb` decodes, RLE-decompresses to exactly 153,600 bytes, and three
    spot-checked pixels (HUD bar, panel rectangle, background) all match
    the source image exactly.
- **`idf.py build` clean for esp32s3 against ESP-IDF v6.0.2, zero
  warnings under `-Wall -Wextra -Werror`**, in three configurations, each
  a full `fullclean` + `set-target esp32s3` + `build`:
  1. Default (`KF_DBG_BRIDGE_ENABLE=1`, `KF_DBG_INPUT_INJECT_ENABLE=1`).
     `kamiframe-firmware.bin`: **647,419 bytes** total image (0x9e170),
     38% of the 1MB app partition free. Internal RAM (DIRAM): **88,073
     bytes** (25.77% of the 341,760-byte pool), IRAM at its fixed 16,384
     bytes (100%, unrelated to this change -- see below).
  2. `KF_DBG_BRIDGE_ENABLE=0`. Confirmed the flag actually removes the
     bridge rather than merely disabling it at runtime -- and caught a
     real bug doing so: the first cut of `kf_dbg_bridge.cpp` only wrapped
     the four public functions' bodies in `#if KF_DBG_BRIDGE_ENABLE`,
     leaving every static helper, task and global at file scope
     unconditional. That compiled -- ESP-IDF's own default flags exempt
     `-Wunused-function`/`-Wunused-variable` from `-Werror` (visible in
     the component's own compile command) -- but produced seven
     warnings, which is not "zero warnings" by this project's own
     standard even though it is zero *fatal* warnings. Restructured so
     the entire implementation lives inside one `#if KF_DBG_BRIDGE_ENABLE`
     block with a four-empty-function `#else`; rebuilt clean, zero
     warnings, confirmed by `grep -i warning` over the full build log
     finding nothing.
  3. `KF_DBG_INPUT_INJECT_ENABLE=0` alone (bridge on, injection off).
     Same zero-warnings check, confirming the narrower flag is
     independently clean too -- the injected-mask globals are declared
     inside their own nested `#if`, not just unused.
  - Restoring the default configuration and rebuilding from a full clean
    reproduced the exact same 647,419-byte image, confirming the
    restructure changed nothing about the enabled path's generated code.
- **Flash/RAM delta against the pre-bridge baseline**, measured by
  `git stash`-ing every change in this slice, rebuilding, then
  `git stash pop` and rebuilding again (both `idf.py size` runs, same
  build tree, same toolchain invocation): baseline 626,619 bytes total
  image / 85,229 bytes DIRAM; with the bridge, 647,419 / 88,073. **+20,800
  bytes flash (+3.3%), +2,844 bytes internal RAM (+3.3%)** for the whole
  bridge, at its default (fully enabled) configuration. Both tasks' stacks
  (3,072 bytes each, generous rather than measured -- see "Not verified")
  and both queues are heap-allocated at `kf_dbg_bridge_init()` time, not
  static, so they do not show up in this static DIRAM figure at all; see
  "Not verified" for what that means for the number that matters on a
  real device.

## Not verified

- **Nothing in this ADR has run against real hardware.** No board is
  reachable from this environment (see CLAUDE.md's "no hardware yet"
  architecture rule and the task's own instruction to say this
  explicitly). Every claim above is either a native-host test of the
  portable codec, a cross-check against the real host-side Python, or a
  clean cross-compile -- never a byte actually sent or received over a
  physical UART.

  **Flagged, not resolved, during a later documentation pass:** ADR 0032
  (one day later, 2026-08-09) opens "Once the pet was rendering on real
  hardware" and describes observing tearing get "obvious when the time
  multiplier is turned up" -- the time multiplier is exactly ADR 0031's
  `KFDBG MULT`, reachable only over this bridge. Whether that multiplier
  was actually driven live over the physical UART on real hardware by
  2026-08-09 (which would mean this claim was current for less than a day)
  or set some other way (a compile-time default, a desktop-only
  observation) is not established by either document. Left for Chris to
  confirm rather than guessed at here.
- **`uart_driver_install()` sharing the console UART with zero
  reconfiguration is the single biggest real risk here.** ESP-IDF's own
  startup already configures UART0 for `printf`/`KF_LOGx` output via a
  polling path before `app_main()` runs; `kf_dbg_bridge_init()`
  deliberately does not call `uart_param_config()`/`uart_set_pin()`,
  relying on that existing configuration and only adding the
  interrupt-driven driver on top for RX (and TX via `kf_dbg_tx_task()`).
  This is a well-established ESP-IDF pattern (installing the UART driver
  on the console port to read stdin alongside existing log output), but
  it has not been tried on THIS project's actual boot sequence, and
  whether ordinary `KF_LOGx` lines keep interleaving cleanly with KFDBG
  framed blocks on a real board -- rather than corrupting each other's
  bytes on the wire -- is exactly the kind of thing that either works
  perfectly or needs a real debugging session, with no useful middle
  ground reachable from here.
- **Both tasks' 3,072-byte stacks are sized generously, not measured.**
  There is no hardware to watch a real high-water mark on. Both spend
  nearly all their time blocked (see "Decision" #1), which bounds but
  does not eliminate risk -- `KF_LOGW`/`KF_LOGE` calls from
  `kf_dbg_rx_task()` and `snprintf`-heavy command handlers on the main
  thread are the deepest call stacks either path takes.
- **The reply queue's small depth (2) and the "drop and log" behaviour
  under backpressure are reasoned from `tools/kf_debug.py`'s own
  request/response discipline, not exercised against a live host
  actually doing that.**
- **The RLE compression ratio in the worked example (96x, a synthetic
  HUD-shaped test image) is illustrative, not a measurement of what the
  real pet screen or LVGL menu screens actually compress to.** Expect it
  to be good -- both are mostly large solid regions, exactly what RLE is
  for -- but "expect" is doing real work in that sentence until a real
  screen is captured.
- **The IRAM figure showing 100% (16,384/16,384) used in every
  configuration measured, including the pre-bridge baseline, is
  unrelated to this change** (confirmed by the baseline comparison
  itself: identical in both), but is worth someone's attention
  independently of this ADR -- an IRAM region already at its configured
  cap has no headroom for it before the next thing that needs some.

## Cost to change

**Turning the bridge off entirely, or narrowing it to read-only:** one
line each, in `ports/esp32/main/CMakeLists.txt` -- see "Decision" #4 and
that file's own comment for exactly where. Zero cost to any other file:
`app_main.cpp`'s calls into `kf_dbg_bridge_*()` and `esp_input.cpp`'s
`kf_dbg_input_mask()` call are unconditional either way, resolving to
empty functions and a constant `0` respectively when disabled.

**Adding a new `KFDBG` command:** one `else if` branch in
`process_command_line()`, one handler function following the existing
`handle_*()` shape (build a decoded-content byte string, call
`kf_dbg_enqueue_reply()`), no change to `kf_dbg_build_reply()`,
`kf_dbg_codec.{h,cpp}`, or either task -- the framing/base64/CRC machinery
is already generic across every reply type.

**Changing the wire format itself** (a different framing marker, a
different CRC polynomial, dropping base64 for `fb` specifically): touches
`kf_dbg_build_reply()` in one place on this side, and the exactly
corresponding logic in `tools/kf_debug.py`'s `read_frame()`/
`build_frame_lines()` (that file and `kf_debug_selftest.py`) on the other
-- genuinely two files to keep in step, which is the reason this ADR
leaned on reading the real host source directly rather than re-deriving
the spec from English prose partway through implementation (see
"Context").

**Moving command execution off the main thread** (if a future command
needed to be genuinely async, e.g. something slower than one frame):
would reintroduce exactly the race `kf_dbg_bridge.h`'s header comment
explains why command handling currently avoids -- `kf_fb_pixels()` and
`kf_pet_session_state()` are not safe to read from a second thread while
the frame loop writes them, and the injected-input state's lock-free
design assumes single-thread access. Not a one-line change; would need
either a lock around whichever state a slow command reads, or a
snapshot-and-hand-off design, decided when (if) a command actually needs
this.
