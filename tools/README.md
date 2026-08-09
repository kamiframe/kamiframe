# tools/

Small standalone scripts that support the build. They are not part of the
firmware or the simulator.

- `check_no_heap.py` -- CI check that core never reaches for the heap.
- `make_font.py` -- generates the checked-in bitmap font header (still a
  baked-in header on purpose; see that file's own docstring).
- `kf_pack_assets.py` -- the asset pipeline's packer (ADR 0033): packs
  named sprites (and, later, audio clips) into a `.kfpack` file
  `kf/assets.h` loads at runtime. Supersedes the old `make_test_sprite.py`,
  which is gone.
- `kf_debug.py` -- talk to a real board over USB. Explained below.
- `kf_debug_selftest.py` -- proves `kf_debug.py` decodes the wire protocol
  correctly, without needing a board plugged in.
- `kf_panel.py` -- a window you can click and type into to drive a real
  board. Explained below.

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

## kf_panel.py -- a remote control for the hardware

`kf_debug.py` is a command you run once and get one answer back.
`kf_panel.py` is a window that stays open: a pad of buttons you click (or
control from the keyboard) that presses the real buttons on the board, a
box that shows the pet's live stats, and a button to grab a screenshot
whenever you want one. Think of it as a remote control, not a screenshot
tool.

The idea is you keep this window open next to the physical device (or
plugged into your laptop with the screen facing you) while you poke at
it, instead of typing a new `kf_debug.py` command every time you want to
press a button.

### Launching it

```
python3 tools/kf_panel.py
```

That auto-detects the board's serial port, the same way `kf_debug.py`
does, and shows a "Connect" button -- nothing happens to the board until
you click it. If you have more than one board plugged in, or the
auto-detect doesn't find it, there's a port dropdown next to Connect;
pick the right one and click Rescan if it's not listed yet.

To try the window with no hardware at all -- to see what it looks like,
or if you're changing something in this file and want to check it still
works -- there's a demo mode that fakes a whole device in software:

```
python3 tools/kf_panel.py --demo
```

In demo mode the window connects itself automatically, the button pad
moves a little coloured blob around a fake screen, and the state readout
shows made-up-but-plausible numbers that actually change when you press
buttons. It's not a real pet, just enough of a stand-in to prove the
window works.

### What's in the window

- **Buttons.** UP, DOWN, LEFT, RIGHT, A, B, and MENU, laid out like a
  real device's D-pad-plus-two-buttons-plus-menu. Click one for a quick
  tap; click and hold for a longer, timed press (the board tells the
  difference the same way a real finger would). The same seven buttons
  work from the keyboard too: arrow keys for the D-pad, Z and X for A and
  B, and Enter or Escape for MENU -- clicking with a mouse seven times in
  a row gets old fast.
- **Pet state.** Stage, hunger, happiness, energy, personality trait, how
  long it's been in this stage, plus some low-level health numbers (free
  memory, frames per second) -- refreshed automatically, about once a
  second by default. This reply is tiny, so refreshing it often doesn't
  slow anything else down. You can change how often it refreshes with the
  "State poll" box next to Connect.
- **Save Screenshot.** Grabs a real screenshot straight off the device
  and saves it as a PNG, the same as `kf_debug.py shot` does. Every click
  makes a new file with the time in its name (like
  `kf_shot_20260808_193355.png`) in your Downloads folder, so clicking it
  five times in a row while debugging gives you five separate files
  instead of overwriting the same one. The full path to the last file you
  saved is shown in a box you can click into and copy (Cmd+C / Ctrl+C) --
  handy for dragging or pasting the file into a chat with someone (or
  something) that isn't standing next to the device. There's also a
  keyboard shortcut, Cmd+S on a Mac or Ctrl+S elsewhere. A small preview
  of the last screenshot you took sits underneath the button, so you can
  tell at a glance you captured the moment you meant to.

  A screenshot takes a second or two to arrive -- the button will say
  "Capturing..." and grey itself out while that's happening. That's
  normal, not a freeze; the rest of the window (buttons, state readout)
  keeps working while it waits.
- **Simulator debug controls.** A greyed-out section for things like
  fast-forwarding the pet's clock and resetting it -- controls the
  desktop simulator's own debug window already has. The real board
  doesn't support any of this yet, so every control in this section is
  disabled; hover over one to see why. It's there so the window doesn't
  need to be redesigned later, once those commands exist.

There is deliberately **no live view of the screen itself** in this
window. The assumption is you're looking at the actual device screen (or
the simulator window) while you use this panel to drive it, so nothing
here polls the display in the background -- that would only compete with
button presses and state updates for no benefit. If you want a one-off
picture of what's currently on the screen, that's what Save Screenshot is
for.

### Picking what to connect to

By default `kf_panel.py` looks for a real board over USB, same as
`kf_debug.py`. You can be explicit with `--target`:

```
python3 tools/kf_panel.py --target serial:/dev/cu.usbserial-1420
python3 tools/kf_panel.py --target serial:auto     # same as the default
python3 tools/kf_panel.py --demo                   # no hardware at all
```

There's also a `sim:` form (`--target sim:localhost:9500`) reserved for
once the desktop simulator can speak this same protocol over the network
instead of a real board over USB -- it isn't built yet, so using it today
just gives you a clear "not implemented" message instead of a crash.

### If something goes wrong

Same causes as `kf_debug.py` -- see that section above. The window
reports connection and command failures in the status line along the
bottom rather than popping up dialogs, so if a button press or screenshot
doesn't seem to do anything, that's the first place to look.
