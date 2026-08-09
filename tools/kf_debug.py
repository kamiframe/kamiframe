#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright the Kamiframe contributors.
"""Talk to Kamiframe firmware over the USB debug serial console.

The firmware exposes a tiny text-command / framed-binary-reply protocol on
the same UART that `idf.py monitor` uses. This lets a developer -- or an AI
assistant working on the code with no hardware in front of it -- pull a real
screenshot off the device, read the pet's live state, and press buttons
remotely, without a camera pointed at the screen.

Wire protocol (fixed; the device side implements the exact same spec):

    Host -> device, one line, ASCII, newline-terminated:
        KFDBG PING
        KFDBG SHOT
        KFDBG STATE
        KFDBG BTN <mask>
        KFDBG BTNHOLD <mask> <ms>
        KFDBG ADVANCE <seconds>
        KFDBG RESET
        KFDBG MULT <n>

    Device -> host, a framed block. Ordinary firmware log lines may appear
    interleaved between (never inside) these blocks and are skipped:

        KFDBG-BEGIN <type> <length>
        <payload, base64, wrapped at any width the device chooses>
        KFDBG-END <crc32-hex>

    <length> is the base64 character count. The CRC32 in KFDBG-END covers
    the *decoded* (post-base64) payload bytes, for every reply type -- for
    an `fb` reply those decoded bytes are themselves an RLE stream that gets
    a second decompression pass; for `pong`/`json`/`ack`/`err` they are
    plain UTF-8 text. See the module docstring in kf_debug_selftest.py for
    why this reading was chosen where the spec was ambiguous.

    fb payload, once base64-decoded: a repeated stream of
    <uint16 count><uint16 pixel>, little-endian, count >= 1, decompressing
    to exactly 240*320*2 = 153600 bytes of RGB565 little-endian pixels,
    row-major, 240 wide by 320 high.

Usage:
    python3 tools/kf_debug.py [--port PORT] ping
    python3 tools/kf_debug.py [--port PORT] shot [--out FILE.png]   # default: ~/Downloads
    python3 tools/kf_debug.py [--port PORT] state [--json]
    python3 tools/kf_debug.py [--port PORT] press UP,A [--hold-ms 300]
    python3 tools/kf_debug.py [--port PORT] advance 1d      # or 30s, 5m, 2h, 1w
    python3 tools/kf_debug.py [--port PORT] mult 64         # 1..256
    python3 tools/kf_debug.py [--port PORT] reset           # back to a fresh egg
    python3 tools/kf_debug.py [--port PORT] watch [--interval 1.0]

See tools/README.md for a plain-language walkthrough.
"""

import argparse
import base64
import binascii
import json
import os
import re
import struct
import sys
import time
import zlib

FB_WIDTH = 240
FB_HEIGHT = 320
FB_PIXEL_BYTES = FB_WIDTH * FB_HEIGHT * 2

DEFAULT_TIMEOUT = 5.0   # seconds, for ping/state/press/ack-sized replies
SHOT_TIMEOUT = 30.0     # seconds, a full screenshot is much bigger

# Button bit values, mirrored from the kf_button enum in
# hakoniwaos/include/kf/types.h (the enum input.h actually uses). If that
# enum ever changes, this table needs to change with it -- there is no
# machine-readable link between the two, on purpose, because this file must
# not reach into the C build.
BUTTON_BITS = {
    "UP": 1 << 0,
    "DOWN": 1 << 1,
    "LEFT": 1 << 2,
    "RIGHT": 1 << 3,
    "A": 1 << 4,
    "B": 1 << 5,
    "MENU": 1 << 6,
}

