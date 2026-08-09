#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright the Kamiframe contributors.
"""A live control panel for Kamiframe hardware -- a window with a button
pad, a live pet-state readout, and an on-demand screenshot button, talking
to the device the same way kf_debug.py does.

This is not a screenshot tool. It's meant to sit open on screen next to
the physical device (or the desktop simulator, once it speaks this same
protocol -- see the Transport section below) while you drive it: press
buttons with the mouse or the keyboard, watch hunger/happiness/energy
change in near-real-time, and grab a screenshot to paste into a chat
whenever something looks wrong.

Why there's no live view of the screen itself: a full screenshot is a
slow, ~1.5-second transfer over the debug UART (see kf_debug.py's module
docstring), and the assumption here is you're looking at the physical
screen directly while this window drives it. Polling framebuffers on a
timer would only compete with button presses and state polls for no
benefit. The "Save Screenshot" button below still exists for handing a
frame to someone (or something) that isn't standing in front of the
device -- it just fetches on demand instead of continuously.

Reuses kf_debug.py for everything protocol-related: frame parsing,
base64, CRC32, RLE decode, RGB565->RGB888, port auto-detection, and PNG
writing. This file adds the window, the input handling, and a `Transport`
seam so the exact same UI can eventually drive the desktop simulator over
a socket instead of a board over serial (see the Transport classes below).

Usage:
    python3 tools/kf_panel.py                          # serial, auto-detect
    python3 tools/kf_panel.py --target serial:/dev/cu.usbserial-1420
    python3 tools/kf_panel.py --demo                    # no hardware needed

See tools/README.md for a plain-language walkthrough.
"""

import abc
import argparse
import base64
import json
import os
import queue
import struct
import sys
import threading
import time
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import kf_debug as kfd  # noqa: E402

# Shown in the window title. Bump it whenever the panel's appearance or
# controls change. This is not decoration: three separate rounds of "it still
# looks the same" turned out to be a stale window or an older copy of this
# file being run, and there was no way to tell by looking at it. Now there is.
PANEL_BUILD = "2026-08-08.2 light-theme"

try:
    import tkinter as tk
    from tkinter import ttk
except ImportError:
    tk = None
    ttk = None


# ==========================================================================
# Transport: what the UI needs from a KFDBG connection, independent of
# what's on the other end of it.
#
# Today that's a real board over USB serial. The plan (per CLAUDE.md's
# "no emulator" rule) is for the desktop simulator to speak this identical
# KFDBG protocol over a local TCP socket -- so this one panel can drive
# either a real board or the simulator with no change to the UI, the same
# way the firmware's own HAL swaps a display driver underneath identical
# drawing code. Everything above this section talks to a Transport;
# everything in this section implements one.
#
# The interface is deliberately thin -- send one line out, read one line
# back -- because kf_debug.read_frame() already knows how to turn a raw
# line source into a parsed, CRC-checked KFDBG frame. A Transport doesn't
# need to know anything about that framing; it only needs to move bytes.
# ==========================================================================

class Transport(abc.ABC):
    """One line out, one line back. Implementations own whatever is on the
    other end (a serial port, eventually a socket) and raise
    kfd.KfDebugError with a plain-English message if that fails."""

    @abc.abstractmethod
    def send_line(self, line):
        """Send one command line (no trailing newline needed)."""

    @abc.abstractmethod
    def readline(self):
        """Return one line as bytes (trailing newline optional), or b''
        if nothing arrived on this attempt -- see kfd.read_frame()'s
        docstring for why that particular contract matters."""

    def close(self):
        pass

    def __enter__(self):
        return self

    def __exit__(self, *exc_info):
        self.close()


class SerialTransport(Transport):
    """Wraps kf_debug.SerialLink -- the existing, working serial
    implementation -- behind the Transport interface, rather than
    reimplementing port-opening, write-framing, or error messages."""

    def __init__(self, port, baud=115200, verbose=False):
        self.port = port  # the resolved port string (auto-detection, if
        # any, already happened in build_transport() before this runs) --
        # kept so the UI can show *which* port it actually connected to,
        # not just the literal "auto" the user typed.
        self._link = kfd.SerialLink(port, baud=baud, verbose=verbose)

    def send_line(self, line):
        self._link.send(line)

    def readline(self):
        return self._link.readline()

    def close(self):
        self._link.close()


class SocketTransport(Transport):
    """Reserved for talking KFDBG to the desktop simulator over a local
    TCP socket, once the simulator speaks the protocol. Not implemented --
    `--target sim:<host>:<port>` raises a clear error until it lands.

    When it does: open the socket in __init__, and implement send_line()
    (write the line + b"\\n") and readline() (read up to the next b"\\n",
    returning b"" on a per-call timeout) exactly like SerialTransport
    does. Nothing above this class -- not the UI, not kfdbg_ping/shot/
    state/press below -- should need to change, because they only ever
    talk to the Transport interface, never to sockets or serial ports
    directly."""

    def __init__(self, host, port):
        raise kfd.KfDebugError(
            "the simulator doesn't speak KFDBG over a socket yet -- "
            "`--target sim:<host>:<port>` is reserved for that, once it "
            "does. Use `--target serial:<port>` (or serial:auto) for a "
            "real board, or `--demo` to try the panel with no device at "
            "all.")

    # These never run -- __init__ always raises first -- but Transport is
    # an abstract base class, so Python refuses to construct *any*
    # subclass missing an implementation of its abstract methods, even one
    # whose constructor never gets that far. Stubbed only to satisfy that.
    def send_line(self, line):
        raise NotImplementedError

    def readline(self):
        raise NotImplementedError


# --------------------------------------------------------------------------
# Protocol-level commands, built on a Transport. Thin on purpose: the real
# work -- framing, base64, CRC32 -- lives in kfd.read_frame(), reused
# as-is. This is the Transport-shaped equivalent of kf_debug.py's private
# `_expect()` helper, which is shaped around SerialLink (send + read_frame
# bundled together) rather than the send_line/readline split a socket
# transport will need.
# --------------------------------------------------------------------------

def expect_frame(transport, command, expected_type, timeout=kfd.DEFAULT_TIMEOUT):
    transport.send_line(command)
    frame_type, payload = kfd.read_frame(transport.readline, overall_timeout=timeout)
    if frame_type == "err":
        raise kfd.KfDebugError(
            f"device rejected `{command}`: {payload.decode('utf-8', 'replace')}")
    if frame_type != expected_type:
        raise kfd.ProtocolError(
            f"expected a `{expected_type}` reply to `{command}`, got `{frame_type}`")
    return payload


def kfdbg_ping(transport):
    payload = expect_frame(transport, "KFDBG PING", "pong")
    return payload.decode("utf-8", "replace")


def kfdbg_state(transport):
    payload = expect_frame(transport, "KFDBG STATE", "json")
    return json.loads(payload.decode("utf-8", "replace"))


def kfdbg_press(transport, mask, hold_ms=0):
    command = f"KFDBG BTNHOLD {mask} {hold_ms}" if hold_ms else f"KFDBG BTN {mask}"
    payload = expect_frame(transport, command, "ack")
    return payload.decode("utf-8", "replace")


def kfdbg_advance(transport, seconds):
    """Same `KFDBG ADVANCE <seconds>` command as kf_debug.py's cmd_advance
    -- ages the pet forward without waiting for it to happen in real time.
    Callers build `seconds` with kfd.parse_duration(), the same helper
    cmd_advance() uses, rather than a second copy of that parsing."""
    payload = expect_frame(transport, f"KFDBG ADVANCE {seconds}", "ack")
    return payload.decode("utf-8", "replace")


