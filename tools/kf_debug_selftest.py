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

import argparse
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


def test_stage_parsing():
    print("stage name/number parsing matches kf_pet_stage (kf/pet.h)")
    check("'egg' -> 0", kfd.parse_stage("egg") == 0)
    check("'teen' -> 3", kfd.parse_stage("teen") == 3)
    check("'ADULT' -> 4 (case-insensitive)", kfd.parse_stage("ADULT") == 4)
    check("'2' -> 2 (raw number still accepted)", kfd.parse_stage("2") == 2)
    try:
        kfd.parse_stage("hatchling")
        check("unknown stage name raises", False)
    except kfd.KfDebugError:
        check("unknown stage name raises", True)
    try:
        kfd.parse_stage("5")
        check("out-of-range stage number raises", False)
    except kfd.KfDebugError:
        check("out-of-range stage number raises", True)


def test_care_action_aliases():
    print("care action 1-5 aliases match the desktop's number-key order "
          "(sdl_input.cpp: feed/play/rest/bath/flush)")
    check("'1' -> feed", kfd.CARE_ACTION_ALIASES["1"] == "feed")
    check("'2' -> play", kfd.CARE_ACTION_ALIASES["2"] == "play")
    check("'3' -> rest", kfd.CARE_ACTION_ALIASES["3"] == "rest")
    check("'4' -> bath", kfd.CARE_ACTION_ALIASES["4"] == "bath")
    check("'5' -> flush", kfd.CARE_ACTION_ALIASES["5"] == "flush")
    check("name aliases are identities",
          all(kfd.CARE_ACTION_ALIASES[n] == n for n in kfd.CARE_ACTIONS))


class FakeLink:
    """Stand-in for SerialLink, for testing the KFDBG command STRING each
    cmd_*() function builds -- without a real serial port. Records every
    command sent and returns one canned reply, exactly the shape _expect()
    (kf_debug.py) needs from whatever `link` it's given."""

    def __init__(self, reply_type="ack", reply_payload=b""):
        self.sent = []
        self._reply_type = reply_type
        self._reply_payload = reply_payload

    def send(self, command):
        self.sent.append(command)

    def read_frame(self, overall_timeout=None):
        return self._reply_type, self._reply_payload


def _care_args(action, variation=None):
    return argparse.Namespace(action=action, variation=variation)


def test_care_command_building():
    print("`care` builds the exact KFDBG FEED/PLAY/REST/BATH/FLUSH wire command")
    link = FakeLink()
    kfd.cmd_care(link, _care_args("1"))
    check("care 1 (default variation) -> KFDBG FEED 0",
          link.sent[-1] == "KFDBG FEED 0")

    link = FakeLink()
    kfd.cmd_care(link, _care_args("play", variation=2))
    check("care play --variation 2 -> KFDBG PLAY 2",
          link.sent[-1] == "KFDBG PLAY 2")

    link = FakeLink()
    kfd.cmd_care(link, _care_args("5"))
    check("care 5 (flush) -> KFDBG FLUSH, no variation appended",
          link.sent[-1] == "KFDBG FLUSH")

    try:
        kfd.cmd_care(FakeLink(), _care_args("5", variation=1))
        check("flush rejects an explicit --variation", False)
    except kfd.KfDebugError:
        check("flush rejects an explicit --variation", True)

    try:
        kfd.cmd_care(FakeLink(), _care_args("feed", variation=3))
        check("out-of-range variation (>= CARE_VARIATION_COUNT) raises", False)
    except kfd.KfDebugError:
        check("out-of-range variation (>= CARE_VARIATION_COUNT) raises", True)

    try:
        kfd.cmd_care(FakeLink(), _care_args("nope"))
        check("unknown care action raises", False)
    except kfd.KfDebugError:
        check("unknown care action raises", True)


def _jump_args(stage, teen_form=None, adult_branch=None):
    return argparse.Namespace(stage=stage, teen_form=teen_form,
                               adult_branch=adult_branch)


