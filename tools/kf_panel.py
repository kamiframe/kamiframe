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
        return {
            "stage": self.stage,
            "base_trait": self.trait,
            "hunger": self.hunger,
            "happiness": self.happiness,
            "energy": self.energy,
            "time_in_stage_s": round(elapsed, 1),
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

    class Tooltip:
        """A small delayed label that follows the mouse over one widget.
        Used on the disabled simulator-debug controls to explain *why*
        they're disabled without needing a status bar essay for each one."""

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
                              background="#ffffe0", relief="solid",
                              borderwidth=1, font=("TkDefaultFont", 9),
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
    STATE_FIELD_ORDER = [
        "stage", "base_trait", "trait", "hunger", "happiness", "energy",
        "time_in_stage_s", "free_heap_bytes", "free_psram_bytes", "fps",
        "frame_time_ms",
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

            root.title("Kamiframe Hardware Panel")
            root.protocol("WM_DELETE_WINDOW", self.on_close)

            self._build_ui()
            self._bind_keys()

            if self.target_kind == "demo":
                self.status_var.set("Demo mode -- connecting to the fake device...")
                self._connect()

            self.root.after(50, self._drain_results)
            self.root.after(int(self.state_interval.get() * 1000), self._poll_state_tick)

        # ------------------------------------------------------------
        # UI construction
        # ------------------------------------------------------------

        def _build_ui(self):
            root = self.root
            pad = {"padx": 6, "pady": 4}

            top = ttk.Frame(root)
            top.pack(fill="x", **pad)
            self._build_connection_controls(top)

            body = ttk.Frame(root)
            body.pack(fill="both", expand=True, **pad)

            left = ttk.Frame(body)
            left.pack(side="left", fill="y", padx=(0, 12))
            self._build_button_pad(left)
            self._build_screenshot_controls(left)

            right = ttk.Frame(body)
            right.pack(side="left", fill="both", expand=True)
            self._build_state_readout(right)
            self._build_sim_controls(right)

            status = ttk.Frame(root)
            status.pack(fill="x", **pad)
            self.status_var = tk.StringVar(value="Not connected.")
            ttk.Label(status, textvariable=self.status_var,
                      foreground="#555").pack(side="left")

        def _build_connection_controls(self, parent):
            frame = ttk.LabelFrame(parent, text="Connection")
            frame.pack(fill="x")

            if self.target_kind == "demo":
                ttk.Label(frame, text="DEMO MODE -- driving an in-process "
                                       "fake device, no hardware needed.",
                          foreground="#a05a00").grid(row=0, column=0,
                                                      columnspan=4, sticky="w",
                                                      padx=6, pady=4)
                return

            ttk.Label(frame, text="Port:").grid(row=0, column=0, padx=(6, 2), pady=6)
            self.port_var = tk.StringVar(
                value=self.target_arg if self.target_arg not in (None, "auto") else "")
            self.port_combo = ttk.Combobox(frame, textvariable=self.port_var, width=28)
            self.port_combo.grid(row=0, column=1, padx=2, pady=6)
            self._refresh_ports()

            ttk.Button(frame, text="Rescan", command=self._refresh_ports).grid(
                row=0, column=2, padx=2, pady=6)

            self.connect_btn = ttk.Button(frame, text="Connect", command=self._on_connect_click)
            self.connect_btn.grid(row=0, column=3, padx=(10, 6), pady=6)

            ttk.Label(frame, text="State poll (s):").grid(row=0, column=4, padx=(10, 2))
            ttk.Spinbox(frame, from_=0.2, to=10.0, increment=0.1, width=5,
                        textvariable=self.state_interval).grid(row=0, column=5, padx=2)

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
            frame.pack(fill="x", pady=(0, 8))

            grid = ttk.Frame(frame)
            grid.grid(row=0, column=0, padx=10, pady=10)

            def make(name, r, c, cspan=1):
                b = tk.Button(grid, text=name, width=6, height=2)
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
                      foreground="#555", wraplength=260,
                      justify="left").grid(row=1, column=0, padx=10, pady=(0, 8))

        def _build_screenshot_controls(self, parent):
            frame = ttk.LabelFrame(parent, text="Screenshot")
            frame.pack(fill="x")

            self.shot_btn = ttk.Button(frame, text="Save Screenshot (Cmd/Ctrl+S)",
                                        command=self._on_save_screenshot)
            self.shot_btn.pack(fill="x", padx=8, pady=(8, 4))

            ttk.Label(frame, text="Fetches a fresh frame from the device -- "
                                   "takes a second or two, that's expected, "
                                   "not a hang.", foreground="#555",
                      wraplength=260, justify="left").pack(padx=8, anchor="w")

            self.shot_path_var = tk.StringVar(value="")
            self.shot_path_entry = ttk.Entry(frame, textvariable=self.shot_path_var,
                                              state="readonly", width=32)
            self.shot_path_entry.pack(fill="x", padx=8, pady=(4, 8))

            self.thumb_label = tk.Label(frame, text="No screenshot yet",
                                         width=30, height=16, relief="sunken",
                                         background="#222", foreground="#888")
            self.thumb_label.pack(padx=8, pady=(0, 8))

        def _build_state_readout(self, parent):
            frame = ttk.LabelFrame(parent, text="Pet state (live)")
            frame.pack(fill="x", pady=(0, 8))
            self.state_grid = ttk.Frame(frame)
            self.state_grid.pack(fill="x", padx=8, pady=8)
            self._state_row_widgets = {}  # field name -> (key label, value label)
            self._last_ping_var = tk.StringVar(value="")
            ttk.Label(frame, textvariable=self._last_ping_var,
                      foreground="#555").pack(anchor="w", padx=8, pady=(0, 6))

        def _build_sim_controls(self, parent):
            # ------------------------------------------------------------
            # Placeholder for the simulator debug window's controls
            # (simulator/src/sdl/sdl_debug_window.cpp), laid out now so
            # wiring them up later is a small change, not a redesign.
            #
            # KFDBG has no commands for any of this yet. Every control
            # below is disabled and tooltipped for that reason. When
            # commands land, wire each one inside _on_sim_button() --
            # that's the one place to touch; nothing about the layout
            # needs to change.
            #
            # One of these will likely never work against real hardware:
            # the seekable timeline needs the simulator's 2048-entry debug
            # snapshot ring (200KB+), which is compiled out on the ESP32
            # build (KF_PET_SESSION_ENABLE_DEBUG_TOOLS=0) because the
            # device only has 512KB of RAM total and the framebuffer alone
            # is 150KB of it. So even once wired, expect it to stay
            # unavailable on a real board and work only against the
            # simulator.
            # ------------------------------------------------------------
            frame = ttk.LabelFrame(
                parent, text="Simulator debug controls "
                             "(not supported by this connection yet)")
            frame.pack(fill="x")

            row1 = ttk.Frame(frame)
            row1.pack(fill="x", padx=8, pady=(8, 2))
            for label in ("Skip 1 Hour", "Skip 1 Day", "Skip 1 Week", "Reset Egg"):
                self._make_disabled_sim_button(row1, label)

            mult_frame = ttk.Frame(frame)
            mult_frame.pack(fill="x", padx=8, pady=2)
            ttk.Label(mult_frame, text="Time multiplier:").pack(side="left", padx=(0, 6))
            for mult in (1, 2, 4, 8, 16, 32, 64, 128, 256):
                self._make_disabled_sim_button(mult_frame, f"{mult}x", width=4)

            timeline_frame = ttk.Frame(frame)
            timeline_frame.pack(fill="x", padx=8, pady=(6, 2))
            ttk.Label(timeline_frame, text="Pet-age timeline (seek):").pack(anchor="w")
            scale = ttk.Scale(timeline_frame, from_=0, to=100, orient="horizontal")
            scale.state(["disabled"])
            scale.pack(fill="x", pady=2)
            Tooltip(scale, "No KFDBG command for this yet -- and it likely "
                            "never will work on real hardware, only the "
                            "simulator (see the code comment above "
                            "_build_sim_controls). Reserved space, not "
                            "wired up.")
            ttk.Label(timeline_frame, text="egg  →  baby  →  child  →  "
                                            "teen  →  adult",
                      foreground="#888", font=("TkDefaultFont", 8)).pack(anchor="w")

        def _make_disabled_sim_button(self, parent, label, width=None):
            kwargs = {"width": width} if width else {}
            btn = ttk.Button(parent, text=label, state="disabled", **kwargs)
            btn.pack(side="left", padx=2, pady=2)
            Tooltip(btn, "The device doesn't support this yet -- KFDBG has "
                         "no command for it. This button is reserved space "
                         "for when one is added; see _on_sim_button() in "
                         "kf_panel.py for where to wire it.")
            return btn

        def _on_sim_button(self, name):
            # Reserved: the single place to dispatch a simulator-debug
            # control once KFDBG grows commands for it. Every caller above
            # is currently disabled, so this is unreachable today -- left
            # in place, and commented, so wiring a new command later means
            # adding one branch here rather than redesigning the panel.
            raise NotImplementedError(
                f"'{name}' has no KFDBG command yet -- see the comment "
                "above _build_sim_controls()")

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
                self.status_var.set("Disconnecting...")
                return
            self.target_arg = self.port_var.get() or "auto"
            self._connect()

        def _connect(self):
            if self.target_kind != "demo":
                self.connect_btn.configure(state="disabled")
            self.status_var.set("Connecting...")
            self.cmd_queue.put({"type": "connect"})

        def on_close(self):
            self.cmd_queue.put({"type": "shutdown"})
            self.root.destroy()

        # ------------------------------------------------------------
        # Worker thread -- ALL serial/socket/fake-device I/O happens here.
        # Tk callbacks only ever enqueue work and read results off
        # result_queue via root.after(); nothing above this method touches
        # a Transport directly, on purpose (see the module docstring on
        # threading in the task this file was built against).
        # ------------------------------------------------------------

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
                        self.result_queue.put({"type": "connect", "ok": True, "info": info})
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
                    self.status_var.set(f"Connected. {result['info']}")
                else:
                    self.connected = False
                    self.status_var.set(f"Connect failed: {result['error']}")
            elif kind == "disconnect":
                self.connected = False
                if self.target_kind != "demo":
                    self.connect_btn.configure(state="normal", text="Connect")
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

        def _update_state_readout(self, data):
            keys = list(STATE_FIELD_ORDER) + \
                [k for k in sorted(data) if k not in STATE_FIELD_ORDER]
            keys = [k for k in keys if k in data]
            for i, key in enumerate(keys):
                if key not in self._state_row_widgets:
                    key_lbl = ttk.Label(self.state_grid, text=key, foreground="#777")
                    val_lbl = ttk.Label(self.state_grid, text="", font=("TkDefaultFont", 10, "bold"))
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
            if self.connected and not self._state_pending:
                self._state_pending = True
                self.cmd_queue.put({"type": "state"})
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