def kfdbg_reset(transport):
    """Same `KFDBG RESET` command as kf_debug.py's cmd_reset -- back to a
    fresh egg."""
    payload = expect_frame(transport, "KFDBG RESET", "ack")
    return payload.decode("utf-8", "replace")


def kfdbg_mult(transport, factor):
    """Same `KFDBG MULT <n>` command as kf_debug.py's cmd_mult, n in
    1..256. The UI only ever offers the fixed multiplier buttons below, so
    an out-of-range value here would be a bug in this file, not a user
    typo -- but the check is cheap and the resulting error is worth having
    either way."""
    if not 1 <= factor <= 256:
        raise kfd.KfDebugError(
            f"time multiplier must be between 1 and 256, got {factor}")
    payload = expect_frame(transport, f"KFDBG MULT {factor}", "ack")
    return payload.decode("utf-8", "replace")


def kfdbg_shot(transport):
    """Returns raw RGB565-LE pixel bytes, 240*320*2, row-major."""
    payload = expect_frame(transport, "KFDBG SHOT", "fb", timeout=kfd.SHOT_TIMEOUT)
    return kfd.decode_rle(payload, kfd.FB_PIXEL_BYTES)


# ==========================================================================
# The fake device: an in-process stand-in for real hardware, for --demo
# mode. It has no serial port at all -- FakeTransport below builds real
# KFDBG-BEGIN/END wire frames (base64, RLE, CRC32) from this device's
# state and feeds them through kfd.read_frame(), the exact same parser a
# real connection uses. That's deliberate: --demo mode proves the wire
# protocol code works, not just that the window draws, and it's what lets
# this whole file be verified with no board on the desk.
# ==========================================================================

