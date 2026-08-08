# tools/

Small standalone scripts that support the build. They are not part of the
firmware or the simulator.

- `check_no_heap.py` -- CI check that core never reaches for the heap.
- `make_font.py`, `make_test_sprite.py` -- generate checked-in asset headers.
- `kf_debug.py` -- talk to a real board over USB. Explained below.
- `kf_debug_selftest.py` -- proves `kf_debug.py` decodes the wire protocol
  correctly, without needing a board plugged in.

## kf_debug.py -- seeing and driving a real board without a camera

Normally, checking what's on the little screen means physically looking at
it, or photographing it. `kf_debug.py` is a plain command-line tool that
talks to the board over the same USB cable you'd use to flash it, and asks
the firmware directly for a screenshot, the pet's current stats, or to
simulate a button press. This is what lets someone (including an AI
assistant with no eyes and no hands) verify that a change to the firmware
actually works, on the actual hardware, without anyone standing next to the
device with a phone camera.

It talks over "serial" -- an old but still very normal way for a computer to
send small messages back and forth with a device over USB. You don't need
to understand it to use this tool; the plumbing is handled for you.

### One-time setup

```
pip install pyserial
```

That's the only thing this script needs beyond what's already on your
machine. If you skip this step, the script will tell you, clearly, the
moment it needs it -- it doesn't need it just to print `--help`.

### Before you run it

Close `idf.py monitor` (or any other program watching the same USB port,
including a second copy of this tool) first. Only one program can talk to
the board's serial port at a time -- if `idf.py monitor` still has it open,
`kf_debug.py` cannot get a word in, and the most likely symptom is a
timeout with no obvious cause.

### Basic use

```
python3 tools/kf_debug.py ping
python3 tools/kf_debug.py shot
python3 tools/kf_debug.py shot --out latest.png
python3 tools/kf_debug.py state
python3 tools/kf_debug.py state --json
python3 tools/kf_debug.py press UP
python3 tools/kf_debug.py press UP,A --hold-ms 300
python3 tools/kf_debug.py watch --interval 0.5
```

- `ping` -- confirms the board is alive and responding, and prints its
  firmware build date.
- `shot` -- takes a real screenshot straight from the display hardware and
  saves it as a PNG (default `kf_shot.png`). This is the pixels actually on
  the screen, not a simulation of them.
- `state` -- prints what the pet is currently doing (stage, hunger,
  happiness, energy, and some low-level health numbers like free memory and
  frame rate). Add `--json` if you want the raw machine-readable line
  instead of the plain-text summary.
- `press` -- simulates pressing one or more physical buttons, as if you'd
  pressed them yourself. Combine buttons with a comma (`UP,A`). Add
  `--hold-ms 300` to hold them down for that many milliseconds instead of a
  quick tap.
- `watch` -- prints `state` repeatedly, once per interval (default every
  second), so you can watch values change live. Stop it with Ctrl-C.

### Picking the port

The board shows up on your computer as a "serial port" -- on a Mac
something like `/dev/cu.usbserial-1420`. `kf_debug.py` looks for exactly one
port that looks like a USB-serial board and uses it automatically, telling
you which one it picked. If it finds none, or more than one candidate (for
example, two boards plugged in at once), it will list what it saw and ask
you to be specific:

```
python3 tools/kf_debug.py --port /dev/cu.usbserial-1420 shot
```

### If something goes wrong

- **"no reply from device"** -- almost always means something else already
  has the port open (see "Before you run it" above), or the board isn't
  plugged in, or it's plugged into a charge-only cable/port rather than a
  data one.
- **"CRC32 mismatch"** -- the screenshot or data got corrupted in transit
  (a noisy cable, a flaky USB hub). It is not a bug in what's on the
  screen. Just retry.
- `--verbose` prints every raw line sent and received, which is the thing
  to reach for if you need to see exactly what the board said.

### Testing this tool itself

There's no hardware in CI, so `kf_debug.py`'s protocol parser is tested
against a synthetic, hand-built reply instead of a real board:

```
python3 tools/kf_debug_selftest.py
```

This builds a fake screenshot, encodes it exactly the way the real
firmware is specified to, feeds it through the same parser `kf_debug.py`
uses, and checks the picture that comes back out is pixel-for-pixel
identical to the one that went in -- along with a few other checks (a
deliberately corrupted reply is caught, a silent board times out with a
useful message, and so on).