def resolve_shot_path(out):
    """Where a screenshot should land.

    Default is the user's Downloads folder rather than the current directory,
    because the command is nearly always run from wherever you happen to be
    standing -- often deep inside ports/esp32 -- and a PNG dropped there is a
    PNG you have to go hunting for. Downloads is somewhere both a person and
    an assistant can reliably find afterwards.

    An explicit --out always wins, and `~` is expanded so `--out ~/foo.png`
    behaves the way it looks. If Downloads does not exist (a stripped-down
    Linux box, a CI container), fall back to the current directory rather than
    inventing folders in someone's home.
    """
    if out:
        return os.path.abspath(os.path.expanduser(out))
    downloads = os.path.expanduser("~/Downloads")
    directory = downloads if os.path.isdir(downloads) else os.getcwd()
    return os.path.join(directory, "kf_shot.png")


_BEGIN_RE = re.compile(r"^KFDBG-BEGIN (\S+) (\d+)$")
_END_RE = re.compile(r"^KFDBG-END ([0-9a-fA-F]+)$")

# A payload line: nothing but the base64 alphabet, optional '=' padding at the
# very end. Used to tell payload apart from firmware log lines sharing the
# same UART -- see the comment in read_frame()'s payload loop.
_B64_LINE_RE = re.compile(r"^[A-Za-z0-9+/]+={0,2}$")


class KfDebugError(Exception):
    """Anything that should stop the CLI with a plain-English message."""


class ProtocolError(KfDebugError):
    """The device said something that does not match the wire format."""


class CrcMismatchError(ProtocolError):
    """A frame decoded, but its CRC32 does not match what the device sent."""


class DeviceTimeoutError(KfDebugError):
    """No (complete) reply arrived in time."""


# --------------------------------------------------------------------------
# Frame parsing. This is the part the selftest exercises directly: it is
# handed a fake `readline` and must behave identically to how it behaves
# against a real serial port.
# --------------------------------------------------------------------------

def read_frame(readline, overall_timeout=DEFAULT_TIMEOUT, verbose=False):
    """Read one KFDBG-BEGIN/END block, skipping ordinary log lines first.

    `readline` is a zero-argument callable returning one line as bytes
    (trailing newline optional), or b"" to mean "no data arrived on this
    attempt" -- which is how pyserial reports its own per-call timeout, and
    also how a test double simulates a device that has gone silent. This
    function keeps calling it until either a complete, CRC-verified frame
    has been read, or `overall_timeout` seconds have passed with no
    progress.

    Returns (frame_type, decoded_payload_bytes).
    """
    deadline = time.monotonic() + overall_timeout

    def next_line():
        raw = readline()
        if not raw:
            return None
        line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
        if verbose:
            print(f"<< {line}", file=sys.stderr)
        return line

    # Phase 1: skip ordinary log lines until KFDBG-BEGIN turns up.
    frame_type = None
    b64_len = None
    while True:
        if time.monotonic() > deadline:
            raise DeviceTimeoutError(
                f"no reply from device within {overall_timeout:.1f}s -- is "
                "the board plugged into the UART port, and is `idf.py "
                "monitor` (or another program) holding the port open? A "
                "busy port is the most common cause of this.")
        line = next_line()
        if line is None or line == "":
            continue
        m = _BEGIN_RE.match(line)
        if m:
            frame_type, b64_len = m.group(1), int(m.group(2))
            break
        # An ordinary firmware log line between frames. Tolerated, skipped.

    # Phase 2: accumulate base64 payload lines until b64_len chars arrive.
    chunks = []
    got = 0
    while got < b64_len:
        if time.monotonic() > deadline:
            raise DeviceTimeoutError(
                f"device started a `{frame_type}` reply but never finished "
                f"it (got {got}/{b64_len} payload characters) -- the "
                "connection may have dropped mid-transfer.")
        line = next_line()
        if line is None:
            continue
        # Two things this deliberately does NOT do.
        #
        # It does not enforce a maximum line length. That used to reject
        # anything over 76 characters because the spec suggested that wrap
        # width; the device wraps at 88, and a real screenshot failed on a
        # cosmetic disagreement. Wrap width is the sender's business. The
        # declared length below and the CRC32 catch real corruption.
        #
        # It does not assume the payload arrives uninterrupted. The bridge
        # shares one UART with ordinary KF_LOG output, and those logs come
        # from a different task than the transfer, so a screenshot that takes
        # seconds to stream WILL have a log line land in the middle of it --
        # the frame budget report alone prints once a second. Treating those
        # as payload is what produced "17036 declared, 17081 arrived".
        #
        # Base64 is a strict alphabet, and log lines are full of spaces and
        # brackets, so telling them apart is reliable. A log line that happened
        # to be pure base64 characters would slip through, but then the CRC32
        # rejects the frame rather than handing back a silently corrupted
        # screenshot -- which is the failure mode that matters.
        if not _B64_LINE_RE.match(line):
            continue
        chunks.append(line)
        got += len(line)
    if got != b64_len:
        raise ProtocolError(
            f"payload length mismatch: KFDBG-BEGIN said {b64_len} base64 "
            f"chars, but {got} arrived before KFDBG-END")

    # Phase 3: the closing line, with the CRC.
    crc_hex = None
    while crc_hex is None:
        if time.monotonic() > deadline:
            raise DeviceTimeoutError(
                f"`{frame_type}` reply payload arrived complete but "
                "KFDBG-END never came")
        line = next_line()
        if line is None or line == "":
            continue
        m = _END_RE.match(line)
        if not m:
            raise ProtocolError(f"expected KFDBG-END, got: {line!r}")
        crc_hex = m.group(1)

    b64_text = "".join(chunks)
    try:
        decoded = base64.b64decode(b64_text, validate=True)
    except (binascii.Error, ValueError) as e:
        raise ProtocolError(f"payload is not valid base64: {e}") from e

    expected_crc = int(crc_hex, 16)
    actual_crc = zlib.crc32(decoded) & 0xFFFFFFFF
    if actual_crc != expected_crc:
        raise CrcMismatchError(
            f"CRC32 mismatch on `{frame_type}` reply: device said "
            f"{expected_crc:08x}, decoded payload is {actual_crc:08x}. "
            "The transfer was corrupted in transit -- treat this as a "
            "communication fault, not a rendering bug, and retry.")

    return frame_type, decoded


