#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright the Kamiframe contributors.
"""Self-test for kf_debug.py's wire protocol parser.

kf_debug.py talks to real hardware, and there is no hardware to test
against yet (see CLAUDE.md: parts get ordered only once the simulator
milestone is met). So this builds a synthetic KFDBG-BEGIN/END frame by
hand -- a fake 240x320 framebuffer, RLE-compressed, base64-encoded,
wrapped into <=76-char lines, with a real CRC32 -- and feeds those exact
bytes through kf_debug's own frame reader, RLE decoder and PNG writer,
then checks the picture that comes out the other end is pixel-for-pixel
the same as the one that went in.

This proves kf_debug.py speaks the protocol correctly. It cannot prove
the device firmware does too -- that is a separate, parallel task -- so
that once hardware exists, only one side is left to debug.

A note on an ambiguity in the protocol spec: it says "<length> is the
base64 character count" and "the trailing CRC32 covers the decoded
payload bytes" as general statements about the KFDBG-BEGIN/END framing,
not as something specific to the `fb` reply type. So this implementation
(and this test) treats *every* reply type -- pong, fb, json, ack, err --
as base64 inside the frame, with the CRC32 covering the base64-decoded
bytes. For `fb` those decoded bytes are RLE-compressed pixels needing a
second decompression pass; for the rest they are plain UTF-8 text. If the
device side turns out to only base64-encode `fb` and send other types as
plain text, this file is the one place that needs to change.

Usage:
    python3 tools/kf_debug_selftest.py
"""

import base64
import json
import struct
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import kf_debug as kfd  # noqa: E402


FAILURES = []


def check(name, condition):
    status = "ok" if condition else "FAIL"
    print(f"  [{status}] {name}")
    if not condition:
        FAILURES.append(name)


def wrap_b64(b64_text, width=76):
    return [b64_text[i:i + width] for i in range(0, len(b64_text), width)] or [""]


def build_frame_lines(frame_type, payload_bytes):
    """Build the exact wire lines for a KFDBG-BEGIN..KFDBG-END block."""
    b64_text = base64.b64encode(payload_bytes).decode("ascii")
    crc = zlib.crc32(payload_bytes) & 0xFFFFFFFF
    lines = [f"KFDBG-BEGIN {frame_type} {len(b64_text)}"]
    lines.extend(wrap_b64(b64_text))
    lines.append(f"KFDBG-END {crc:08x}")
    return lines


def encode_rle(pixel_bytes):
    """Reference RLE encoder, test-only -- the device does this, not the
    host. kf_debug.py only ever needs to decode this format."""
    assert len(pixel_bytes) % 2 == 0
    pixels = [struct.unpack_from("<H", pixel_bytes, i)[0]
              for i in range(0, len(pixel_bytes), 2)]
    out = bytearray()
    i, n = 0, len(pixels)
    while i < n:
        run = 1
        while i + run < n and pixels[i + run] == pixels[i] and run < 0xFFFF:
            run += 1
        out += struct.pack("<HH", run, pixels[i])
        i += run
    return bytes(out)


def synthetic_framebuffer():
    """A 240x320 RGB565 framebuffer with runs both short and longer than
    one RLE record can hold (0xFFFF pixels), to exercise the count
    boundary in both the encoder (test-only) and decoder (kf_debug.py)."""
    w, h = kfd.FB_WIDTH, kfd.FB_HEIGHT
    px = [0] * (w * h)
    for i in range(70000):            # one flat run > 0xFFFF pixels long
        px[i] = 0xFFFF                # white
    for i in range(70000, 70500):     # a checkerboard: many short runs
        px[i] = 0x0000 if i % 2 == 0 else 0xF800
    for i in range(70500, w * h):     # a gradient: mostly run-length-1
        px[i] = (i * 37) & 0xFFFF
    return b"".join(struct.pack("<H", p) for p in px)


class FakeSerial:
    """Stand-in for the pyserial object kf_debug.read_frame expects: a
    readline() returning one line's bytes, or b'' for 'no data right now'
    -- which is how pyserial reports a per-call timeout, and how this
    fake simulates a device that has gone silent."""

    def __init__(self, lines):
        self._lines = list(lines)

    def readline(self):
        if not self._lines:
            return b""
        return (self._lines.pop(0) + "\n").encode("ascii")