class FakeDevice:
    """A pretend pet: enough state to make the readout and screenshot
    look alive, and to visibly react to button presses."""

    _STAGE_ORDER = ("hatchling", "juvenile", "adult")
    _STAGE_COLOR = {
        "hatchling": 0xFFE0,  # yellow, RGB565
        "juvenile": 0x07E0,   # green
        "adult": 0x001F,      # blue
    }

    def __init__(self):
        self.stage = "hatchling"
        self.trait = "curious"
        self.hunger = 72
        self.happiness = 55
        self.energy = 88
        self.stage_started = time.monotonic()
        self.boot_time = time.monotonic()
        self.frame_count = 0
        self.cursor_x = kfd.FB_WIDTH // 2
        self.cursor_y = kfd.FB_HEIGHT // 2
        self.last_button_note = "none"
        self.time_multiplier = 1
        self.advanced_seconds = 0  # accumulated via KFDBG ADVANCE, demo-only

    def handle_advance(self, seconds):
        """KFDBG ADVANCE for the fake device: just banks the seconds so
        state_dict()'s pet_age_s reflects it immediately. The real
        firmware actually re-runs decay math for the skipped time; the
        demo device only needs to prove the panel's buttons round-trip
        the command and the readout updates."""
        self.advanced_seconds += seconds
        return f"advanced {seconds}s (demo)"

    def handle_mult(self, factor):
        self.time_multiplier = factor
        return f"multiplier now {factor}x (demo)"

    def reset(self):
        """KFDBG RESET for the fake device: back to a fresh egg, same as
        __init__ builds."""
        self.__init__()
        return "reset to a fresh egg (demo)"

    def handle_button(self, mask, hold_ms):
        """Moves the on-screen blob and nudges the stats, so a screenshot
        taken before and after a press visibly differs -- the whole point
        of --demo mode being able to prove button presses do something."""
        step = 12 if not hold_ms else min(48, 12 + hold_ms // 40)
        if mask & kfd.BUTTON_BITS["UP"]:
            self.cursor_y -= step
        if mask & kfd.BUTTON_BITS["DOWN"]:
            self.cursor_y += step
        if mask & kfd.BUTTON_BITS["LEFT"]:
            self.cursor_x -= step
        if mask & kfd.BUTTON_BITS["RIGHT"]:
            self.cursor_x += step
        if mask & kfd.BUTTON_BITS["A"]:
            self.happiness = min(100, self.happiness + 5)
        if mask & kfd.BUTTON_BITS["B"]:
            self.hunger = max(0, self.hunger - 5)
        if mask & kfd.BUTTON_BITS["MENU"]:
            idx = self._STAGE_ORDER.index(self.stage)
            self.stage = self._STAGE_ORDER[(idx + 1) % len(self._STAGE_ORDER)]
            self.stage_started = time.monotonic()

        margin = 24
        self.cursor_x = max(margin, min(kfd.FB_WIDTH - margin, self.cursor_x))
        self.cursor_y = max(margin, min(kfd.FB_HEIGHT - margin, self.cursor_y))

        names = [name for name, bit in kfd.BUTTON_BITS.items() if mask & bit]
        self.last_button_note = "+".join(names) if names else "none"
        return self.last_button_note

    def state_dict(self):
        elapsed = time.monotonic() - self.stage_started
        pet_age_s = (time.monotonic() - self.boot_time) + self.advanced_seconds
        return {
            "stage": self.stage,
            "base_trait": self.trait,
            "hunger": self.hunger,
            "happiness": self.happiness,
            "energy": self.energy,
            "time_in_stage_s": round(elapsed, 1),
            "pet_age_s": round(pet_age_s, 1),
            "time_multiplier": self.time_multiplier,
            "free_heap_bytes": 181_000 - (self.frame_count * 37) % 4000,
            "free_psram_bytes": 7_800_000 - (self.frame_count * 191) % 20000,
            "fps": round(29.4 + 0.6 * ((self.frame_count % 10) / 10), 1),
            "frame_time_ms": round(1000.0 / 29.7, 2),
            "last_button": self.last_button_note,
        }

    def render_framebuffer(self):
        """A plausible 240x320 RGB565 framebuffer: a drifting gradient
        background, a border, and a filled circle standing in for the pet
        that moves with the D-pad and changes colour with stage."""
        w, h = kfd.FB_WIDTH, kfd.FB_HEIGHT
        px = [0] * (w * h)
        shift = (self.frame_count * 5) % h

        for y in range(h):
            g = ((y + shift) % h) * 63 // h
            bg = (4 << 11) | (g << 5) | 12
            row = y * w
            for x in range(w):
                px[row + x] = bg

        for x in range(w):
            px[x] = 0xFFFF
            px[(h - 1) * w + x] = 0xFFFF
        for y in range(h):
            px[y * w] = 0xFFFF
            px[y * w + w - 1] = 0xFFFF

        color = self._STAGE_COLOR.get(self.stage, 0xF800)
        radius = 22
        cx, cy = self.cursor_x, self.cursor_y
        for dy in range(-radius, radius + 1):
            yy = cy + dy
            if yy < 1 or yy >= h - 1:
                continue
            span = int((radius * radius - dy * dy) ** 0.5)
            x0, x1 = max(1, cx - span), min(w - 2, cx + span)
            row = yy * w
            for xx in range(x0, x1 + 1):
                px[row + xx] = color

        self.frame_count += 1
        return b"".join(struct.pack("<H", p) for p in px)


def _encode_rle_for_demo(pixel_bytes):
    """Reference RLE encoder for --demo mode only. The real device does
    this encoding; kf_debug.py (and this file, for real connections) only
    ever needs to decode it. Mirrors kf_debug_selftest.py's encoder --
    kept separate rather than imported from a test file."""
    pixels = [p for (p,) in struct.iter_unpack("<H", pixel_bytes)]
    out = bytearray()
    i, n = 0, len(pixels)
    while i < n:
        run = 1
        while i + run < n and pixels[i + run] == pixels[i] and run < 0xFFFF:
            run += 1
        out += struct.pack("<HH", run, pixels[i])
        i += run
    return bytes(out)


class FakeTransport(Transport):
    """Transport backed by FakeDevice instead of a real port. Builds
    genuine KFDBG-BEGIN/END wire text (base64 + CRC32, with a stray log
    line thrown in) and hands it to kfd.read_frame() one line at a time --
    the same parser SerialTransport's readline() ultimately feeds. This is
    what lets --demo mode round-trip through the real protocol code."""

    def __init__(self):
        self.device = FakeDevice()
        self._pending_lines = []

    def send_line(self, line):
        frame_type, payload = self._build_reply(line.strip())
        b64 = base64.b64encode(payload).decode("ascii")
        wrapped = [b64[i:i + 88] for i in range(0, len(b64), 88)] or [""]
        crc = zlib.crc32(payload) & 0xFFFFFFFF
        lines = ["I (demo) kf_dbg: fake device reply"]
        lines.append(f"KFDBG-BEGIN {frame_type} {len(b64)}")
        lines.extend(wrapped)
        lines.append(f"KFDBG-END {crc:08x}")
        self._pending_lines = lines

    def _build_reply(self, command):
        parts = command.split()
        head = parts[:2]
        if head == ["KFDBG", "PING"]:
            return "pong", b"kamiframe demo-fw (fake device, no board attached)"
        if head == ["KFDBG", "SHOT"]:
            fb = self.device.render_framebuffer()
            return "fb", _encode_rle_for_demo(fb)
        if head == ["KFDBG", "STATE"]:
            return "json", json.dumps(self.device.state_dict()).encode("utf-8")
        if head == ["KFDBG", "BTN"] and len(parts) >= 3:
            note = self.device.handle_button(int(parts[2]), 0)
            return "ack", note.encode("utf-8")
        if head == ["KFDBG", "BTNHOLD"] and len(parts) >= 4:
            note = self.device.handle_button(int(parts[2]), int(parts[3]))
            return "ack", note.encode("utf-8")
        if head == ["KFDBG", "ADVANCE"] and len(parts) >= 3:
            note = self.device.handle_advance(int(parts[2]))
            return "ack", note.encode("utf-8")
        if head == ["KFDBG", "RESET"]:
            note = self.device.reset()
            return "ack", note.encode("utf-8")
        if head == ["KFDBG", "MULT"] and len(parts) >= 3:
            factor = int(parts[2])
            if not 1 <= factor <= 256:
                return "err", (f"time multiplier must be between 1 and 256, "
                                f"got {factor}").encode("utf-8")
            note = self.device.handle_mult(factor)
            return "ack", note.encode("utf-8")
        return "err", f"fake device doesn't understand: {command}".encode("utf-8")

    def readline(self):
        if not self._pending_lines:
            return b""
        return (self._pending_lines.pop(0) + "\n").encode("utf-8")


# --------------------------------------------------------------------------
# --target parsing
# --------------------------------------------------------------------------

def parse_target(spec):
    """`serial:<port>`, `serial:auto` (default), `sim:<host>:<port>`
    (reserved, see SocketTransport), or `demo`/`fake`. Returns (kind, arg).
    Only validates shape here -- e.g. `sim:` parses fine and fails later,
    at connect time, with a friendly message, so a mistyped --target
    doesn't crash the window before it opens."""
    spec = (spec or "").strip()
    if spec.lower() in ("demo", "fake"):
        return ("demo", None)
    if ":" not in spec:
        raise kfd.KfDebugError(
            f"--target must look like serial:<port>, serial:auto, "
            f"sim:<host>:<port>, or demo -- got {spec!r}")
    kind, _, rest = spec.partition(":")
    kind = kind.lower()
    if kind == "serial":
        return ("serial", rest or "auto")
    if kind == "sim":
        return ("sim", rest)
    raise kfd.KfDebugError(
        f"unknown --target kind '{kind}' -- expected serial, sim, or demo")


def build_transport(kind, arg, baud, verbose):
    if kind == "demo":
        return FakeTransport()
    if kind == "serial":
        port = arg
        if not port or port == "auto":
            port = kfd.find_port(verbose=verbose)
        return SerialTransport(port, baud=baud, verbose=verbose)
    if kind == "sim":
        host, _, port_s = arg.partition(":")
        return SocketTransport(host, port_s)
    raise kfd.KfDebugError(f"unknown target kind '{kind}'")


# --------------------------------------------------------------------------
# Screenshot saving -- timestamped filenames in ~/Downloads by default, so
# repeated clicks produce a stack of files rather than overwriting the
# last one. Reuses kfd.resolve_shot_path() for the actual path
# normalisation (expanduser + abspath); it only knows how to build a fixed
# `kf_shot.png` name with no argument, so the timestamped name is built
# here and handed to it as an explicit path.
# --------------------------------------------------------------------------

def _shot_directory():
    """Same fallback kfd.resolve_shot_path() uses when given no name:
    ~/Downloads if it exists, else the current directory. Duplicated as a
    single `if` rather than imported, because resolve_shot_path() only
    exposes that choice bundled with the fixed 'kf_shot.png' filename."""
    downloads = os.path.expanduser("~/Downloads")
    return downloads if os.path.isdir(downloads) else os.getcwd()


def timestamped_shot_path():
    base_dir = _shot_directory()
    stamp = time.strftime("%Y%m%d_%H%M%S")
    path = kfd.resolve_shot_path(os.path.join(base_dir, f"kf_shot_{stamp}.png"))
    if not os.path.exists(path):
        return path
    # Two saves inside the same second (a fast double-click) would
    # otherwise collide. Keep the same timestamp shape, disambiguate.
    n = 2
    while True:
        candidate = kfd.resolve_shot_path(
            os.path.join(base_dir, f"kf_shot_{stamp}_{n}.png"))
        if not os.path.exists(candidate):
            return candidate
        n += 1


def rgb565_bytes_to_rgb888(raw):
    """RGB565-LE pixel bytes -> flat RGB888 bytes, same conversion
    kf_debug.py's cmd_shot() does."""
    rgb = bytearray(kfd.FB_WIDTH * kfd.FB_HEIGHT * 3)
    idx = 0
    for (px,) in struct.iter_unpack("<H", raw):
        r, g, b = kfd.rgb565_to_rgb888(px)
        rgb[idx], rgb[idx + 1], rgb[idx + 2] = r, g, b
        idx += 3
    return bytes(rgb)


def save_screenshot(transport):
    """Fetches one fresh frame and writes it to a timestamped PNG. Returns
    (path, rgb888_bytes) so the caller can also update a preview."""
    raw = kfdbg_shot(transport)
    rgb = rgb565_bytes_to_rgb888(raw)
    png_bytes = kfd.png_encode(kfd.FB_WIDTH, kfd.FB_HEIGHT, rgb)
    path = timestamped_shot_path()
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "wb") as f:
        f.write(png_bytes)
    return path, rgb


def rgb888_to_ppm(rgb_bytes, width, height):
    header = f"P6 {width} {height} 255\n".encode("ascii")
    return header + rgb_bytes


# ==========================================================================
# Tk UI
# ==========================================================================