def decode_rle(data, expected_len):
    """Decompress a <uint16 count><uint16 pixel> LE RLE stream."""
    out = bytearray()
    i, n = 0, len(data)
    while i < n:
        if i + 4 > n:
            raise ProtocolError("RLE stream truncated mid-record")
        count, pixel = struct.unpack_from("<HH", data, i)
        i += 4
        if count == 0:
            raise ProtocolError("RLE record has count=0, protocol requires >=1")
        out += struct.pack("<H", pixel) * count
    if len(out) != expected_len:
        raise ProtocolError(
            f"decompressed framebuffer is {len(out)} bytes, expected "
            f"{expected_len} (240*320*2)")
    return bytes(out)


def rgb565_to_rgb888(px):
    """RGB565 -> RGB888, replicating high bits into low bits so white
    (0xFFFF) comes out (255, 255, 255), not (248, 252, 248)."""
    r5 = (px >> 11) & 0x1F
    g6 = (px >> 5) & 0x3F
    b5 = px & 0x1F
    r8 = (r5 << 3) | (r5 >> 2)
    g8 = (g6 << 2) | (g6 >> 4)
    b8 = (b5 << 3) | (b5 >> 2)
    return r8, g8, b8


# --------------------------------------------------------------------------
# PNG writing, standard library only (zlib + struct). No third-party image
# dependency, per the task's constraint.
# --------------------------------------------------------------------------

_PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def _png_chunk(tag, data):
    return (struct.pack(">I", len(data)) + tag + data +
            struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))


def png_encode(width, height, rgb_bytes):
    """RGB888, row-major, 3 bytes/pixel -> a minimal truecolor PNG."""
    if len(rgb_bytes) != width * height * 3:
        raise ValueError("rgb_bytes does not match width*height*3")
    stride = width * 3
    raw = bytearray()
    for y in range(height):
        raw.append(0)  # filter type 0 (None) for every scanline
        raw += rgb_bytes[y * stride:(y + 1) * stride]
    compressed = zlib.compress(bytes(raw), 9)
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    return (_PNG_SIGNATURE + _png_chunk(b"IHDR", ihdr) +
            _png_chunk(b"IDAT", compressed) + _png_chunk(b"IEND", b""))