def test_jump_command_building():
    print("`jump` builds the exact KFDBG JUMP wire command, defaults included")
    link = FakeLink()
    kfd.cmd_jump(link, _jump_args("teen"))
    check("jump teen (no teen_form/adult_branch) -> KFDBG JUMP 3 0 0",
          link.sent[-1] == "KFDBG JUMP 3 0 0")

    link = FakeLink()
    kfd.cmd_jump(link, _jump_args("adult", teen_form=1, adult_branch=2))
    check("jump adult --teen-form 1 --adult-branch 2 -> KFDBG JUMP 4 1 2",
          link.sent[-1] == "KFDBG JUMP 4 1 2")

    # KF_PET_TEEN_FORM_DUST (4) is a real, reachable form -- must NOT be
    # silently clamped away by this host-side CLI, only range-checked.
    link = FakeLink()
    kfd.cmd_jump(link, _jump_args("teen", teen_form=kfd.TEEN_FORM_DUST))
    check("teen_form == TEEN_FORM_DUST (4) is accepted, not clamped",
          link.sent[-1] == "KFDBG JUMP 3 4 0")

    try:
        kfd.cmd_jump(FakeLink(), _jump_args("teen", teen_form=5))
        check("teen_form past TEEN_FORM_DUST raises", False)
    except kfd.KfDebugError:
        check("teen_form past TEEN_FORM_DUST raises", True)


def test_mutate_gate_rejection_is_actionable():
    """A device with KF_DBG_MUTATE_ENABLE=0 (ADR 0035) replies `err` to
    every mutating command instead of running it -- kf_dbg_bridge.cpp's
    require_mutate_enabled() names the exact flag in that reply. This
    proves _expect() (shared by every cmd_*() that can mutate the pet)
    turns that into a KfDebugError carrying the device's message verbatim,
    not a timeout or a bare protocol error -- same style as
    test_timeout_is_actionable() above, for the sibling failure mode."""
    print("a mutate-gated device rejects with a message naming the flag, "
          "not a timeout")
    device_message = ("mutating KFDBG commands are disabled on this build "
                       "-- set KF_DBG_MUTATE_ENABLE=1 to re-enable (PING/"
                       "SHOT/STATE/SCANLINE/VSYNC still work): KFDBG FEED 0")
    link = FakeLink(reply_type="err", reply_payload=device_message.encode("utf-8"))
    try:
        kfd.cmd_care(link, _care_args("1"))
        check("mutate-gated FEED raises KfDebugError", False)
    except kfd.KfDebugError as e:
        check("mutate-gated FEED raises KfDebugError", True)
        check("error names KF_DBG_MUTATE_ENABLE, the flag to flip",
              "KF_DBG_MUTATE_ENABLE" in str(e))
        check("error is not a bare protocol error -- it's the device's own text",
              "disabled on this build" in str(e))

    # jump goes through the identical _expect() path -- one more mutating
    # command, not a different code path, confirming the rejection is
    # generic across cmd_*() rather than something only cmd_care() gets.
    link = FakeLink(reply_type="err",
                     reply_payload=device_message.replace(
                         "KFDBG FEED 0", "KFDBG JUMP 3 0 0").encode("utf-8"))
    try:
        kfd.cmd_jump(link, _jump_args("teen"))
        check("mutate-gated JUMP raises KfDebugError", False)
    except kfd.KfDebugError as e:
        check("mutate-gated JUMP raises KfDebugError", True)
        check("JUMP's rejection also names KF_DBG_MUTATE_ENABLE",
              "KF_DBG_MUTATE_ENABLE" in str(e))


def main():
    test_shot_roundtrip()
    test_crc_mismatch_is_caught()
    test_timeout_is_actionable()
    test_state_and_pong_are_plain_text()
    test_button_masks()
    test_stage_parsing()
    test_care_action_aliases()
    test_care_command_building()
    test_jump_command_building()
    test_mutate_gate_rejection_is_actionable()

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