if tk is not None:

    # ==========================================================================
    # Fixed colour palette. Applied explicitly to every widget below, rather
    # than left to inherit from the OS ttk theme (macOS's "aqua" theme mostly
    # ignores configured foreground/background on ttk widgets and substitutes
    # system colours instead -- which is exactly why the panel had invisible
    # widgets under dark mode: black-on-nothing text landed on a dark system
    # background). These colours are fixed regardless of whether the OS is in
    # light or dark mode, so the panel looks the same, and stays legible,
    # either way. See _setup_style() below for where they're wired in.
    # ==========================================================================
    COLOR_BG = "#f2f2f3"
    COLOR_FG = "#151515"
    COLOR_MUTED_FG = "#4a4a4a"
    COLOR_BORDER = "#c7c7c7"
    COLOR_BUTTON_BG = "#e3e3e3"
    COLOR_BUTTON_ACTIVE_BG = "#d2d2d2"
    COLOR_ENTRY_BG = "#ffffff"
    COLOR_WARN_FG = "#8a4b00"
    COLOR_CONNECTED_FG = "#1a7f37"
    COLOR_FAILED_FG = "#b42318"
    COLOR_DISABLED_FG = "#8a8a8a"

    class Tooltip:
        """A small delayed label that follows the mouse over one widget.
        Used on the seek-timeline control to explain *why* it's disabled
        without needing a status bar essay for it."""

        def __init__(self, widget, text):
            self.widget = widget
            self.text = text
            self._tip = None
            widget.bind("<Enter>", self._show)
            widget.bind("<Leave>", self._hide)

        def _show(self, _event=None):
            if self._tip is not None:
                return
            x = self.widget.winfo_rootx() + 12
            y = self.widget.winfo_rooty() + self.widget.winfo_height() + 6
            self._tip = tk.Toplevel(self.widget)
            self._tip.wm_overrideredirect(True)
            self._tip.wm_geometry(f"+{x}+{y}")
            label = tk.Label(self._tip, text=self.text, justify="left",
                              background="#ffffe0", foreground="#111111",
                              relief="solid", borderwidth=1,
                              font=("TkDefaultFont", 9),
                              wraplength=260, padx=6, pady=3)
            label.pack()

        def _hide(self, _event=None):
            if self._tip is not None:
                self._tip.destroy()
                self._tip = None


    # Preferred order for the state readout; anything the device sends
    # that isn't in this list still shows up, appended below, so an
    # unrecognised field is visible rather than silently dropped (the
    # protocol doesn't pin field names down -- see kf_debug.py).
    # time_multiplier and pet_age_s are the two fields KFDBG STATE grew for
    # the time controls below -- placed right after the stats a reader
    # already cares about, not buried at the end.
    STATE_FIELD_ORDER = [
        "stage", "base_trait", "trait", "hunger", "happiness", "energy",
        "time_in_stage_s", "pet_age_s", "time_multiplier",
        "free_heap_bytes", "free_psram_bytes", "fps", "frame_time_ms",
    ]

    BUTTON_LABELS = ["UP", "DOWN", "LEFT", "RIGHT", "A", "B", "MENU"]

    # Keyboard bindings -> button name. Arrow keys for the D-pad, Z/X for
    # A/B (a common emulator-style layout for a left-hand + right-hand
    # split), Return and Escape both map to MENU since it's the one button
    # people reach for on either a "confirm" or a "back out" instinct.
    KEY_TO_BUTTON = {
        "Up": "UP", "Down": "DOWN", "Left": "LEFT", "Right": "RIGHT",
        "z": "A", "Z": "A", "x": "B", "X": "B",
        "Return": "MENU", "Escape": "MENU",
    }

    TAP_VS_HOLD_MS = 150  # below this, a click/keypress is a tap (BTN);
                           # at or above it, it's a hold (BTNHOLD elapsed).

    # Skip-forward presets for the time controls. Seconds are computed once,
    # at import time, with kfd.parse_duration() -- the exact same duration
    # parser kf_debug.py's `advance` subcommand uses -- rather than a second
    # copy of "1h means 3600" living in this file.
    ADVANCE_PRESETS = [
        ("Skip 1 Hour", kfd.parse_duration("1h")),
        ("Skip 1 Day", kfd.parse_duration("1d")),
        ("Skip 1 Week", kfd.parse_duration("1w")),
    ]

    MULT_PRESETS = (1, 2, 4, 8, 16, 32, 64, 128, 256)


    class PanelApp:
        def __init__(self, root, target, baud, state_interval, verbose):
            self.root = root
            self.target_kind, self.target_arg = target
            self.baud = baud
            self.verbose = verbose
            self.state_interval = tk.DoubleVar(value=state_interval)

            self.transport = None
            self.connected = False
            self._state_pending = False
            self._shot_pending = False
            self._last_thumb_photo = None  # keep a reference or Tk drops it

            self._key_start = {}
            self._key_pending_release = {}

            self.cmd_queue = queue.Queue()
            self.result_queue = queue.Queue()
            self._worker = threading.Thread(target=self._worker_loop, daemon=True)
            self._worker.start()

            # The build marker in the title is not decoration. Three separate
            # rounds of "it still looks the same" turned out to be a stale
            # window or an older file being run, and there was no way to tell
            # by looking. Now there is: if the title does not say the build
            # you expect, you are not running the code you think you are.
            root.title(f"Kamiframe Hardware Panel [{PANEL_BUILD}]")
            root.protocol("WM_DELETE_WINDOW", self.on_close)

            self._build_ui()
            self._bind_keys()

            # Auto-connect on launch, for every target kind, not just demo --
            # the whole point is that Connect shouldn't be a control the user
            # has to go find first. If nothing is plugged in, this fails
            # quickly and says so plainly in the connection-state banner
            # rather than leaving the panel simply looking unconnected.
            if self.target_kind == "demo":
                self.status_var.set("Demo mode -- connecting to the fake device...")
            else:
                self.status_var.set("Looking for a device to connect to...")
            self._connect()

            self.root.after(50, self._drain_results)
            self.root.after(int(self.state_interval.get() * 1000), self._poll_state_tick)

        # ------------------------------------------------------------
        # UI construction
        # ------------------------------------------------------------

        def _setup_style(self):
            """Fixed, explicit colours for every ttk widget class used
            below, on the 'clam' theme rather than the platform-native one.
            'clam' actually honours style.configure() colours; macOS's
            'aqua' theme substitutes system control colours for most of
            them regardless of what's configured, which is what made
            widgets vanish under dark mode in the first place -- switching
            themes fixes that at the root instead of patching around it
            per-widget."""
            self.root.configure(background=COLOR_BG)
            style = ttk.Style(self.root)
            try:
                style.theme_use("clam")
            except tk.TclError:
                pass  # 'clam' ships with every standard Tk build; if some
                      # install is missing it, fall back to whatever theme
                      # is active rather than crashing -- every colour
                      # below is still set explicitly, just possibly with
                      # less effect on that theme.

            style.configure(".", background=COLOR_BG, foreground=COLOR_FG,
                             fieldbackground=COLOR_ENTRY_BG)
            style.configure("TFrame", background=COLOR_BG)
            style.configure("TLabel", background=COLOR_BG, foreground=COLOR_FG)
            style.configure("Muted.TLabel", background=COLOR_BG,
                             foreground=COLOR_MUTED_FG)
            style.configure("Warn.TLabel", background=COLOR_BG,
                             foreground=COLOR_WARN_FG)
            style.configure("ConnGood.TLabel", background=COLOR_BG,
                             foreground=COLOR_CONNECTED_FG,
                             font=("TkDefaultFont", 12, "bold"))
            style.configure("ConnBad.TLabel", background=COLOR_BG,
                             foreground=COLOR_FAILED_FG,
                             font=("TkDefaultFont", 12, "bold"))
            style.configure("ConnNeutral.TLabel", background=COLOR_BG,
                             foreground=COLOR_FG,
                             font=("TkDefaultFont", 12, "bold"))
            style.configure("TLabelframe", background=COLOR_BG,
                             foreground=COLOR_FG, bordercolor=COLOR_BORDER)
            style.configure("TLabelframe.Label", background=COLOR_BG,
                             foreground=COLOR_FG,
                             font=("TkDefaultFont", 10, "bold"))
            style.configure("TButton", background=COLOR_BUTTON_BG,
                             foreground=COLOR_FG, bordercolor=COLOR_BORDER)
            style.map("TButton",
                      background=[("active", COLOR_BUTTON_ACTIVE_BG),
                                  ("disabled", COLOR_BG)],
                      foreground=[("disabled", COLOR_DISABLED_FG)])
            style.configure("TEntry", fieldbackground=COLOR_ENTRY_BG,
                             foreground=COLOR_FG)
            style.map("TEntry",
                      fieldbackground=[("readonly", COLOR_ENTRY_BG)],
                      foreground=[("readonly", COLOR_FG)])
            style.configure("TCombobox", fieldbackground=COLOR_ENTRY_BG,
                             foreground=COLOR_FG, background=COLOR_BUTTON_BG,
                             arrowcolor=COLOR_FG)
            style.map("TCombobox",
                      fieldbackground=[("readonly", COLOR_ENTRY_BG)],
                      foreground=[("readonly", COLOR_FG)])
            style.configure("TSpinbox", fieldbackground=COLOR_ENTRY_BG,
                             foreground=COLOR_FG, background=COLOR_BUTTON_BG,
                             arrowcolor=COLOR_FG)
            style.configure("Horizontal.TScale", background=COLOR_BG)
            style.configure("TScrollbar", background=COLOR_BUTTON_BG,
                             troughcolor=COLOR_BG, bordercolor=COLOR_BORDER,
                             arrowcolor=COLOR_FG)

            # The combobox's popdown list is a plain Tk Listbox underneath,
            # styled through the option database rather than ttk.Style.
            self.root.option_add("*TCombobox*Listbox.background", COLOR_ENTRY_BG)
            self.root.option_add("*TCombobox*Listbox.foreground", COLOR_FG)
            self.root.option_add("*TCombobox*Listbox.selectBackground",
                                  COLOR_BUTTON_ACTIVE_BG)
            self.root.option_add("*TCombobox*Listbox.selectForeground", COLOR_FG)

        def _build_ui(self):
            self._setup_style()
            root = self.root

            # Explicit size, always -- never auto-sized to content. Content
            # that needs more room scrolls (see the Canvas+Scrollbar setup
            # below); the window itself does not grow to chase it.
            root.geometry("560x900")
            root.minsize(480, 640)

            # Status line: packed at the very bottom of the *root* window,
            # before anything else claims space, so it's reserved and
            # always visible -- outside the scrollable area on purpose,
            # per the "pin the status line" requirement.
            status = ttk.Frame(root)
            status.pack(side="bottom", fill="x")
            self.status_var = tk.StringVar(value="Not connected.")
            self.status_label = ttk.Label(status, textvariable=self.status_var,
                                           style="Muted.TLabel", wraplength=540,
                                           justify="left")
            self.status_label.pack(side="left", padx=8, pady=6, fill="x")
            ttk.Separator(root, orient="horizontal").pack(side="bottom", fill="x")

            # Everything else: one vertical column (Connection, Buttons,
            # Screenshot, State, Time controls) inside a scrollable canvas,
            # so nothing can ever end up unreachable regardless of window
            # size or how much the state readout grows.
            outer = ttk.Frame(root)
            outer.pack(side="top", fill="both", expand=True)

            self.canvas = tk.Canvas(outer, background=COLOR_BG,
                                     highlightthickness=0, borderwidth=0)
            scrollbar = ttk.Scrollbar(outer, orient="vertical",
                                       command=self.canvas.yview)
            self.canvas.configure(yscrollcommand=scrollbar.set)
            self.canvas.pack(side="left", fill="both", expand=True)
            scrollbar.pack(side="right", fill="y")

            self.scroll_frame = ttk.Frame(self.canvas)
            self._scroll_window = self.canvas.create_window(
                (0, 0), window=self.scroll_frame, anchor="nw")

            def _on_frame_configure(_event=None):
                self.canvas.configure(scrollregion=self.canvas.bbox("all"))

            def _on_canvas_configure(event):
                # Keep the inner column exactly as wide as the visible
                # canvas, so content wraps rather than ever needing a
                # horizontal scrollbar -- only vertical scrolling is
                # offered.
                self.canvas.itemconfigure(self._scroll_window, width=event.width)

            self.scroll_frame.bind("<Configure>", _on_frame_configure)
            self.canvas.bind("<Configure>", _on_canvas_configure)

            def _on_mousewheel(event):
                if sys.platform == "darwin":
                    self.canvas.yview_scroll(int(-1 * event.delta), "units")
                else:
                    self.canvas.yview_scroll(int(-1 * (event.delta / 120)), "units")

            self.canvas.bind_all("<MouseWheel>", _on_mousewheel)
            self.canvas.bind_all("<Button-4>",
                                  lambda e: self.canvas.yview_scroll(-3, "units"))
            self.canvas.bind_all("<Button-5>",
                                  lambda e: self.canvas.yview_scroll(3, "units"))

            col = self.scroll_frame
            self._build_connection_controls(col)
            self._build_button_pad(col)
            self._build_screenshot_controls(col)
            self._build_state_readout(col)
            self._build_sim_controls(col)
            ttk.Frame(col, height=8).pack(fill="x")  # bottom breathing room

        def _build_connection_controls(self, parent):
            frame = ttk.LabelFrame(parent, text="Connection")
            frame.pack(fill="x", padx=8, pady=(8, 10))

            # The prominent, plain-English connection banner -- always the
            # first thing in the window, always in words, not just a
            # button's enabled/disabled state.
            self.conn_state_var = tk.StringVar(value="Not connected.")
            self.conn_state_label = ttk.Label(
                frame, textvariable=self.conn_state_var,
                style="ConnNeutral.TLabel", wraplength=500, justify="left")
            self.conn_state_label.pack(fill="x", padx=8, pady=(8, 4), anchor="w")

            if self.target_kind == "demo":
                ttk.Label(frame, text="Demo mode -- driving an in-process "
                                       "fake device, no hardware needed.",
                          style="Warn.TLabel", wraplength=500,
                          justify="left").pack(fill="x", padx=8, pady=(0, 8),
                                                anchor="w")
                return

            row = ttk.Frame(frame)
            row.pack(fill="x", padx=8, pady=(0, 6))
            ttk.Label(row, text="Port:").pack(side="left", padx=(0, 4))
            self.port_var = tk.StringVar(
                value=self.target_arg if self.target_arg not in (None, "auto")
                else "auto")
            self.port_combo = ttk.Combobox(row, textvariable=self.port_var, width=22)
            self.port_combo.pack(side="left", padx=(0, 4))
            self._refresh_ports()

            ttk.Button(row, text="Rescan", command=self._refresh_ports).pack(
                side="left", padx=(0, 8))

            self.connect_btn = ttk.Button(row, text="Connect",
                                           command=self._on_connect_click)
            self.connect_btn.pack(side="left")

            row2 = ttk.Frame(frame)
            row2.pack(fill="x", padx=8, pady=(0, 8))
            ttk.Label(row2, text="State poll (s):").pack(side="left", padx=(0, 4))
            ttk.Spinbox(row2, from_=0.2, to=10.0, increment=0.1, width=5,
                        textvariable=self.state_interval).pack(side="left")

        def _refresh_ports(self):
            try:
                from serial.tools import list_ports
                ports = [p.device for p in list_ports.comports()]
            except ImportError:
                ports = []
                self.port_combo.configure(
                    values=["(install pyserial to list ports: pip install pyserial)"])
                return
            self.port_combo.configure(values=["auto"] + ports)
            if not self.port_var.get():
                self.port_var.set("auto")

        def _build_button_pad(self, parent):
            frame = ttk.LabelFrame(parent, text="Buttons "
                                    "(click, click-and-hold, or use the keyboard)")
            frame.pack(fill="x", padx=8, pady=(0, 10))

            grid = ttk.Frame(frame)
            grid.pack(padx=10, pady=10)

            def make(name, r, c, cspan=1):
                b = tk.Button(grid, text=name, width=6, height=2,
                              background=COLOR_BUTTON_BG, foreground=COLOR_FG,
                              activebackground=COLOR_BUTTON_ACTIVE_BG,
                              activeforeground=COLOR_FG,
                              highlightbackground=COLOR_BG,
                              highlightthickness=1)
                b.grid(row=r, column=c, columnspan=cspan, padx=3, pady=3)
                b.bind("<ButtonPress-1>", lambda e, n=name: self._pad_down(n))
                b.bind("<ButtonRelease-1>", lambda e, n=name: self._pad_up(n))
                return b

            self.pad_buttons = {}
            self.pad_buttons["UP"] = make("UP", 0, 1)
            self.pad_buttons["LEFT"] = make("LEFT", 1, 0)
            self.pad_buttons["RIGHT"] = make("RIGHT", 1, 2)
            self.pad_buttons["DOWN"] = make("DOWN", 2, 1)
            self.pad_buttons["A"] = make("A", 1, 4)
            self.pad_buttons["B"] = make("B", 2, 4)
            self.pad_buttons["MENU"] = make("MENU", 3, 0, cspan=3)

            ttk.Label(frame, text="Keyboard: arrows = D-pad, Z/X = A/B, "
                                   "Enter/Esc = MENU. Hold a bit under "
                                   f"{TAP_VS_HOLD_MS}ms and it's a tap; "
                                   "longer sends a timed hold.",
                      style="Muted.TLabel", wraplength=500,
                      justify="left").pack(padx=10, pady=(0, 10), anchor="w")

        def _build_screenshot_controls(self, parent):
            frame = ttk.LabelFrame(parent, text="Screenshot")
            frame.pack(fill="x", padx=8, pady=(0, 10))

            self.shot_btn = ttk.Button(frame, text="Save Screenshot (Cmd/Ctrl+S)",
                                        command=self._on_save_screenshot)
            self.shot_btn.pack(fill="x", padx=8, pady=(8, 4))

            ttk.Label(frame, text="Fetches a fresh frame from the device -- "
                                   "takes a second or two, that's expected, "
                                   "not a hang.", style="Muted.TLabel",
                      wraplength=500, justify="left").pack(padx=8, anchor="w")

            self.shot_path_var = tk.StringVar(value="")
            self.shot_path_entry = ttk.Entry(frame, textvariable=self.shot_path_var,
                                              state="readonly", width=32)
            self.shot_path_entry.pack(fill="x", padx=8, pady=(4, 8))

            self.thumb_label = tk.Label(frame, text="No screenshot yet",
                                         width=30, height=16, relief="sunken",
                                         background="#222222", foreground="#aaaaaa")
            self.thumb_label.pack(padx=8, pady=(0, 8))

        def _build_state_readout(self, parent):
            frame = ttk.LabelFrame(parent, text="Pet state (live)")
            frame.pack(fill="x", padx=8, pady=(0, 10))
            self.state_grid = ttk.Frame(frame)
            self.state_grid.pack(fill="x", padx=8, pady=8)
            self._state_row_widgets = {}  # field name -> (key label, value label)
            self._last_ping_var = tk.StringVar(value="")
            ttk.Label(frame, textvariable=self._last_ping_var,
                      style="Muted.TLabel").pack(anchor="w", padx=8, pady=(0, 8))

        def _build_sim_controls(self, parent):
            # ------------------------------------------------------------
            # Time controls: KFDBG ADVANCE / RESET / MULT. These work
            # against real hardware now, not just the simulator -- only
            # the seekable pet-age timeline at the bottom stays disabled,
            # because it needs the simulator's 2048-entry debug snapshot
            # ring (200KB+), which is compiled out of the ESP32 build
            # (KF_PET_SESSION_ENABLE_DEBUG_TOOLS=0) since the device has
            # 512KB of RAM total and the framebuffer alone is 150KB of it.
            # It is labelled simulator-only below rather than left looking
            # like a bug.
            # ------------------------------------------------------------
            frame = ttk.LabelFrame(parent, text="Time controls")
            frame.pack(fill="x", padx=8, pady=(0, 10))

            ttk.Label(frame, text="Skip the pet forward, or change how fast "
                                   "time passes for it. These work on real "
                                   "hardware, not only the simulator.",
                      style="Muted.TLabel", wraplength=500,
                      justify="left").pack(fill="x", padx=8, pady=(8, 6),
                                            anchor="w")

            row1 = ttk.Frame(frame)
            row1.pack(fill="x", padx=8, pady=(0, 6))
            self.time_control_buttons = []
            for label, seconds in ADVANCE_PRESETS:
                btn = ttk.Button(
                    row1, text=label,
                    command=lambda l=label, s=seconds: self._on_advance_click(l, s))
                btn.pack(side="left", padx=(0, 6))
                self.time_control_buttons.append(btn)
            reset_btn = ttk.Button(row1, text="Reset Egg",
                                    command=self._on_reset_click)
            reset_btn.pack(side="left")
            self.time_control_buttons.append(reset_btn)

            # The label gets its own line, and the nine multiplier buttons
            # split across two rows rather than one -- nine buttons plus a
            # label on one row doesn't fit at the window's minimum width
            # (this was caught by tools/kf_panel_layout_check.py: the last
            # button came back unmapped, the exact "control silently
            # disappears" failure mode this whole rewrite exists to rule
            # out).
            mult_frame = ttk.Frame(frame)
            mult_frame.pack(fill="x", padx=8, pady=(0, 8))
            ttk.Label(mult_frame, text="Time multiplier:").pack(anchor="w")
            half = (len(MULT_PRESETS) + 1) // 2
            for row_values in (MULT_PRESETS[:half], MULT_PRESETS[half:]):
                mult_row = ttk.Frame(mult_frame)
                mult_row.pack(fill="x", pady=(2, 0))
                for mult in row_values:
                    btn = ttk.Button(mult_row, text=f"{mult}x", width=4,
                                      command=lambda m=mult: self._on_mult_click(m))
                    btn.pack(side="left", padx=2, pady=2)
                    self.time_control_buttons.append(btn)

            timeline_frame = ttk.Frame(frame)
            timeline_frame.pack(fill="x", padx=8, pady=(2, 8))
            ttk.Label(timeline_frame,
                      text="Pet-age timeline (seek) -- simulator-only, not "
                           "available on this connection:",
                      style="Muted.TLabel", wraplength=500,
                      justify="left").pack(anchor="w")
            scale = ttk.Scale(timeline_frame, from_=0, to=100, orient="horizontal")
            scale.state(["disabled"])
            scale.pack(fill="x", pady=2)
            Tooltip(scale, "This needs the simulator's debug snapshot ring, "
                            "which is compiled out of the ESP32 build to "
                            "save RAM -- see the comment above "
                            "_build_sim_controls() in kf_panel.py. It will "
                            "only ever work against the desktop simulator, "
                            "once that speaks KFDBG, never a real board. "
                            "Reserved space, not a bug.")
            ttk.Label(timeline_frame, text="egg -> baby -> child -> teen -> adult",
                      style="Muted.TLabel", font=("TkDefaultFont", 8)).pack(anchor="w")

        # ------------------------------------------------------------
        # Keyboard + mouse input, with tap-vs-hold detection
        # ------------------------------------------------------------

        def _bind_keys(self):
            for keysym in KEY_TO_BUTTON:
                self.root.bind(f"<KeyPress-{keysym}>",
                                lambda e, k=keysym: self._key_down(KEY_TO_BUTTON[k]))
                self.root.bind(f"<KeyRelease-{keysym}>",
                                lambda e, k=keysym: self._key_up(KEY_TO_BUTTON[k]))
            self.root.bind_all("<Control-s>", lambda e: self._on_save_screenshot())
            self.root.bind_all("<Command-s>", lambda e: self._on_save_screenshot())

        def _focus_is_text_entry(self):
            """True while the port field (or any other text box) has
            focus. The button keybindings are on the root window, which
            sits in every child widget's bindtag chain -- so without this
            check, pressing Left/Right to move the cursor while typing a
            port path would *also* send LEFT/RIGHT button presses to the
            device."""
            widget = self.root.focus_get()
            return isinstance(widget, (tk.Entry, tk.Spinbox, ttk.Entry,
                                        ttk.Combobox, ttk.Spinbox))

        def _key_down(self, name):
            if self._focus_is_text_entry():
                return
            # Autorepeat sends KeyRelease immediately followed by another
            # KeyPress for a held key. If a release is still pending
            # (scheduled but not yet run) when a new press for the same
            # key arrives, it's autorepeat, not a real release -- cancel
            # the pending release and treat the key as still held.
            if name in self._key_pending_release:
                self.root.after_cancel(self._key_pending_release.pop(name))
                return
            if name in self._key_start:
                return
            self._key_start[name] = time.monotonic()

        def _key_up(self, name):
            if name not in self._key_start:
                return

            def finish():
                self._key_pending_release.pop(name, None)
                start = self._key_start.pop(name, None)
                if start is None:
                    return
                elapsed_ms = int((time.monotonic() - start) * 1000)
                self._send_button(name, elapsed_ms)

            self._key_pending_release[name] = self.root.after(1, finish)

        def _pad_down(self, name):
            self._key_start[f"pad:{name}"] = time.monotonic()

        def _pad_up(self, name):
            start = self._key_start.pop(f"pad:{name}", None)
            if start is None:
                return
            elapsed_ms = int((time.monotonic() - start) * 1000)
            self._send_button(name, elapsed_ms)

        def _send_button(self, name, elapsed_ms):
            if not self.connected:
                self.status_var.set("Not connected -- press ignored.")
                return
            hold_ms = elapsed_ms if elapsed_ms >= TAP_VS_HOLD_MS else 0
            mask = kfd.BUTTON_BITS[name]
            self.cmd_queue.put({"type": "press", "mask": mask, "hold_ms": hold_ms,
                                 "label": name})

        # ------------------------------------------------------------
        # Connection lifecycle
        # ------------------------------------------------------------

        def _on_connect_click(self):
            if self.connected:
                self.cmd_queue.put({"type": "disconnect"})
                self.connect_btn.configure(state="disabled")
                self.conn_state_var.set("Disconnecting...")
                self.conn_state_label.configure(style="ConnNeutral.TLabel")
                self.status_var.set("Disconnecting...")
                return
            self.target_arg = self.port_var.get() or "auto"
            self._connect()

        def _connect(self):
            if self.target_kind != "demo" and hasattr(self, "connect_btn"):
                self.connect_btn.configure(state="disabled")
            self.conn_state_var.set("Connecting...")
            self.conn_state_label.configure(style="ConnNeutral.TLabel")
            self.status_var.set("Connecting...")
            self.cmd_queue.put({"type": "connect"})

        def on_close(self):
            self.cmd_queue.put({"type": "shutdown"})
            self.root.destroy()

        # ------------------------------------------------------------
        # Time controls -- KFDBG ADVANCE / RESET / MULT. Same
        # not-connected-ignored-with-a-status-message pattern as the
        # button pad and screenshot button above, for consistency.
        # ------------------------------------------------------------

        def _on_advance_click(self, label, seconds):
            if not self.connected:
                self.status_var.set("Not connected -- command ignored.")
                return
            self.cmd_queue.put({"type": "advance", "seconds": seconds, "label": label})

        def _on_reset_click(self):
            if not self.connected:
                self.status_var.set("Not connected -- command ignored.")
                return
            self.cmd_queue.put({"type": "reset"})

        def _on_mult_click(self, factor):
            if not self.connected:
                self.status_var.set("Not connected -- command ignored.")
                return
            self.cmd_queue.put({"type": "mult", "factor": factor})

        def _request_state_refresh(self):
            """Ask for one state poll right away rather than waiting for
            the next timer tick -- used after a time control fires, so the
            readout reflects the new age/multiplier immediately instead of
            up to `state_interval` seconds later."""
            if self.connected and not self._state_pending:
                self._state_pending = True
                self.cmd_queue.put({"type": "state"})

        # ------------------------------------------------------------
        # Worker thread -- ALL serial/socket/fake-device I/O happens here.
        # Tk callbacks only ever enqueue work and read results off
        # result_queue via root.after(); nothing above this method touches
        # a Transport directly, on purpose (see the module docstring on
        # threading in the task this file was built against).
        # ------------------------------------------------------------

        def _describe_transport(self, transport):
            """A short, plain-English name for what a Transport is
            actually connected to, for the connection banner. Runs on the
            worker thread (no Tk calls here), same as everything else in
            _worker_loop."""
            if self.target_kind == "demo":
                return "the demo device (no hardware)"
            if isinstance(transport, SerialTransport):
                return transport.port
            if isinstance(transport, SocketTransport):
                return f"the simulator at {self.target_arg}"
            return "the device"

        def _worker_loop(self):
            while True:
                item = self.cmd_queue.get()
                kind = item["type"]
                if kind == "shutdown":
                    if self.transport is not None:
                        try:
                            self.transport.close()
                        except Exception:
                            pass
                    return
                try:
                    if kind == "connect":
                        transport = build_transport(
                            self.target_kind, self.target_arg, self.baud, self.verbose)
                        info = kfdbg_ping(transport)
                        self.transport = transport
                        display = self._describe_transport(transport)
                        self.result_queue.put({"type": "connect", "ok": True,
                                                "info": info, "display": display})
                    elif kind == "disconnect":
                        if self.transport is not None:
                            self.transport.close()
                            self.transport = None
                        self.result_queue.put({"type": "disconnect", "ok": True})
                    elif kind == "state":
                        state = kfdbg_state(self.transport)
                        self.result_queue.put({"type": "state", "ok": True, "data": state})
                    elif kind == "press":
                        note = kfdbg_press(self.transport, item["mask"], item["hold_ms"])
                        self.result_queue.put({"type": "press", "ok": True,
                                                "label": item["label"],
                                                "hold_ms": item["hold_ms"], "note": note})
                    elif kind == "save_shot":
                        path, rgb = save_screenshot(self.transport)
                        self.result_queue.put({"type": "save_shot", "ok": True,
                                                "path": path, "rgb": rgb})
                    elif kind == "advance":
                        note = kfdbg_advance(self.transport, item["seconds"])
                        self.result_queue.put({"type": "advance", "ok": True,
                                                "label": item["label"],
                                                "seconds": item["seconds"], "note": note})
                    elif kind == "reset":
                        note = kfdbg_reset(self.transport)
                        self.result_queue.put({"type": "reset", "ok": True, "note": note})
                    elif kind == "mult":
                        note = kfdbg_mult(self.transport, item["factor"])
                        self.result_queue.put({"type": "mult", "ok": True,
                                                "factor": item["factor"], "note": note})
                except kfd.KfDebugError as e:
                    self.result_queue.put({"type": kind, "ok": False, "error": str(e)})
                except Exception as e:  # noqa: BLE001 -- must never kill this thread
                    self.result_queue.put({"type": kind, "ok": False,
                                            "error": f"unexpected error: {e}"})

        # ------------------------------------------------------------
        # Result draining -- runs on the Tk thread via root.after()
        # ------------------------------------------------------------

        def _drain_results(self):
            try:
                while True:
                    result = self.result_queue.get_nowait()
                    self._apply_result(result)
            except queue.Empty:
                pass
            self.root.after(50, self._drain_results)

        def _apply_result(self, result):
            kind = result["type"]
            if kind == "connect":
                if self.target_kind != "demo":
                    self.connect_btn.configure(
                        state="normal", text="Disconnect" if result["ok"] else "Connect")
                if result["ok"]:
                    self.connected = True
                    self.conn_state_var.set(f"Connected to {result['display']}.")
                    self.conn_state_label.configure(style="ConnGood.TLabel")
                    self.status_var.set(f"Connected. {result['info']}")
                else:
                    self.connected = False
                    error = result["error"]
                    if self.target_kind == "serial" and \
                            "no serial port looked like" in error:
                        self.conn_state_var.set("Not connected — no device found.")
                    else:
                        self.conn_state_var.set(f"Connection failed: {error}")
                    self.conn_state_label.configure(style="ConnBad.TLabel")
                    self.status_var.set(f"Connect failed: {error}")
            elif kind == "disconnect":
                self.connected = False
                if self.target_kind != "demo":
                    self.connect_btn.configure(state="normal", text="Connect")
                self.conn_state_var.set("Not connected.")
                self.conn_state_label.configure(style="ConnNeutral.TLabel")
                self.status_var.set("Disconnected.")
            elif kind == "state":
                self._state_pending = False
                if result["ok"]:
                    self._update_state_readout(result["data"])
                    self._last_ping_var.set(
                        f"last updated {time.strftime('%H:%M:%S')}")
                else:
                    self._last_ping_var.set(f"state poll failed: {result['error']}")
            elif kind == "press":
                if result["ok"]:
                    verb = f"held {result['hold_ms']}ms" if result["hold_ms"] else "tapped"
                    self.status_var.set(f"{result['label']} {verb} -- {result['note']}")
                else:
                    self.status_var.set(f"button press failed: {result['error']}")
            elif kind == "save_shot":
                self._shot_pending = False
                self.shot_btn.configure(state="normal", text="Save Screenshot (Cmd/Ctrl+S)")
                if result["ok"]:
                    self.shot_path_var.set(result["path"])
                    self.shot_path_entry.selection_range(0, "end")
                    self.status_var.set(f"Saved {result['path']}")
                    self._update_thumbnail(result["rgb"])
                else:
                    self.status_var.set(f"Screenshot failed: {result['error']}")
            elif kind == "advance":
                if result["ok"]:
                    self.status_var.set(f"{result['label']} -- {result['note']}")
                    self._request_state_refresh()
                else:
                    self.status_var.set(f"Time skip failed: {result['error']}")
            elif kind == "reset":
                if result["ok"]:
                    self.status_var.set(f"Reset to a fresh egg -- {result['note']}")
                    self._request_state_refresh()
                else:
                    self.status_var.set(f"Reset failed: {result['error']}")
            elif kind == "mult":
                if result["ok"]:
                    self.status_var.set(
                        f"Time multiplier now {result['factor']}x -- {result['note']}")
                    self._request_state_refresh()
                else:
                    self.status_var.set(f"Time multiplier failed: {result['error']}")

        def _update_state_readout(self, data):
            keys = list(STATE_FIELD_ORDER) + \
                [k for k in sorted(data) if k not in STATE_FIELD_ORDER]
            keys = [k for k in keys if k in data]
            for i, key in enumerate(keys):
                if key not in self._state_row_widgets:
                    key_lbl = ttk.Label(self.state_grid, text=key, style="Muted.TLabel")
                    val_lbl = ttk.Label(self.state_grid, text="", foreground=COLOR_FG,
                                         background=COLOR_BG,
                                         font=("TkDefaultFont", 10, "bold"))
                    key_lbl.grid(row=i, column=0, sticky="w", padx=(0, 12), pady=1)
                    val_lbl.grid(row=i, column=1, sticky="w", pady=1)
                    self._state_row_widgets[key] = (key_lbl, val_lbl)
                else:
                    key_lbl, val_lbl = self._state_row_widgets[key]
                    key_lbl.grid(row=i, column=0)
                    val_lbl.grid(row=i, column=1)
                self._state_row_widgets[key][1].configure(text=str(data[key]))

        def _update_thumbnail(self, rgb_bytes):
            ppm = rgb888_to_ppm(rgb_bytes, kfd.FB_WIDTH, kfd.FB_HEIGHT)
            photo = tk.PhotoImage(data=ppm, format="PPM")
            self.thumb_label.configure(image=photo, text="", width=kfd.FB_WIDTH,
                                        height=kfd.FB_HEIGHT)
            self._last_thumb_photo = photo  # keep a reference, Tk needs it kept alive

        # ------------------------------------------------------------
        # Periodic polling
        # ------------------------------------------------------------

        def _poll_state_tick(self):
            self._request_state_refresh()
            interval_ms = max(200, int(self.state_interval.get() * 1000))
            self.root.after(interval_ms, self._poll_state_tick)

        def _on_save_screenshot(self):
            if not self.connected:
                self.status_var.set("Not connected -- can't take a screenshot.")
                return
            if self._shot_pending:
                return
            self._shot_pending = True
            self.shot_btn.configure(state="disabled", text="Capturing...")
            self.status_var.set("Fetching a fresh screenshot (takes a second or two)...")
            self.cmd_queue.put({"type": "save_shot"})


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------