def png_decode(data):
    """Minimal reader for the selftest: only handles what png_encode writes
    (8-bit truecolor, filter type 0 on every row, one IHDR, any number of
    IDAT chunks). Not a general PNG decoder."""
    if data[:8] != _PNG_SIGNATURE:
        raise ValueError("not a PNG (bad signature)")
    pos = 8
    width = height = None
    idat = bytearray()
    while pos < len(data):
        (length,) = struct.unpack_from(">I", data, pos)
        tag = data[pos + 4:pos + 8]
        chunk = data[pos + 8:pos + 8 + length]
        pos += 8 + length + 4  # + CRC, unchecked here
        if tag == b"IHDR":
            width, height = struct.unpack_from(">II", chunk, 0)
        elif tag == b"IDAT":
            idat += chunk
        elif tag == b"IEND":
            break
    raw = zlib.decompress(bytes(idat))
    stride = width * 3
    rgb = bytearray()
    p = 0
    for _ in range(height):
        filter_type = raw[p]
        p += 1
        if filter_type != 0:
            raise ValueError("png_decode only supports filter type 0")
        rgb += raw[p:p + stride]
        p += stride
    return width, height, bytes(rgb)


# --------------------------------------------------------------------------
# Buttons
# --------------------------------------------------------------------------

def parse_buttons(spec):
    mask = 0
    for name in spec.split(","):
        name = name.strip().upper()
        if not name:
            continue
        if name not in BUTTON_BITS:
            valid = ", ".join(sorted(BUTTON_BITS))
            raise KfDebugError(f"unknown button '{name}' -- valid names: {valid}")
        mask |= BUTTON_BITS[name]
    if mask == 0:
        raise KfDebugError("no buttons given")
    return mask


# --------------------------------------------------------------------------
# Serial link. pyserial is the one allowed third-party dependency; it is
# only imported here, lazily, so that importing this module (e.g. for the
# selftest, or for `--help`) never requires it to be installed.
# --------------------------------------------------------------------------

class SerialLink:
    def __init__(self, port, baud=115200, verbose=False):
        try:
            import serial
        except ImportError as e:
            raise KfDebugError(
                "pyserial is not installed. Install it with:\n\n"
                "    pip install pyserial\n\n"
                "It's the only third-party dependency kf_debug.py needs."
            ) from e
        self.verbose = verbose
        try:
            self._ser = serial.Serial(port, baud, timeout=0.2)
        except serial.SerialException as e:
            raise KfDebugError(
                f"could not open {port}: {e}\n"
                "A busy port is the most common cause -- close `idf.py "
                "monitor` or any other program using this port and try "
                "again."
            ) from e

    def send(self, command):
        if self.verbose:
            print(f">> {command}", file=sys.stderr)
        self._ser.reset_input_buffer()
        self._ser.write((command + "\n").encode("ascii"))
        self._ser.flush()

    def read_frame(self, overall_timeout=DEFAULT_TIMEOUT):
        return read_frame(self._ser.readline, overall_timeout=overall_timeout,
                           verbose=self.verbose)

    def readline(self):
        """The raw per-call-timeout readline() read_frame() is built on,
        exposed directly. Callers that want the framing/CRC/log-skipping
        logic should use read_frame() above -- this exists so something
        outside this module (kf_panel.py's transport layer) can hand
        read_frame() (the free function) its own line source without
        reaching into the pyserial object this class wraps."""
        return self._ser.readline()

    def close(self):
        self._ser.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc_info):
        self.close()


