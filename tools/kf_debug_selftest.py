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
import contextlib
import io
import json
import struct
import time
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

    # NOT `all(CARE_ACTION_ALIASES[n] == n for n in CARE_ACTIONS)` -- kf_
    # debug.py builds CARE_ACTION_ALIASES with
    # `.update({name: name for name in CARE_ACTIONS})`, the identical
    # comprehension, so that assertion could never fail regardless of what
    # CARE_ACTIONS even contains (verified: it stayed green when this was
    # pointed out). Two real claims instead: every name is actually
    # present as its own key (a membership check, not an echo of the
    # dict's own construction), and -- the behavioural claim that
    # actually matters -- dispatching by name and by its matching digit
    # through cmd_care() produces the IDENTICAL wire command, so "care
    # feed" and "care 1" are not just individually valid but proven
    # equivalent. test_care_command_building() below already proves each
    # form works in isolation; this proves the two forms agree.
    check("every care action name is also a key in CARE_ACTION_ALIASES",
          all(n in kfd.CARE_ACTION_ALIASES for n in kfd.CARE_ACTIONS))
    for i, name in enumerate(kfd.CARE_ACTIONS):
        digit = str(i + 1)
        kwargs = {} if name == "flush" else {"variation": 0}
        link_by_name = FakeLink()
        link_by_digit = FakeLink()
        kfd.cmd_care(link_by_name, _care_args(name, **kwargs))
        kfd.cmd_care(link_by_digit, _care_args(digit, **kwargs))
        check(f"'{digit}' and '{name}' send the identical KFDBG command",
              link_by_name.sent == link_by_digit.sent)


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


def _clock_args(target):
    return argparse.Namespace(target=target)


def test_clock_command_building():
    print("`clock` builds the exact KFDBG CLOCK wire command, name or epoch")
    for point in kfd.CLOCK_POINTS:
        link = FakeLink()
        kfd.cmd_clock(link, _clock_args(point))
        check(f"clock {point} -> KFDBG CLOCK {point.upper()}",
              link.sent[-1] == f"KFDBG CLOCK {point.upper()}")

    # Case-insensitive, matching parse_stage()'s own "name or number"
    # convention (test_stage_parsing() above).
    link = FakeLink()
    kfd.cmd_clock(link, _clock_args("DROWSY"))
    check("clock DROWSY (uppercase) -> KFDBG CLOCK DROWSY",
          link.sent[-1] == "KFDBG CLOCK DROWSY")

    link = FakeLink()
    kfd.cmd_clock(link, _clock_args("1737936000"))
    check("clock 1737936000 (explicit epoch) -> KFDBG CLOCK EPOCH 1737936000",
          link.sent[-1] == "KFDBG CLOCK EPOCH 1737936000")

    # `sync` is the named target meaning "this machine's clock, now". Two
    # things worth pinning, both of which were shipped wrong once:
    #
    #   1. It must go out in the EPOCH wire form. The panel's own Sync
    #      Clock button sent a bare `KFDBG CLOCK <number>`, which the
    #      firmware parser rejects, so the button did nothing at all.
    #   2. It must be LOCAL time, not UTC. `clock $(date +%s)` set a
    #      UTC-4 board four hours fast for exactly this reason.
    link = FakeLink()
    kfd.cmd_clock(link, _clock_args("sync"))
    sent = link.sent[-1]
    check("clock sync -> the EPOCH wire form, not a bare number",
          sent.startswith("KFDBG CLOCK EPOCH "))

    sync_epoch = int(sent.rsplit(" ", 1)[1])
    # Decoding the epoch AS IF UTC must give back this machine's LOCAL
    # civil time -- that is precisely what "the epoch IS local time" means.
    # Comparing against time.localtime() rather than a constant, because
    # the correct answer depends on where the test is being run.
    decoded = time.gmtime(sync_epoch)
    local = time.localtime()
    check("clock sync sends LOCAL time, not UTC "
          f"(sent {time.strftime('%H:%M', decoded)}, "
          f"local {time.strftime('%H:%M', local)})",
          decoded.tm_hour == local.tm_hour and decoded.tm_min == local.tm_min)

    try:
        kfd.cmd_clock(FakeLink(), _clock_args("nonsense"))
        check("clock with an unknown target raises", False)
    except kfd.KfDebugError:
        check("clock with an unknown target raises", True)


def _rtc_args(as_json=False):
    return argparse.Namespace(json=as_json)