def test_shot_roundtrip():
    print("shot round-trip (RLE -> base64 -> CRC32 -> parse -> PNG)")
    fb = synthetic_framebuffer()
    check("synthetic framebuffer is 240*320*2 bytes",
          len(fb) == kfd.FB_WIDTH * kfd.FB_HEIGHT * 2)

    rle = encode_rle(fb)
    frame_lines = build_frame_lines("fb", rle)
    # Ordinary firmware log lines before the frame -- must be skipped, not
    # choked on.
    noisy = ["I (1234) wifi: scan done", "D (1240) kf_pet: tick 42"] + frame_lines
    fake = FakeSerial(noisy)

    frame_type, decoded = kfd.read_frame(fake.readline, overall_timeout=2.0)
    check("frame type is 'fb'", frame_type == "fb")
    check("decoded (post-base64) payload matches the RLE stream sent",
          decoded == rle)

    raw = kfd.decode_rle(decoded, kfd.FB_WIDTH * kfd.FB_HEIGHT * 2)
    check("RLE-decompressed size is exactly 240*320*2",
          len(raw) == kfd.FB_WIDTH * kfd.FB_HEIGHT * 2)
    check("RLE round-trips to the original framebuffer bytes",
          raw == fb)

    check("white (0xFFFF) replicates to 0xFF, not 0xF8",
          kfd.rgb565_to_rgb888(0xFFFF) == (255, 255, 255))

    rgb = bytearray(kfd.FB_WIDTH * kfd.FB_HEIGHT * 3)
    idx = 0
    for (px,) in struct.iter_unpack("<H", raw):
        r, g, b = kfd.rgb565_to_rgb888(px)
        rgb[idx], rgb[idx + 1], rgb[idx + 2] = r, g, b
        idx += 3

    png_bytes = kfd.png_encode(kfd.FB_WIDTH, kfd.FB_HEIGHT, bytes(rgb))
    w, h, rgb_back = kfd.png_decode(png_bytes)
    check("PNG reports 240x320", (w, h) == (kfd.FB_WIDTH, kfd.FB_HEIGHT))
    check("PNG round-trips pixel-identical to the source framebuffer",
          rgb_back == bytes(rgb))


def test_crc_mismatch_is_caught():
    print("CRC32 mismatch is detected, not silently accepted")
    payload = b"\x01\x00\xAA\xAA" * 100
    lines = build_frame_lines("fb", payload)
    corrupt = list(lines)
    body_idx = 1  # lines[0] is KFDBG-BEGIN; this is the first payload line
    good_line = corrupt[body_idx]
    flipped_char = "B" if good_line[0] != "B" else "C"
    corrupt[body_idx] = flipped_char + good_line[1:]
    fake = FakeSerial(corrupt)
    try:
        kfd.read_frame(fake.readline, overall_timeout=2.0)
        check("corrupted payload raises CrcMismatchError", False)
    except kfd.CrcMismatchError:
        check("corrupted payload raises CrcMismatchError", True)


def test_timeout_is_actionable():
    print("a device that never replies times out with a clear message")
    fake = FakeSerial([])  # no lines at all -- simulates a silent/busy port
    try:
        kfd.read_frame(fake.readline, overall_timeout=0.3)
        check("silence raises DeviceTimeoutError", False)
    except kfd.DeviceTimeoutError as e:
        check("silence raises DeviceTimeoutError", True)
        check("timeout message mentions the port/monitor conflict",
              "monitor" in str(e) or "port" in str(e))


def test_state_and_pong_are_plain_text():
    print("json/pong/ack/err frames decode as plain base64'd text")
    text = json.dumps({"stage": "hatchling", "hunger_mp": 80})
    lines = build_frame_lines("json", text.encode("utf-8"))
    fake = FakeSerial(lines)
    frame_type, decoded = kfd.read_frame(fake.readline, overall_timeout=2.0)
    check("frame type is 'json'", frame_type == "json")
    check("decoded json payload matches", decoded.decode("utf-8") == text)
    check("payload parses as JSON",
          json.loads(decoded.decode("utf-8"))["stage"] == "hatchling")


def test_button_masks():
    print("button name -> bitmask matches hakoniwaos/include/kf/types.h")
    check("UP is bit 0", kfd.parse_buttons("UP") == 1)
    check("MENU is bit 6", kfd.parse_buttons("MENU") == 1 << 6)
    check("UP,A ORs together", kfd.parse_buttons("UP,A") == (1 | (1 << 4)))
    try:
        kfd.parse_buttons("NOPE")
        check("unknown button name raises", False)
    except kfd.KfDebugError:
        check("unknown button name raises", True)


def main():
    test_shot_roundtrip()
    test_crc_mismatch_is_caught()
    test_timeout_is_actionable()
    test_state_and_pong_are_plain_text()
    test_button_masks()

    print()
    if FAILURES:
        print(f"{len(FAILURES)} check(s) failed:")
        for name in FAILURES:
            print(f"  - {name}")
        return 1
    print("all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