_PORT_DEVICE_HINTS = (
    "usbserial", "usbmodem", "slab_usbtouart", "wchusbserial",
    "ttyusb", "ttyacm",
)
_PORT_DESC_HINTS = (
    "cp210", "ch340", "ch910", "ftdi", "silicon labs",
    "usb serial", "usb-serial", "uart bridge", "esp32",
)


def _looks_like_kf_port(port_info):
    dev = (port_info.device or "").lower()
    desc = (port_info.description or "").lower()
    manu = (getattr(port_info, "manufacturer", "") or "").lower()
    return (any(h in dev for h in _PORT_DEVICE_HINTS) or
            any(h in desc for h in _PORT_DESC_HINTS) or
            any(h in manu for h in _PORT_DESC_HINTS))


def find_port(verbose=False):
    try:
        from serial.tools import list_ports
    except ImportError as e:
        raise KfDebugError(
            "pyserial is not installed, so the port can't be auto-detected.\n"
            "Install it with:\n\n    pip install pyserial\n\n"
            "or pass --port explicitly (e.g. --port /dev/cu.usbserial-1420)."
        ) from e

    ports = list(list_ports.comports())
    if verbose:
        for p in ports:
            print(f"   saw port: {p.device} ({p.description})", file=sys.stderr)

    matches = [p for p in ports if _looks_like_kf_port(p)]
    if len(matches) == 1:
        chosen = matches[0]
        print(f"Auto-detected port: {chosen.device} ({chosen.description})",
              file=sys.stderr)
        return chosen.device

    if not matches:
        seen = "\n".join(f"  {p.device} ({p.description})" for p in ports)
        raise KfDebugError(
            "no serial port looked like a Kamiframe board.\n"
            + ("Ports seen:\n" + seen if ports else "No serial ports were seen at all.")
            + "\n\nPass --port explicitly, e.g. --port /dev/cu.usbserial-1420"
        )

    listed = "\n".join(f"  {p.device} ({p.description})" for p in matches)
    raise KfDebugError(
        f"multiple candidate ports found -- pick one with --port:\n{listed}")


# --------------------------------------------------------------------------
# Commands
# --------------------------------------------------------------------------

def _expect(link, command, expected_type, timeout=DEFAULT_TIMEOUT):
    link.send(command)
    frame_type, payload = link.read_frame(overall_timeout=timeout)
    if frame_type == "err":
        raise KfDebugError(
            f"device rejected `{command}`: {payload.decode('utf-8', 'replace')}")
    if frame_type != expected_type:
        raise ProtocolError(
            f"expected a `{expected_type}` reply to `{command}`, got `{frame_type}`")
    return payload


def cmd_ping(link, args):
    payload = _expect(link, "KFDBG PING", "pong")
    print(payload.decode("utf-8", "replace"))


def cmd_shot(link, args):
    payload = _expect(link, "KFDBG SHOT", "fb", timeout=SHOT_TIMEOUT)
    raw = decode_rle(payload, FB_PIXEL_BYTES)

    rgb = bytearray(FB_WIDTH * FB_HEIGHT * 3)
    idx = 0
    for (px,) in struct.iter_unpack("<H", raw):
        r, g, b = rgb565_to_rgb888(px)
        rgb[idx], rgb[idx + 1], rgb[idx + 2] = r, g, b
        idx += 3

    png_bytes = png_encode(FB_WIDTH, FB_HEIGHT, bytes(rgb))
    out_path = resolve_shot_path(args.out)
    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    with open(out_path, "wb") as f:
        f.write(png_bytes)
    print(f"wrote {out_path} ({FB_WIDTH}x{FB_HEIGHT})")


def _print_state_line(payload, as_json):
    text = payload.decode("utf-8", "replace")
    if as_json:
        print(text)
        return
    # Field names aren't pinned down by the protocol spec, so this prints
    # whatever keys the device actually sends rather than guessing at names.
    try:
        obj = json.loads(text)
    except ValueError:
        print(text)
        return
    for key, value in obj.items():
        print(f"{key}: {value}")