def test_rtc_command_decode():
    print("`rtc` decodes a matching reply, an osf==1 reply, and an err "
          "when no chip answered -- KFDBG RTC needs no hardware to prove "
          "wire decoding, which is the whole point of this file")

    # A healthy chip: OSF clear, RAM clock agrees with it.
    healthy = {"present": True, "epoch": 1737936005, "osf": False,
               "wall": 1737936005, "wall_valid": True}
    link = FakeLink(reply_type="json",
                     reply_payload=json.dumps(healthy).encode("utf-8"))
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        kfd.cmd_rtc(link, _rtc_args())
    text = out.getvalue()
    check("healthy reply prints epoch", "epoch: 1737936005" in text)
    check("healthy reply prints osf: False", "osf: False" in text)
    check("healthy reply's VERDICT reports the chip and RAM clock agreeing",
          "agree" in text)

    # OSF set: the chip does not trust its own registers -- a distinct,
    # diagnosable outcome from "no chip at all" (see handle_rtc()'s own
    # comment in kf_dbg_bridge.cpp), NOT folded into a generic failure.
    osf_set = {"present": True, "epoch": 946684800, "osf": True,
               "wall": 0, "wall_valid": False}
    link = FakeLink(reply_type="json",
                     reply_payload=json.dumps(osf_set).encode("utf-8"))
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        kfd.cmd_rtc(link, _rtc_args())
    text = out.getvalue()
    check("osf==1 reply decodes osf: True", "osf: True" in text)
    check("osf==1 reply's VERDICT names OSF specifically",
          "OSF is set" in text)

    # No chip ever answered at boot: the device replies `err`, not a
    # present:false JSON -- reading the RAM clock instead would make the
    # whole command vacuous, which is exactly the trap this test guards
    # against regressing into.
    device_message = ("no DS3231 answered at boot -- nothing to read (see "
                       "esp_time.cpp's try_init_ds3231()): KFDBG RTC")
    link = FakeLink(reply_type="err",
                     reply_payload=device_message.encode("utf-8"))
    try:
        kfd.cmd_rtc(link, _rtc_args())
        check("no chip answered raises KfDebugError, not a silent "
              "present:false", False)
    except kfd.KfDebugError as e:
        check("no chip answered raises KfDebugError, not a silent "
              "present:false", True)
        check("error message names the missing chip",
              "no DS3231 answered" in str(e))

    # --json passthrough: the raw wire line, unmodified -- same contract
    # `state --json` and `scanline --json` already give.
    link = FakeLink(reply_type="json",
                     reply_payload=json.dumps(healthy).encode("utf-8"))
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        kfd.cmd_rtc(link, _rtc_args(as_json=True))
    check("--json prints the raw JSON line verbatim",
          out.getvalue().strip() == json.dumps(healthy))


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

    # clock goes through the identical _expect() path too -- CLOCK joined
    # the mutating set under ADR 0054, on the same gate as everything else
    # in this test, not a special case.
    link = FakeLink(reply_type="err",
                     reply_payload=device_message.replace(
                         "KFDBG FEED 0", "KFDBG CLOCK DROWSY").encode("utf-8"))
    try:
        kfd.cmd_clock(link, _clock_args("drowsy"))
        check("mutate-gated CLOCK raises KfDebugError", False)
    except kfd.KfDebugError as e:
        check("mutate-gated CLOCK raises KfDebugError", True)
        check("CLOCK's rejection also names KF_DBG_MUTATE_ENABLE",
              "KF_DBG_MUTATE_ENABLE" in str(e))

    # RTC, by contrast, is observe tier (ADR 0054) -- it must NOT go
    # through this rejection path at all. Nothing to assert here beyond
    # what test_rtc_command_decode() above already proves (a `json` reply
    # decodes normally); this comment exists so a future reader doesn't
    # wonder why RTC has no rejection case in this function -- it's
    # deliberate, not an oversight.


# The full ADR 0036 key set a real STATE reply carries, on top of the
# pre-existing fields test_state_and_pong_are_plain_text() above already
# covers -- used by both tests below, once with every value present and
# once with all of it stripped out to simulate older firmware.
_BUDGET_KEYS = {
    "draw_us": 115, "transfer_us": 923, "cpu_us": 1038, "post_us": 42,
    "dirty_rects": 1, "dirty_pct": 3, "opaque_px": 2304, "keyed_px": 2304,
    "over_budget": False, "worst_us": 1200, "p99_us": 1100,
    "frames": 300, "over_budget_frames": 0,
}