def build_parser():
    p = argparse.ArgumentParser(
        prog="kf_panel.py",
        description="A live control panel for Kamiframe hardware: a "
                     "button pad, a live pet-state readout, and an "
                     "on-demand screenshot button, over USB (or --demo "
                     "with no hardware at all).")
    p.add_argument("--target", default="serial:auto",
                    help="serial:<port>, serial:auto (default), "
                         "sim:<host>:<port> (reserved, not implemented "
                         "yet), or demo")
    p.add_argument("--demo", "--fake", dest="demo", action="store_true",
                    help="skip real hardware and drive the panel from an "
                         "in-process fake device")
    p.add_argument("--baud", type=int, default=115200,
                    help="serial baud rate (default 115200)")
    p.add_argument("--state-interval", type=float, default=1.0,
                    help="seconds between state-readout polls once "
                         "connected (default 1.0)")
    p.add_argument("--verbose", action="store_true",
                    help="dump raw protocol lines to stderr, same as "
                         "kf_debug.py --verbose")
    return p


def main(argv=None):
    args = build_parser().parse_args(argv)

    if tk is None:
        print(
            "error: tkinter is not available in this Python install.\n"
            "kf_panel.py needs it (it's normally part of the Python "
            "standard library, but some installs -- notably some "
            "Homebrew Python versions on macOS -- ship without it). "
            "Try `brew install python-tk`, or run this with a different "
            "Python that has tkinter (the system /usr/bin/python3 on "
            "macOS usually does), or use tools/kf_debug.py from the "
            "command line instead.",
            file=sys.stderr)
        return 1

    try:
        target = ("demo", None) if args.demo else parse_target(args.target)
    except kfd.KfDebugError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    root = tk.Tk()
    PanelApp(root, target=target, baud=args.baud,
              state_interval=args.state_interval, verbose=args.verbose)
    root.mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