def cmd_state(link, args):
    payload = _expect(link, "KFDBG STATE", "json")
    _print_state_line(payload, args.json)


# Suffixes so "skip a day" is `advance 1d` rather than `advance 86400`. The
# bare-number case stays seconds, so nothing that already worked breaks.
_DURATION_UNITS = {"s": 1, "m": 60, "h": 3600, "d": 86400, "w": 604800}


def parse_duration(text):
    """'90' -> 90, '5m' -> 300, '1d' -> 86400, '2w' -> 1209600."""
    t = text.strip().lower()
    if not t:
        raise KfDebugError("empty duration")
    unit = 1
    if t[-1] in _DURATION_UNITS:
        unit = _DURATION_UNITS[t[-1]]
        t = t[:-1]
    try:
        value = float(t)
    except ValueError:
        raise KfDebugError(
            f"could not read '{text}' as a duration. Use plain seconds, or a "
            "suffix: 30s, 5m, 2h, 1d, 1w.") from None
    if value <= 0:
        raise KfDebugError("duration must be positive")
    return int(value * unit)


def cmd_press(link, args):
    mask = parse_buttons(args.buttons)
    if args.hold_ms:
        command = f"KFDBG BTNHOLD {mask} {args.hold_ms}"
    else:
        command = f"KFDBG BTN {mask}"
    payload = _expect(link, command, "ack")
    note = payload.decode("utf-8", "replace")
    print(f"ack{': ' + note if note else ''}")


def cmd_advance(link, args):
    """Jump the pet forward in time.

    Exists because the pet is otherwise untestable on hardware: an egg lasts
    an hour and does not decay at all, and after hatching hunger falls at
    1042 mp/hour -- four real days from full to empty. Watching a board for
    an hour shows you roughly 1% of one bar moving.
    """
    seconds = parse_duration(args.duration)
    payload = _expect(link, f"KFDBG ADVANCE {seconds}", "ack")
    print(f"advanced {seconds}s ({args.duration}) -- "
          f"{payload.decode('utf-8', 'replace')}")


def cmd_reset(link, args):
    payload = _expect(link, "KFDBG RESET", "ack")
    print(f"reset to a fresh egg -- {payload.decode('utf-8', 'replace')}")


def cmd_mult(link, args):
    if not 1 <= args.factor <= 256:
        raise KfDebugError(
            f"time multiplier must be between 1 and 256, got {args.factor}")
    payload = _expect(link, f"KFDBG MULT {args.factor}", "ack")
    print(f"time multiplier now {args.factor}x -- "
          f"{payload.decode('utf-8', 'replace')}")


