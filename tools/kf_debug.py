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
        KFDBG CLOCK DROWSY|BEDTIME|MORNING
        KFDBG CLOCK EPOCH <seconds>
        KFDBG RTC
        KFDBG SCANLINE [read_hz]
        KFDBG VSYNC <0|1>
        KFDBG FEED <variation>
        KFDBG PLAY <variation>
        KFDBG REST <variation>
        KFDBG BATH <variation>
        KFDBG FLUSH
        KFDBG JUMP <stage> [teen_form] [adult_branch]

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
    python3 tools/kf_debug.py [--port PORT] clock drowsy    # or bedtime, morning
    python3 tools/kf_debug.py [--port PORT] clock 1737936000  # an explicit epoch
    python3 tools/kf_debug.py [--port PORT] rtc              # read the DS3231 directly
    python3 tools/kf_debug.py [--port PORT] reset           # back to a fresh egg
    python3 tools/kf_debug.py [--port PORT] watch [--interval 1.0]
    python3 tools/kf_debug.py [--port PORT] scanline [--read-hz 2000000]
    python3 tools/kf_debug.py [--port PORT] vsync on|off
    python3 tools/kf_debug.py [--port PORT] care 1            # feed, variation 0
    python3 tools/kf_debug.py [--port PORT] care feed 2       # feed, variation 2
    python3 tools/kf_debug.py [--port PORT] care 5            # flush (no variation)
    python3 tools/kf_debug.py [--port PORT] jump teen         # start of teen stage
    python3 tools/kf_debug.py [--port PORT] jump teen --teen-form 1