def _state_args():
    return argparse.Namespace(json=False)


def test_state_budget_line_with_every_key_present():
    print("`state`'s human output gains a budget line carrying ADR 0036's "
          "new fields")
    obj = {"stage": 2, "hunger_mp": 50000, **_BUDGET_KEYS}
    link = FakeLink(reply_type="json",
                     reply_payload=json.dumps(obj).encode("utf-8"))
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        kfd.cmd_state(link, _state_args())
    text = out.getvalue()
    check("per-key dump still prints the pre-existing fields",
          "stage: 2" in text)
    check("a budget line is printed",
          "budget:" in text)

    # Against _format_budget_line()'s OWN return value, not the whole
    # printed dump above: _print_state_line() already prints "key: value"
    # for every field in `obj` before the budget line ever runs, so
    # `str(value) in text` against the full text would pass even if
    # _format_budget_line() dropped every field it claims to summarise --
    # it did, once, down to three, and this assertion did not notice (see
    # the audit that found it). Each fragment matches the formatter's own
    # "label=value" shape exactly, so a dropped or mislabelled field fails
    # here specifically.
    budget_line = kfd._format_budget_line(obj)
    px = f"{_BUDGET_KEYS['opaque_px']}+{_BUDGET_KEYS['keyed_px']}"
    expected_fragments = {
        "draw_us": f"draw={_BUDGET_KEYS['draw_us']}us",
        "transfer_us": f"xfer={_BUDGET_KEYS['transfer_us']}us",
        "cpu_us": f"cpu={_BUDGET_KEYS['cpu_us']}us",
        "post_us": f"post={_BUDGET_KEYS['post_us']}us",
        "dirty_rects": f"rects={_BUDGET_KEYS['dirty_rects']}",
        "dirty_pct": f"({_BUDGET_KEYS['dirty_pct']}%)",
        "opaque_px": f"px={px}",
        "keyed_px": f"px={px}",
        "over_budget": f"over_budget={_BUDGET_KEYS['over_budget']}",
        "worst_us": f"worst={_BUDGET_KEYS['worst_us']}us",
        "p99_us": f"p99={_BUDGET_KEYS['p99_us']}us",
        "frames": f"frames={_BUDGET_KEYS['frames']}",
        "over_budget_frames": f"over={_BUDGET_KEYS['over_budget_frames']}",
    }
    assert expected_fragments.keys() == _BUDGET_KEYS.keys(), (
        "expected_fragments must cover exactly _BUDGET_KEYS -- update both "
        "together")
    for key, fragment in expected_fragments.items():
        check(f"budget line carries {key} as '{fragment}'",
              fragment in budget_line)


def test_state_budget_line_missing_keys_does_not_crash():
    print("`state` against firmware that predates ADR 0036 prints '?' "
          "instead of raising KeyError")
    obj = {"stage": 0, "hunger_mp": 100000}  # none of _BUDGET_KEYS present
    link = FakeLink(reply_type="json",
                     reply_payload=json.dumps(obj).encode("utf-8"))
    out = io.StringIO()
    try:
        with contextlib.redirect_stdout(out):
            kfd.cmd_state(link, _state_args())
        check("cmd_state does not raise when budget keys are absent", True)
    except KeyError:
        check("cmd_state does not raise when budget keys are absent", False)
        return
    text = out.getvalue()
    check("missing budget fields print '?' rather than being omitted",
          "draw=?us" in text and "cpu=?us" in text and "post=?us" in text)


def test_watch_summary_formatter():
    print("_format_watch_summary() carries fps/cpu_us/post_us/rects, and "
          "does not raise when they are absent")
    full = {"fps": "30.0", "cpu_us": 1038, "post_us": 42, "dirty_rects": 1}
    line = kfd._format_watch_summary(full)
    check("full object formats every field",
          line == "fps=30.0 cpu_us=1038 post_us=42 rects=1")

    empty = {"stage": 0}  # simulates firmware older than ADR 0036
    try:
        line = kfd._format_watch_summary(empty)
        check("missing keys do not raise", True)
    except KeyError:
        check("missing keys do not raise", False)
        return
    check("missing keys format as '?'",
          line == "fps=? cpu_us=? post_us=? rects=?")


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
    test_clock_command_building()
    test_rtc_command_decode()
    test_mutate_gate_rejection_is_actionable()
    test_state_budget_line_with_every_key_present()
    test_state_budget_line_missing_keys_does_not_crash()
    test_watch_summary_formatter()

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