def cmd_scanline(link, args):
    """Ask the panel where its scan currently is, 64 times.

    Answers one question: can this display tell us when it is safe to write?
    It has no TE pin, so polling Get_scanline (0x45) is the only candidate,
    and nobody knows whether this module answers reads at all.

    The device reads at a slow, dedicated clock for the duration of this
    probe (2MHz by default -- the ILI9341's read cycle is only rated to
    about 6MHz, well under the 40MHz the display normally writes at), not
    the display's own write clock. --read-hz overrides that from here, so a
    human can try 1MHz or 4MHz without reflashing. The screen will visibly
    glitch while this runs -- the firmware tears the panel down and rebuilds
    it, twice -- and that is expected, not a bug.
    """
    command = "KFDBG SCANLINE"
    if args.read_hz is not None:
        if args.read_hz <= 0:
            raise KfDebugError("--read-hz must be a positive number of Hz")
        command += f" {args.read_hz}"
    payload = _expect(link, command, "json", timeout=15.0)
    text = payload.decode("utf-8", "replace")
    if args.json:
        print(text)
        return
    try:
        d = json.loads(text)
    except ValueError:
        print(text)
        return
    for k in sorted(d):
        print(f"  {k}: {d[k]}")
    distinct = d.get("distinct_values", 0)
    inc, dec = d.get("increases", 0), d.get("decreases", 0)
    alt_distinct = d.get("alt_distinct_values", 0)
    alt_inc, alt_dec = d.get("alt_increases", 0), d.get("alt_decreases", 0)
    print()
    if not d.get("probe_ok", True):
        print("NOTE: the device could not even rebuild the panel at the requested "
              "read_hz -- the reads below failed for that reason, not because the "
              "register itself is unusable. See the device's serial log.")
    if d.get("ok", 0) == 0:
        print("VERDICT: the panel did not answer a single read. Beam-racing is "
              "not possible on this module -- either SDO is not wired through, "
              "or it does not respond at this SPI clock.")
    elif distinct <= 1:
        print("VERDICT: reads succeeded but the value never changed (datasheet "
              "framing). That is a stuck register, not a scan counter -- not "
              "usable, at least under that framing.")
    elif inc > dec * 2:
        print("VERDICT: looks like a real scan counter (it advances and wraps). "
              "Beam-racing may be viable -- the remaining question is whether "
              "avg_read_us is cheap enough to poll within a frame.")
    else:
        print("VERDICT: values change but do not advance consistently under the "
              "datasheet's documented framing (1 dummy byte, 10-bit value). That "
              "reads as noise rather than a counter for that framing.")
    if alt_distinct > 1 and alt_inc > alt_dec * 2 and not (inc > dec * 2):
        print("NOTE: the ALTERNATE framing (no dummy byte -- see alt_sample_values) "
              "looks more like a real, advancing counter than the primary one did. "
              "Worth trying that framing for real if the primary one above reads "
              "as noise or stuck.")


def cmd_watch(link, args):
    print("watching state, Ctrl-C to stop", file=sys.stderr)
    try:
        while True:
            payload = _expect(link, "KFDBG STATE", "json")
            text = payload.decode("utf-8", "replace")
            try:
                obj = json.loads(text)
                summary = " ".join(f"{k}={v}" for k, v in obj.items())
            except ValueError:
                summary = text
            print(f"{time.strftime('%H:%M:%S')} {summary}")
            time.sleep(args.interval)
    except KeyboardInterrupt:
        print("stopped", file=sys.stderr)


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------