See tools/README.md for a plain-language walkthrough.
"""

import argparse
import base64
import binascii
import calendar
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

# How many ways there are to do each of feed/play/rest/bath, mirrored from
# KF_PET_CARE_VARIATION_COUNT (hakoniwaos/include/kf/pet.h) for the same
# reason BUTTON_BITS above mirrors kf/types.h -- no machine-readable link
# to the C build, on purpose, so if Core's count ever changes this needs a
# matching edit, not a silent mismatch.
CARE_VARIATION_COUNT = 3

# The five care actions, in the same order the desktop simulator binds them
# to number keys 1-5 (simulator/src/sdl/sdl_input.cpp,
# simulator/src/pet/kf_home_screen_input.cpp's
# kf_home_screen_handle_care_buttons()) -- feed/play/rest/bath/flush.
# `care <n>` below accepts either the digit or
# the name, so a script that has memorised "1=feed" from the desktop
# keyboard binding works here unchanged. flush takes no variation, per
# kf_pet_flush() (kf/pet.h).
CARE_ACTIONS = ["feed", "play", "rest", "bath", "flush"]
CARE_ACTION_ALIASES = {str(i + 1): name for i, name in enumerate(CARE_ACTIONS)}
CARE_ACTION_ALIASES.update({name: name for name in CARE_ACTIONS})

# Life stages, mirrored from the kf_pet_stage enum in
# hakoniwaos/include/kf/pet.h -- same "no machine-readable link" caveat as
# BUTTON_BITS/CARE_VARIATION_COUNT above.
STAGE_NAMES = {"egg": 0, "baby": 1, "child": 2, "teen": 3, "adult": 4}

# KF_PET_TEEN_FORM_DUST (kf/pet.h): deliberately equal to
# KF_PET_TEEN_FORM_COUNT (4), one past the four named teen forms -- a real,
# reachable form (an uncared-for teen), not an error value. `jump`'s
# --teen-form accepts 0..KF_PET_TEEN_FORM_DUST inclusive, matching the
# device side's own range (see handle_jump()'s comment in
# ports/esp32/main/kf_dbg_bridge.cpp) rather than silently clamping it away.
TEEN_FORM_DUST = 4

# The three named points `clock` can jump the world clock to, mirroring the
# desktop debug window's Drowsy/Bedtime/Morning buttons (sdl_debug_window.cpp)
# and kf_pet_session.h's kf_pet_debug_clock_point enum -- the actual times
# (21:50:05 / 22:00:05 / 07:00:05) are defined exactly once, on the device
# side (kf_pet_session_debug_clock_target()), not duplicated here: this host
# script only ever sends the NAME and lets the firmware resolve it, so the
# two can never drift apart the way an earlier version of the desktop
# buttons once did against their own test.
CLOCK_POINTS = ("drowsy", "bedtime", "morning")


def local_epoch_now():
    """This machine's LOCAL wall clock, as the unlabelled epoch Kamiframe
    uses everywhere.

    NOT time.time(), and the difference is the entire point. Kamiframe has
    no timezone anywhere by design -- hakoniwaos/include/kf/clock.h is
    explicit that the epoch it is handed IS local time -- while
    time.time(), `date +%s` and datetime.timestamp() all report UTC.
    Handing the device a UTC value sets its clock wrong for everybody not
    sitting on the prime meridian.

    Not hypothetical: `kf_debug.py clock $(date +%s)` was offered as the
    way to fix a drifted board and set it four hours fast, because the
    person running it is on UTC-4. The same mistake was in the simulator's
    own boot clock and in the panel's Sync Clock button. All of them now
    route through a local-time helper.

    calendar.timegm() interprets a time tuple as UTC, so feeding it the
    LOCAL broken-down time yields exactly "local civil time expressed as an
    epoch" -- the same trick simulator/src/host/host_time.cpp's
    kf_host_time_system_now() uses, for the same reason. time.localtime()
    resolves DST correctly for a given instant, which hand-rolled offset
    arithmetic gets wrong twice a year.
    """
    return calendar.timegm(time.localtime())


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


def parse_stage(text):
    """'teen' -> 3, '3' -> 3 -- accepts either a name (STAGE_NAMES above)
    or a raw 0-4 decimal, since KFDBG JUMP's own wire syntax takes the raw
    enum value and a name is friendlier to type at a prompt."""
    t = text.strip().lower()
    if t in STAGE_NAMES:
        return STAGE_NAMES[t]
    try:
        v = int(t)
    except ValueError:
        valid = ", ".join(STAGE_NAMES)
        raise KfDebugError(
            f"unknown stage '{text}' -- use a name ({valid}) or 0-4") from None
    if not 0 <= v <= 4:
        raise KfDebugError(f"stage must be 0-4 (or a name), got {v}")
    return v


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
    """Send `command`, expect a reply of `expected_type` back.

    An `err` reply -- including the device refusing a mutating command
    (FEED/PLAY/REST/BATH/FLUSH/JUMP/ADVANCE/RESET/MULT/press) because its
    build has KF_DBG_MUTATE_ENABLE=0, or refuses `press`/BTNHOLD
    specifically because KF_DBG_INPUT_INJECT_ENABLE=0, or refuses
    scanline/vsync because the active panel profile has no read line to
    poll (kf_panel_profile.h's has_read_line; ADR 0039) -- becomes a
    KfDebugError carrying the device's own message verbatim, which already
    names the exact flag or panel to blame (ports/esp32/main/
    kf_dbg_bridge.cpp's require_mutate_enabled()/require_read_line(); see
    ADR 0035, ADR 0039). This is deliberately generic: every cmd_*() below
    that calls this function gets a comprehensible rejection for free, with
    no per-command error handling needed here, and no separate capability
    probe before sending -- the wire round trip already tells the caller in
    one message rather than two.
    """
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


def _format_budget_line(obj):
    """One extra line summarising the frame-budget fields KFDBG STATE
    carries (ADR 0036: draw/transfer/cpu/post time, dirty-rect area, pixel
    counts by bucket, and the rolling summary) -- on top of, not instead
    of, the per-key dump _print_state_line() already prints, since those
    already show every field including these; this just puts the numbers
    Chris actually came here to sanity-check on one line together.

    .get() with a "?" default throughout, not obj["draw_us"] etc.: an
    older firmware on the bench predates every one of these keys (a real
    bring-up scenario, not a hypothetical), and a KeyError here would read
    as a hardware fault rather than a version mismatch."""
    g = lambda key: obj.get(key, "?")  # noqa: E731
    return (f"budget: draw={g('draw_us')}us xfer={g('transfer_us')}us "
            f"cpu={g('cpu_us')}us post={g('post_us')}us "
            f"rects={g('dirty_rects')} ({g('dirty_pct')}%) "
            f"px={g('opaque_px')}+{g('keyed_px')} "
            f"over_budget={g('over_budget')} worst={g('worst_us')}us "
            f"p99={g('p99_us')}us frames={g('frames')} "
            f"over={g('over_budget_frames')}")


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
    print(_format_budget_line(obj))


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


def cmd_save(link, args):
    """Force a save checkpoint now.

    The pet saves after every care action and on a dirty-gated timer (ADR
    0056), so this is not needed to avoid losing progress -- it is here to
    make the save PATH testable on demand: pull the plug immediately after
    and you know exactly what should come back.
    """
    payload = _expect(link, "KFDBG SAVE", "ack")
    print(f"saved -- {payload.decode('utf-8', 'replace')}")


def cmd_nextstage(link, args):
    """Advance one life stage from wherever the pet is now.

    The counterpart to `jump`, which takes an absolute stage. Adult is
    terminal, so from Adult this refills needs and clears sickness without
    moving the stage marker -- see run_next_stage() in kf_debug_actions.cpp.
    """
    payload = _expect(link, "KFDBG NEXTSTAGE", "ack")
    print(f"advanced one stage -- {payload.decode('utf-8', 'replace')}")


def cmd_screen(link, args):
    """Advance to the next screen WITHOUT firing a MENU button edge.

    Deliberately different from `press menu`: a real MENU edge also toggles
    Core's on-device HUD (kf/app.cpp). This changes screen and nothing else,
    which is what makes it useful for stepping through screens while
    watching the frame counters.
    """
    payload = _expect(link, "KFDBG SCREEN", "ack")
    print(f"next screen -- {payload.decode('utf-8', 'replace')}")


def cmd_mult(link, args):
    if not 1 <= args.factor <= 256:
        raise KfDebugError(
            f"time multiplier must be between 1 and 256, got {args.factor}")
    payload = _expect(link, f"KFDBG MULT {args.factor}", "ack")
    print(f"time multiplier now {args.factor}x -- "
          f"{payload.decode('utf-8', 'replace')}")


def cmd_clock(link, args):
    """Jump the world clock to a point in the sleep cycle, or to an
    explicit epoch -- the hardware equivalent of the desktop debug window's
    Drowsy/Bedtime/Morning buttons, plus one thing those buttons don't
    offer: an arbitrary epoch, which is what makes a drifted real-time-clock
    DATE on a real board fixable without opening a Settings screen that only
    edits hour and minute (see ports/esp32/README.md's former "no way to
    set the date from the device" open question).

    `target` is either one of CLOCK_POINTS (case-insensitive) or a plain
    decimal number of seconds since 1970 -- same "name or raw number"
    convention `jump`'s stage argument already uses.

    Moves BOTH the device's RAM wall clock and Core's own notion of "now"
    together (see kf_pet_session_debug_set_clock()'s header comment in
    simulator/src/pet/kf_pet_session.h for why one alone is not enough) --
    NOT just kf_time_set_wall(), so this is not the same thing as (and is
    not a substitute for) the Settings screen's own clock control.
    """
    text = args.target.strip()
    point = text.lower()
    if point == "sync":
        # The named target that means "this machine's clock, right now".
        # Exists so nobody has to reach for `clock $(date +%s)`, which is
        # UTC and therefore wrong -- see local_epoch_now().
        epoch = local_epoch_now()
        payload = _expect(link, f"KFDBG CLOCK EPOCH {epoch}", "ack")
        print(f"clock -- {payload.decode('utf-8', 'replace')}")
        print("  synced to this machine's local time: "
              + time.strftime("%Y-%m-%d %H:%M:%S", time.localtime()))
        return
    if point in CLOCK_POINTS:
        command = f"KFDBG CLOCK {point.upper()}"
        payload = _expect(link, command, "ack")
        print(f"clock -- {payload.decode('utf-8', 'replace')}")
        return
    try:
        epoch = int(text)
    except ValueError:
        valid = ", ".join(CLOCK_POINTS)
        raise KfDebugError(
            f"unknown clock target '{args.target}' -- use a point name "
            f"({valid}) or a decimal epoch (seconds since 1970)") from None
    payload = _expect(link, f"KFDBG CLOCK EPOCH {epoch}", "ack")
    when = time.strftime("%Y-%m-%d %H:%M:%S UTC", time.gmtime(epoch))
    print(f"clock -- {payload.decode('utf-8', 'replace')} ({when})")


def cmd_rtc(link, args):
    """Read the DS3231 real-time-clock chip's registers DIRECTLY over I2C
    -- NOT kf_time_wall(), the in-RAM clock KFDBG STATE and everything else
    on this device implicitly relies on. That distinction is the entire
    point: this is the one command that can prove the RAM clock and the
    physical chip haven't drifted apart, and the only way to observe the
    chip's OSF (oscillator-stopped) flag remotely -- see
    ports/esp32/main/kf_dbg_bridge.cpp's handle_rtc() and Task 5 of
    the screens/clock/sleep plan, whose
    coin-cell-removed negative case has no other way to be checked without
    standing at the bench reading a boot log.

    Observe tier: works even with KF_DBG_MUTATE_ENABLE=0, since reading a
    chip's registers changes nothing. Raises KfDebugError (via `err`) if no
    DS3231 ever answered at boot -- there is nothing to read in that case,
    not a present:false reply to parse.
    """
    payload = _expect(link, "KFDBG RTC", "json")
    text = payload.decode("utf-8", "replace")
    if args.json:
        print(text)
        return
    try:
        obj = json.loads(text)
    except ValueError:
        print(text)
        return
    for k in sorted(obj):
        print(f"  {k}: {obj[k]}")
    print()
    if obj.get("osf"):
        print("VERDICT: OSF is set -- the chip does not trust its own "
              "registers (dead/missing backup cell, or never seeded). "
              "`epoch` above is not meaningful until it is reseeded.")
    elif obj.get("wall_valid"):
        drift = obj.get("wall", 0) - obj.get("epoch", 0)
        print(f"VERDICT: chip is healthy (OSF clear); RAM clock and chip "
              f"agree within {drift:+d}s.")
    else:
        print("VERDICT: chip is healthy (OSF clear), but the RAM wall "
              "clock is unset -- set it from the Settings screen or "
              "`kf_debug.py clock`.")


def cmd_scanline(link, args):
    """Ask the panel where its scan currently is, 64 times.

    Answers one question: can this display tell us when it is safe to write?
    It has no TE pin, so polling Get_scanline (0x45) is the only candidate,
    and nobody knows whether this module answers reads at all.

    The device reads at a slow, dedicated clock for the duration of this
    probe (2MHz by default -- the ILI9341's read cycle is only rated to
    about 6MHz, well under the 40MHz this panel normally writes at), not
    the display's own write clock. --read-hz overrides that from here, so a
    human can try 1MHz or 4MHz without reflashing. The screen will visibly
    glitch while this runs -- the firmware tears the panel down and rebuilds
    it, twice -- and that is expected, not a bug.

    A 1/2/4MHz sweep has since confirmed which framing this panel actually
    uses (no dummy byte), so the unprefixed fields in the reply below
    (value_min/max, distinct_values, increases/decreases,
    changed_between_reads) are now THAT framing, not the ILI9341 datasheet's
    documented one -- the alt_-prefixed fields are the datasheet framing
    instead, kept for comparison. dummy_bytes_assumed says which is which
    (0 -- the unprefixed fields assume no dummy byte).

    The firmware makes a good-faith attempt to leave the panel in a clean
    state afterwards (an extra reset settle delay -- see esp_display.cpp's
    rebuild_panel_io()), but that fix has not been verified on real
    hardware. If the screen shows horizontal banding after this command
    runs, that is the known, expected failure mode -- a power cycle (or
    just letting the pet redraw a full frame) should clear it.
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
        print("VERDICT: reads succeeded but the value never changed (no-dummy-byte "
              "framing, the one confirmed correct on this panel). That is a stuck "
              "register, not a scan counter -- not usable at this read_hz.")
    elif inc > dec * 2:
        print("VERDICT: looks like a real scan counter (it advances and wraps). "
              "Beam-racing may be viable -- the remaining question is whether "
              "avg_read_us is cheap enough to poll within a frame.")
    else:
        print("VERDICT: values change but do not advance consistently under the "
              "confirmed no-dummy-byte framing. That reads as noise rather than a "
              "counter at this read_hz.")
    if alt_distinct > 1 and alt_inc > alt_dec * 2 and not (inc > dec * 2):
        print("NOTE: the OLD datasheet framing (1 dummy byte -- see "
              "alt_sample_values) looks more like a real, advancing counter than "
              "the confirmed one above did at this read_hz. Worth double-checking "
              "which framing this specific board actually needs.")


def cmd_vsync(link, args):
    """Turn beam-racing on or off: push_rect() waiting for the panel's scan
    to clear a rectangle before writing it, instead of racing it blind.

    Takes effect on the device's very next write -- no reflash, no reset.
    Compare `state` (or `watch`) before and after to see the effect: fps and
    frame_us are the bottom line, and vsync_rects_written/vsync_rects_waited/
    vsync_avg_wait_us (also in KFDBG STATE) show whether the wait is actually
    doing anything or costing nothing.
    """
    enable = args.setting == "on"
    payload = _expect(link, f"KFDBG VSYNC {1 if enable else 0}", "ack")
    print(f"vsync now {'on' if enable else 'off'} -- "
          f"{payload.decode('utf-8', 'replace')}")


def cmd_care(link, args):
    """Fire one of the five care actions -- feed/play/rest/bath/flush --
    against the live pet, by name or by the same 1-5 digit the desktop
    simulator binds these to (see CARE_ACTION_ALIASES above).

    Calls KFDBG FEED/PLAY/REST/BATH/FLUSH directly, NOT `press A`/`press
    UP`/etc. (KFDBG BTN). Two reasons, matching the device-side reasoning
    in ports/esp32/main/kf_dbg_bridge.cpp's handle_feed() comment:

      1. On the desktop keyboard, which of the KF_PET_CARE_VARIATION_COUNT
         variations a press fires is an implicit counter that cycles with
         each press of the SAME key -- state this CLI has no way to see or
         resync with (a fresh `kf_debug.py` process every invocation has
         no memory of how many times a real device's button was pressed
         before). Passing --variation explicitly here is unambiguous every
         time, which is what a scriptable command needs.
      2. A bare `press` defaults to a 120ms hold specifically because a
         one-shot injection does not clear Core's debounce filter (see
         that command's own --hold-ms comment) -- worth paying for
         testing input handling itself, irrelevant noise for testing a
         care action's effect. KFDBG FEED et al. skip debounce entirely by
         calling the session function directly, same as the device side.
    """
    action = CARE_ACTION_ALIASES.get(args.action)
    if action is None:
        valid = ", ".join(f"{i + 1}={name}" for i, name in enumerate(CARE_ACTIONS))
        raise KfDebugError(f"unknown care action '{args.action}' -- valid: {valid}")

    if action == "flush":
        if args.variation is not None:
            raise KfDebugError("flush takes no --variation (kf_pet_flush() has none)")
        payload = _expect(link, "KFDBG FLUSH", "ack")
        print(f"flush -- {payload.decode('utf-8', 'replace')}")
        return

    variation = args.variation if args.variation is not None else 0
    if not 0 <= variation < CARE_VARIATION_COUNT:
        raise KfDebugError(
            f"variation must be 0..{CARE_VARIATION_COUNT - 1}, got {variation}")
    payload = _expect(link, f"KFDBG {action.upper()} {variation}", "ack")
    print(f"{action} variation={variation} -- {payload.decode('utf-8', 'replace')}")


def cmd_jump(link, args):
    """Jump the pet to the START of a life stage -- alive, not sick, not
    dead, every need topped up -- without living through the real
    (hour-to-week-scale) stage durations first.

    Exists for exactly the case an uncared-for pet on real hardware makes
    otherwise unreachable: a pet neglected past the child stage dies, and a
    dead pet is frozen permanently, so nothing later than that is ever
    visible on a real device without this.

    teen_form/adult_branch default to 0 (kf_pet_session_debug_jump_to_
    stage()'s own "unset" behaviour) and are only meaningful once `stage`
    has passed their own branch point -- see that function's header
    comment in simulator/src/pet/kf_pet_session.h. teen_form's valid range
    is 0..TEEN_FORM_DUST (4) INCLUSIVE: the dust form is a real, reachable
    form (an uncared-for teen), not an error value, so this CLI accepts it
    like any other in-range input rather than silently clamping it away.
    adult_branch's valid range depends on which teen_form was picked (a
    per-family adult count Core computes, not something this host-side
    script duplicates) -- out-of-range input there is not rejected here;
    the device falls it back to 0 itself, same as an omitted value.
    """
    stage = parse_stage(args.stage)
    teen_form = args.teen_form if args.teen_form is not None else 0
    if not 0 <= teen_form <= TEEN_FORM_DUST:
        raise KfDebugError(
            f"--teen-form must be 0..{TEEN_FORM_DUST} ({TEEN_FORM_DUST} = the "
            f"dust form), got {teen_form}")
    adult_branch = args.adult_branch if args.adult_branch is not None else 0
    if adult_branch < 0:
        raise KfDebugError(f"--adult-branch must be >= 0, got {adult_branch}")
    payload = _expect(link, f"KFDBG JUMP {stage} {teen_form} {adult_branch}", "ack")
    print(f"jump -- {payload.decode('utf-8', 'replace')}")


def _format_watch_summary(obj):
    """The curated line `watch` prints once per poll -- fps/cpu_us/post_us/
    dirty_rects (ADR 0036), the frame-budget numbers this command exists to
    watch move in real time, not the full key dump `state` prints: this is
    the command Chris will actually stare at for minutes at a time during
    bring-up, and every other field (pet stats, vsync counters, ...) is a
    `kf_debug.py state` away and would just make each line harder to read
    at a glance here. .get() with a "?" default, not obj["fps"] etc., for
    the same reason _format_budget_line() gives: older firmware on the
    bench predates these keys, and a KeyError mid-watch would look like the
    device dropped off the wire, not like a version mismatch."""
    g = lambda key: obj.get(key, "?")  # noqa: E731
    return (f"fps={g('fps')} cpu_us={g('cpu_us')} "
            f"post_us={g('post_us')} rects={g('dirty_rects')}")


def cmd_watch(link, args):
    print("watching state, Ctrl-C to stop", file=sys.stderr)
    try:
        while True:
            payload = _expect(link, "KFDBG STATE", "json")
            text = payload.decode("utf-8", "replace")
            try:
                summary = _format_watch_summary(json.loads(text))
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

    sub.add_parser("save", parents=[common],
                    help="force a save checkpoint now")

    sub.add_parser("nextstage", parents=[common],
                    help="advance one life stage from wherever the pet is")

    sub.add_parser("screen", parents=[common],
                    help="advance to the next screen (no MENU button edge)")

    mult = sub.add_parser("mult", parents=[common],
                           help="set the pet's time multiplier (1-256)")
    mult.add_argument("factor", type=int,
                       help="1 is normal speed, 256 is the fastest")

    clock = sub.add_parser("clock", parents=[common],
                            help="jump the world clock to a point in the "
                                 "sleep cycle, or to an explicit epoch")
    clock.add_argument("target",
                        help="sync|drowsy|bedtime|morning, or a decimal epoch "
                             "(seconds since 1970)")

    rtc = sub.add_parser("rtc", parents=[common],
                          help="read the DS3231 directly over I2C -- "
                               "observe tier, works even with "
                               "KF_DBG_MUTATE_ENABLE=0")
    rtc.add_argument("--json", action="store_true",
                      help="print the raw JSON instead of a summary")

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

    vsync = sub.add_parser("vsync", parents=[common],
                            help="turn beam-racing (wait for the scan before "
                                 "writing) on or off, default off")
    vsync.add_argument("setting", choices=["on", "off"],
                        help="on or off")

    # 1-5 accepted alongside the names, matching the desktop simulator's own
    # number-key binding (sdl_input.cpp) key for key -- see CARE_ACTION_
    # ALIASES above.
    care = sub.add_parser("care", parents=[common],
                           help="fire a care action: feed/play/rest/bath/flush "
                                "(1-5, same order as the desktop's number keys)")
    care.add_argument("action", choices=sorted(CARE_ACTION_ALIASES),
                       help="1=feed 2=play 3=rest 4=bath 5=flush, or the name")
    care.add_argument("--variation", type=int, default=None,
                       help=f"0..{CARE_VARIATION_COUNT - 1} (default 0; "
                            "flush takes none)")

    jump = sub.add_parser("jump", parents=[common],
                           help="jump the pet to the start of a life stage "
                                "(alive, fully fed) -- for looking at a "
                                "stage's sprites without raising it for real")
    jump.add_argument("stage", help="egg|baby|child|teen|adult, or 0-4")
    jump.add_argument("--teen-form", type=int, default=None,
                       help=f"0-{TEEN_FORM_DUST} ({TEEN_FORM_DUST} = the dust "
                            "form); meaningful once stage >= teen (default 0)")
    jump.add_argument("--adult-branch", type=int, default=None,
                       help="meaningful once stage == adult; out-of-range "
                            "falls back to 0 on the device (default 0)")

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
            elif args.command == "save":
                cmd_save(link, args)
            elif args.command == "nextstage":
                cmd_nextstage(link, args)
            elif args.command == "screen":
                cmd_screen(link, args)
            elif args.command == "mult":
                cmd_mult(link, args)
            elif args.command == "clock":
                cmd_clock(link, args)
            elif args.command == "rtc":
                cmd_rtc(link, args)
            elif args.command == "scanline":
                cmd_scanline(link, args)
            elif args.command == "watch":
                cmd_watch(link, args)
            elif args.command == "vsync":
                cmd_vsync(link, args)
            elif args.command == "care":
                cmd_care(link, args)
            elif args.command == "jump":
                cmd_jump(link, args)
    except KfDebugError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("\ninterrupted", file=sys.stderr)
        return 130
    return 0


if __name__ == "__main__":
    sys.exit(main())