def build_parser():
    p = argparse.ArgumentParser(
        prog="kf_debug.py",
        description="Talk to Kamiframe firmware over the USB debug serial "
                     "console: screenshot the real screen, read live pet "
                     "state, and press buttons, without hardware in front "
                     "of you.")
    # --port and --verbose are attached BOTH to the top-level parser and, via
    # `parents=`, to every subcommand. argparse would otherwise accept only
    # `kf_debug.py --verbose shot` and reject `kf_debug.py shot --verbose`
    # with an unhelpful "unrecognized arguments" -- a footgun that has already
    # cost real time at the bench, where the natural thing to type is the
    # command first and the flag after it. Both orders now work.
    # default=SUPPRESS on the shared copies matters. Without it the subparser
    # writes its OWN default over whatever the top-level parser already
    # stored, so `kf_debug.py --verbose shot` would silently come back with
    # verbose=False -- the flag accepted and then thrown away, which is worse
    # than rejecting it. With SUPPRESS the subparser only sets the attribute
    # when the flag actually appears after the subcommand, and the top-level
    # value survives otherwise.
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--port", default=argparse.SUPPRESS,
                         help="serial device, e.g. /dev/cu.usbserial-1420. "
                              "Auto-detected if omitted.")
    common.add_argument("--verbose", action="store_true",
                         default=argparse.SUPPRESS,
                         help="dump raw protocol lines to stderr")

    p.add_argument("--port",
                    help="serial device, e.g. /dev/cu.usbserial-1420. "
                         "Auto-detected if omitted.")
    p.add_argument("--verbose", action="store_true",
                    help="dump raw protocol lines to stderr")

    sub = p.add_subparsers(dest="command", required=True)

    sub.add_parser("ping", parents=[common],
                    help="check the device is alive, print build info")

    shot = sub.add_parser("shot", parents=[common],
                           help="capture the live screen as a PNG")
    shot.add_argument("--out", default=None,
                       help="output PNG path (default: ~/Downloads/kf_shot.png)")

    state = sub.add_parser("state", parents=[common],
                            help="print the pet's current state")
    state.add_argument("--json", action="store_true",
                        help="print the raw JSON line instead of a summary")

    press = sub.add_parser("press", parents=[common],
                            help="simulate a button press")
    press.add_argument("buttons",
                        help="comma-separated button names, e.g. UP,A")
    # Default 120ms, NOT 0, and the reason is a real bug found on hardware.
    #
    # `KFDBG BTN` injects a one-shot mask that applies to exactly one
    # kf_input_poll() call and then clears itself. Core debounces buttons
    # (kDebounceUs = 8000 in hakoniwaos/src/app.cpp): a value must read the
    # SAME across consecutive polls, at least 8ms apart, before it becomes
    # the stable state and produces a press edge. Polls happen once per
    # frame, ~33ms apart.
    #
    # So a one-shot injection can never register. Poll N sees the button and
    # starts a debounce candidate; poll N+1, 33ms later, sees zero because
    # the mask already cleared, and the candidate resets. The button is
    # acknowledged over the wire and then silently discarded -- which is
    # exactly what it looked like: `ack: BTN mask=64` followed by a screen
    # that did not change.
    #
    # 120ms spans roughly four frames, so the mask is present for several
    # consecutive polls and debounce resolves normally. It is also a
    # realistic human press, which is what this flag is simulating.
    press.add_argument("--hold-ms", type=int, default=120,
                        help="hold the button(s) for this many milliseconds "
                             "(default 120; must exceed core's 8ms debounce "
                             "across consecutive frame polls, so 0 will "
                             "NOT register)")

    advance = sub.add_parser("advance", parents=[common],
                              help="jump the pet forward in time")
    advance.add_argument("duration",
                          help="seconds, or with a suffix: 30s, 5m, 2h, 1d, 1w")

    sub.add_parser("reset", parents=[common],
                    help="reset the pet to a fresh egg")

    mult = sub.add_parser("mult", parents=[common],
                           help="set the pet's time multiplier (1-256)")
    mult.add_argument("factor", type=int,
                       help="1 is normal speed, 256 is the fastest")

    scanline = sub.add_parser("scanline", parents=[common],
                               help="probe whether the panel can report its "
                                    "scan position (beam-racing feasibility)")
    scanline.add_argument("--json", action="store_true",
                           help="print the raw JSON instead of a summary")
    scanline.add_argument("--read-hz", type=int, default=None,
                           help="SPI clock to read the scanline register at, "
                                "in Hz (default: the firmware's own default, "
                                "2MHz). The ILI9341's read cycle is roughly "
                                "150ns (~6MHz max) per the datasheet, so "
                                "values much above that are expected to look "
                                "like noise again.")

    watch = sub.add_parser("watch", parents=[common],
                            help="print device state repeatedly")
    watch.add_argument("--interval", type=float, default=1.0,
                        help="seconds between polls (default: 1.0)")

    return p


def main(argv=None):
    args = build_parser().parse_args(argv)
    try:
        port = args.port or find_port(verbose=args.verbose)
        with SerialLink(port, verbose=args.verbose) as link:
            if args.command == "ping":
                cmd_ping(link, args)
            elif args.command == "shot":
                cmd_shot(link, args)
            elif args.command == "state":
                cmd_state(link, args)
            elif args.command == "press":
                cmd_press(link, args)
            elif args.command == "advance":
                cmd_advance(link, args)
            elif args.command == "reset":
                cmd_reset(link, args)
            elif args.command == "mult":
                cmd_mult(link, args)
            elif args.command == "scanline":
                cmd_scanline(link, args)
            elif args.command == "watch":
                cmd_watch(link, args)
    except KfDebugError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("\ninterrupted", file=sys.stderr)
        return 130
    return 0


if __name__ == "__main__":
    sys.exit(main())
