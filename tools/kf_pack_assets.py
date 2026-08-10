#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright the Kamiframe contributors.
"""Pack named sprites into a Kamiframe asset pack (.kfpack), stdlib only.

Supersedes tools/make_test_sprite.py: there is now an asset pipeline (see
docs/architecture/adr-0033-asset-pipeline.md), so the slice-one test blob is
generated straight into the real pack format below instead of a baked-in C
header. No Pillow: PNG decoding, when a source image is used instead of a
procedurally-generated sprite, goes through zlib + struct only, the same way
tools/kf_debug.py's png_decode() already does -- see decode_png() below,
which is a closer cousin of that function than a new invention.

------------------------------------------------------------------------
THE PACK FORMAT (".kfpack"), all integers little-endian
------------------------------------------------------------------------

This is deliberately simple: a fixed header, a flat directory of
fixed-size entries, then raw payload data. No compression, no nesting, no
per-platform variants. Reading it needs nothing but "seek and read a
struct" -- see hakoniwaos/src/assets.cpp, which parses exactly this and
is the second (and only other) place this layout is written down.

Header, 16 bytes:

    offset  size  field
    0       4     magic, ASCII "KFAP" (Kamiframe Asset Pack)
    4       2     format_version (uint16), currently 1
    6       2     entry_count (uint16)
    8       4     directory_offset (uint32), bytes from the start of the
                  file to the first directory entry -- always 16 today
                  (right after this header), stored explicitly rather than
                  assumed so a future header field can be added without
                  breaking readers that already check this.
    12      4     reserved (uint32), always 0

Directory: entry_count entries, DIRECTORY_ENTRY_BYTES (52) bytes each,
back to back starting at directory_offset:

    offset  size  field
    0       32    name, ASCII, NUL-padded (NOT necessarily NUL-terminated
                   if all 32 bytes are used -- readers must treat byte 31
                   as the practical end regardless). 31 usable characters.
    32      1     asset_type (uint8) -- ASSET_TYPE_* below. Decides how to
                   read type_meta at offset 36.
    33      1     reserved (uint8), 0
    34      2     reserved (uint16), 0 -- padding, so type_meta below starts
                   4-byte aligned within the entry
    36      8     type_meta -- TYPE_META_BYTES raw bytes, meaning depends
                   entirely on asset_type. See "Asset types" below for what
                   each defined type puts here. A type this reader does not
                   recognise is legal: its type_meta is simply opaque to
                   this reader, which does not need to understand it to
                   validate or skip the entry -- see "More than one asset
                   type" below.
    44      4     data_offset (uint32), bytes from the START OF THE FILE
                   (not from the directory or from this entry) to this
                   entry's raw payload. Always a multiple of 4 -- see
                   "Alignment" below.
    48      4     data_bytes (uint32), the payload's length in bytes. For
                   ASSET_TYPE_SPRITE this is width*height*2; for
                   ASSET_TYPE_SPRITE_INDEXED it is
                   palette_bytes_padded + frame_count*width*height (see
                   "Asset types" below for both) -- stored explicitly rather
                   than only recomputed by the reader, so a corrupt
                   width/height/frame_count is caught by a mismatch check
                   instead of silently reading the wrong number of bytes.

Payload data: for each entry, data_bytes raw bytes starting at that
entry's data_offset, meaning depends on asset_type (for a sprite: RGB565
pixels, 2 bytes each, row-major, row 0 first, left to right). Packed back
to back in directory order with no gap other than the alignment padding
below.

------------------------------------------------------------------------
MORE THAN ONE ASSET TYPE, ON PURPOSE
------------------------------------------------------------------------

Sprites are the only type this packer actually builds today, but sound
effects are coming later -- the board's planned MAX98357A I2S amplifier
plays arbitrary PCM -- and they need exactly the same treatment sprites
get: packed into flash, named, memory-mapped, loaded identically on both
backends. Rather than build a second pack format and a second loader for
that later, every directory entry already carries an asset_type tag and an
8-byte type_meta block whose interpretation depends on it, so a new type
is a new value in this table plus a new decoder, not a new file format:

    ASSET_TYPE_SPRITE (0): raw RGB565 pixels, one frame, the format every
        pack used before ASSET_TYPE_SPRITE_INDEXED existed and still the
        right choice for a large opaque sprite (an un-keyed row is a
        memcpy; an indexed row can never be one).
        type_meta: width (u16 @0), height (u16 @2), color_key (u16 @4),
        has_color_key (u8 @6), reserved (u8 @7).

    ASSET_TYPE_AUDIO_CLIP (1): RESERVED. No sound is packed by this tool
        yet, and hakoniwaos/src/assets.cpp has no decoder for it -- this
        value exists purely so the directory format does not need to
        change shape when one is added. Intended shape, so the packer and
        the eventual loader agree on it in advance: 16-bit signed PCM,
        mono, a modest sample rate (around 22kHz -- enough for short sound
        effects, not music), uncompressed. Uncompressed on purpose:
        decoding MP3 or similar per playback costs CPU and RAM a one-shot
        effect clip does not justify. Planned type_meta, NOT yet read or
        written by any code here: sample_rate (u32 @0), channels (u8 @4,
        always 1 for now), bits_per_sample (u8 @5, always 16 for now),
        reserved (u16 @6).

    ASSET_TYPE_SPRITE_INDEXED (2): 8bpp palette-indexed pixels, one or
        more frames.
        type_meta: width (u16 @0), height (u16 @2), frame_count (u16 @4),
        palette_count_minus_1 (u8 @6 -- the palette holds this + 1 entries,
        so 1..256 are expressible in one byte), flags (u8 @7; bit 0 =
        has_color_key, bits 1-7 reserved and 0).

        Payload, in this order:
          - palette: (palette_count_minus_1 + 1) * 2 bytes, RGB565
            little-endian, zero-padded up to a multiple of 4 so the index
            data that follows starts on a 4-byte boundary the same way
            data_offset itself does;
          - indices: frame_count * width * height bytes, one byte per
            pixel, row-major, frame 0 first. FRAME k STARTS AT
            palette_bytes_padded + k * width * height -- contiguous on
            purpose, so a player addresses a frame with a multiply instead
            of a directory lookup.
        data_bytes therefore equals palette_bytes_padded +
        frame_count * width * height, and is checked against exactly that.

        When flags bit 0 is set, PALETTE ENTRY 0 IS THE COLOUR KEY and
        index 0 is never drawn. Fixed by convention rather than stored,
        so the blitter compares against a compile-time constant; the
        producer is what guarantees it. As of this task that means
        quantize_rgb565() (below), which always seeds the palette with the
        key colour first, plus make_indexed_asset()'s own check that
        palette[0] really is the caller's declared color_key.
        tools/kf_ingest_sprites.py routes through both when it builds a
        pack entry (its build_entry()), so a real creature pack gets the
        same guarantee.

        NO FORMAT VERSION BUMP. This is a new asset_type in the existing
        52-byte directory entry, which is precisely the extension path
        adr-0033-asset-pipeline.md's "Cost to change" section describes.
        A reader that does not know this type still walks the directory
        correctly and simply builds no view for the entry.

kf/assets.h's own header comment describes the C++ side of this: the
directory WALK (bounds-checking name/data_offset/data_bytes) is the same
for every entry regardless of type, and only the final, type-specific
decode step branches on asset_type -- so a future kf_assets_get_clip() is
an addition to that file, not a rewrite of it.

------------------------------------------------------------------------
BYTE ORDER -- read this before touching payload data anywhere in this file
------------------------------------------------------------------------

Multi-byte payload values (today, RGB565 pixels; later, PCM samples) are
stored little-endian, which is the NATIVE order of both kf_color (uint16_t,
hakoniwaos/include/kf/types.h) and a 16-bit PCM sample on every target this
project actually builds for: the ESP32-S3 is little-endian, and so is
every desktop this simulator runs on (x86_64, ARM64). This is deliberately
NOT the same thing as a PANEL's byte order -- see
ports/esp32/hal/kf_panel_profile.h's big_endian_fb comment, which explains
why one supported display module needs its pixels byte-swapped on the way
to the glass and the other does not. That swap happens in the display
driver, at present time, right before the bytes hit the wire; it must
NEVER happen here. A pack file swapped for one panel would be silently
wrong on the other, and wrong in a way nothing before hardware bring-up
would catch. If this project ever targets a big-endian host or device,
this format needs an explicit endianness flag and a version bump -- it
does not have one today because doing so speculatively would just be
dead code with no way to test it.

------------------------------------------------------------------------
ALIGNMENT
------------------------------------------------------------------------

Every data_offset is a multiple of 4. Readers on both backends cast
`base + data_offset` straight to a typed pointer (today, `const
kf_color*`; later, presumably `const int16_t*` for PCM) with no copy --
see kf_hal_assets_base() in kf/hal/assets.h and how
hakoniwaos/src/assets.cpp uses it -- so an unaligned offset would be
undefined behaviour in C++ and, on Xtensa, can also just be slower or
outright unsupported for some instruction forms. This packer pads each
entry's payload block up to the next 4-byte boundary before starting the
next one, whether or not the natural size already lands on one.

------------------------------------------------------------------------
LOOKUP: why a name string, not a hash
------------------------------------------------------------------------

kf_assets_get() in hakoniwaos/src/assets.cpp does a plain linear scan,
comparing the caller's name against every entry with strcmp. That is the
right trade at this scale: real packs here are tens of entries, not
thousands, and are looked up once at load time (see kf/demo.cpp's use --
one kf_assets_get() call per sprite, cached, never per frame), so an O(n)
scan costs nothing that matters. The alternative -- pre-hashing names into
a table this packer would compute and hakoniwaos/src/assets.cpp would
re-derive -- buys nothing at this scale and adds a real ongoing cost: the
Python hash and the C++ hash have to agree bit-for-bit forever, which is
exactly the kind of cross-language contract this codebase avoids where it
can (see tools/kf_debug.py's BUTTON_BITS comment for the same call made
the same way, once, already).

------------------------------------------------------------------------
Usage
------------------------------------------------------------------------

    python3 tools/kf_pack_assets.py --test-sprite -o examples/hello_sprite/assets.kfpack
    python3 tools/kf_pack_assets.py --png name=path/to/sprite.png:RRGGBB -o out.kfpack
    python3 tools/kf_pack_assets.py --test-sprite --png hero=hero.png -o out.kfpack

--test-sprite adds one procedurally-generated sprite named "test_sprite" --
the same 32x32 blob tools/make_test_sprite.py used to draw, ported here
pixel-for-pixel so nothing on screen changes shape, only where it comes
from. --png NAME=PATH[:RRGGBB] adds a sprite decoded from a PNG file, with
an optional hex colour key (e.g. FF00FF for magenta); PNG decoding is
narrow -- 8-bit RGB or RGBA truecolour, the same restricted case
tools/kf_debug.py's png_decode() already handles, extended here to cover
every PNG filter type rather than just type 0 -- because that is what this
project's own tooling produces and nothing here needs to read an arbitrary
PNG off the internet.
"""

import argparse
import struct
import sys
import zlib

MAGIC = b"KFAP"
FORMAT_VERSION = 1
HEADER_BYTES = 16
DIRECTORY_ENTRY_BYTES = 52
NAME_BYTES = 32
TYPE_META_BYTES = 8
ALIGN = 4

# Matches kf_asset_type in hakoniwaos/include/kf/assets.h exactly -- see
# that header's own comment for why AUDIO_CLIP is defined but not yet
# produced by anything in this file.
ASSET_TYPE_SPRITE = 0
ASSET_TYPE_AUDIO_CLIP = 1  # reserved, see the format comment above
ASSET_TYPE_SPRITE_INDEXED = 2

TEST_SPRITE_W = 32
TEST_SPRITE_H = 32
TEST_SPRITE_COLOR_KEY = (255, 0, 255)  # magenta, never drawn


def _sprite_type_meta(width: int, height: int, color_key) -> bytes:
    """Packs ASSET_TYPE_SPRITE's 8-byte type_meta block: width, height,
    color_key, has_color_key, reserved."""
    has_key = 1 if color_key is not None else 0
    meta = struct.pack("<HHHBB", width, height, color_key or 0, has_key, 0)
    assert len(meta) == TYPE_META_BYTES
    return meta


def _indexed_sprite_type_meta(width: int, height: int, frame_count: int,
                               palette_count: int, has_color_key: bool) -> bytes:
    """Packs ASSET_TYPE_SPRITE_INDEXED's 8-byte type_meta block. See the
    module docstring's "Asset types" section for the field layout."""
    if not 1 <= palette_count <= 256:
        raise ValueError(f"palette_count {palette_count} outside 1..256")
    if not 1 <= frame_count <= 0xFFFF:
        raise ValueError(f"frame_count {frame_count} outside 1..65535")
    meta = struct.pack("<HHHBB", width, height, frame_count,
                        palette_count - 1, 1 if has_color_key else 0)
    assert len(meta) == TYPE_META_BYTES
    return meta


def make_indexed_asset(name, width, height, frames, palette, has_color_key,
                        color_key=None):
    """`frames` is a list of bytes objects, each width*height index bytes,
    frame 0 first. `palette` is a list of RGB565 ints, entry 0 being the
    colour key when has_color_key. Returns one packable entry dict, the same
    shape make_test_sprite() returns.

    When has_color_key is True, the caller must also pass `color_key` (the
    RGB565 int that pixels equal to should not draw) -- this function is what
    the format comment's "the producer guarantees palette[0] is the colour
    key" claim actually cashes out to, so it checks it itself instead of
    trusting that every caller happened to route through quantize_rgb565(),
    which is the only thing that currently makes it true."""
    if has_color_key:
        if color_key is None:
            raise ValueError(
                f"'{name}': has_color_key=True requires color_key so this "
                "function can verify palette[0] is really it")
        if not palette or palette[0] != color_key:
            raise ValueError(
                f"'{name}': has_color_key=True but palette[0] "
                f"({palette[0] if palette else 'EMPTY'!r}) != color_key "
                f"({color_key!r}) -- KF_SPRITE_KEY_INDEX (kf/types.h) is "
                "fixed at slot 0, so the packer refuses to write a sprite "
                "whose key colour is anywhere else")
    for i, f in enumerate(frames):
        if len(f) != width * height:
            raise ValueError(f"'{name}' frame {i}: {len(f)} bytes, expected "
                              f"{width * height}")
        hi = max(f) if f else 0
        if hi >= len(palette):
            raise ValueError(
                f"'{name}' frame {i}: index {hi} is past the end of a "
                f"{len(palette)}-entry palette -- the packer will not write a "
                "sprite whose own indices read off the end of its palette")
    pal = b"".join(struct.pack("<H", c) for c in palette)
    pal += b"\x00" * ((-len(pal)) % ALIGN)
    return {
        "name": name,
        "asset_type": ASSET_TYPE_SPRITE_INDEXED,
        "type_meta": _indexed_sprite_type_meta(width, height, len(frames),
                                                len(palette), has_color_key),
        "data": pal + b"".join(frames),
    }


def quantize_rgb565(frames_565, key565):
    """Turns a list of RGB565 pixel lists into (palette, index_frames).
    The colour key, if present anywhere, is forced to palette slot 0 --
    KF_SPRITE_KEY_INDEX in hakoniwaos/include/kf/types.h. Every other colour
    follows in first-seen order, which is stable for a given input and so
    keeps a regenerated pack byte-identical. Raises if the union exceeds 256:
    that is the point at which 8bpp stops being lossless, and silently
    quantising would be exactly the quality loss this format was chosen to
    avoid."""
    palette = [key565]
    index_of = {key565: 0}
    out = []
    for pixels in frames_565:
        buf = bytearray(len(pixels))
        for i, c in enumerate(pixels):
            slot = index_of.get(c)
            if slot is None:
                if len(palette) == 256:
                    raise ValueError(
                        "more than 256 distinct colours -- 8bpp indexing is "
                        "lossless only up to 256; split this entry or move to "
                        "a wider format deliberately")
                slot = len(palette)
                index_of[c] = slot
                palette.append(c)
            buf[i] = slot
        out.append(bytes(buf))
    return palette, out


def rgb565(r: int, g: int, b: int) -> int:
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | ((b & 0xF8) >> 3)


def _test_sprite_pixel(x: int, y: int):
    """Pixel-for-pixel identical to tools/make_test_sprite.py's blob (now
    retired): a squat ellipse body, an outline ring, two eyes, two cheeks,
    and top-left shading. Not a Kamiframe creature -- see that file's own
    former docstring, carried forward here -- the creature class name is
    deliberately undecided and any real creature's art is separately
    copyrighted."""
    w, h = TEST_SPRITE_W, TEST_SPRITE_H
    cx, cy = (w - 1) / 2.0, (h - 1) / 2.0 + 2.0
    dx, dy = x - cx, y - cy

    body = (dx * dx) / (13.5 * 13.5) + (dy * dy) / (12.0 * 12.0)
    if body > 1.0:
        return TEST_SPRITE_COLOR_KEY

    if body > 0.82:
        return (40, 46, 72)

    for ex in (-5.0, 5.0):
        ed = (x - (cx + ex)) ** 2 / 6.0 + (y - (cy - 3.0)) ** 2 / 8.0
        if ed <= 1.0:
            return (28, 32, 54)
        if ed <= 2.2:
            return (250, 250, 252)

    for ex in (-8.0, 8.0):
        cd = (x - (cx + ex)) ** 2 / 5.0 + (y - (cy + 2.5)) ** 2 / 3.0
        if cd <= 1.0:
            return (240, 150, 160)

    shade = 1.0 - (body * 0.30) - (dy / float(h)) * 0.25
    shade = max(0.55, min(1.0, shade))
    base = (150, 210, 190)
    return tuple(int(c * shade) for c in base)


def _test_sprite_pixels_565():
    """Row-major RGB565 ints for the test sprite blob -- one generator
    shared by make_test_sprite() (packed as raw RGB565 bytes) and the
    --indexed path (quantized into a palette + index bytes by
    quantize_rgb565()), so both start from identical pixel data and an
    --indexed pack's test_sprite is provably the same image."""
    pixels = []
    for y in range(TEST_SPRITE_H):
        for x in range(TEST_SPRITE_W):
            pixels.append(rgb565(*_test_sprite_pixel(x, y)))
    return pixels


def make_test_sprite():
    """Returns one packable entry dict for the procedural test sprite --
    see pack()'s own docstring for the dict shape every asset type (sprite
    today, audio later) is expected to produce."""
    pixels = b"".join(struct.pack("<H", c) for c in _test_sprite_pixels_565())
    key = rgb565(*TEST_SPRITE_COLOR_KEY)
    return {
        "name": "test_sprite",
        "asset_type": ASSET_TYPE_SPRITE,
        "type_meta": _sprite_type_meta(TEST_SPRITE_W, TEST_SPRITE_H, key),
        "data": pixels,
    }


def make_indexed_test_sprite():
    """Returns 'test_sprite' as ASSET_TYPE_SPRITE_INDEXED instead of raw
    RGB565 -- the same pixel data make_test_sprite() packs, run through
    quantize_rgb565(), so --indexed's output is provably the same image,
    losslessly re-encoded, not a different sprite."""
    key = rgb565(*TEST_SPRITE_COLOR_KEY)
    palette, frames = quantize_rgb565([_test_sprite_pixels_565()], key)
    return make_indexed_asset("test_sprite", TEST_SPRITE_W, TEST_SPRITE_H,
                               frames, palette, has_color_key=True,
                               color_key=key)


def _shift_pixels_565(pixels565, width, height, shift):
    """Rotates each row `shift` columns to the right (wrapping), so frame
    `shift` of the animation fixture is visibly different from frame 0
    without inventing any new colours -- the point of this fixture is
    proving frame addressing, not exercising a bigger palette."""
    out = list(pixels565)
    s = shift % width
    if s == 0:
        return out
    for y in range(height):
        row = pixels565[y * width:(y + 1) * width]
        out[y * width:(y + 1) * width] = row[-s:] + row[:-s]
    return out


def make_test_sprite_anim(frame_count):
    """Returns 'test_sprite_anim' as an N-frame ASSET_TYPE_SPRITE_INDEXED
    fixture: the test sprite blob, shifted one pixel right per frame, so a
    reader can tell frame 0 from frame 1 (and prove frame k really starts
    at k*width*height) without a second hand-drawn image."""
    base = _test_sprite_pixels_565()
    key = rgb565(*TEST_SPRITE_COLOR_KEY)
    frames565 = [_shift_pixels_565(base, TEST_SPRITE_W, TEST_SPRITE_H, k)
                 for k in range(frame_count)]
    palette, frames = quantize_rgb565(frames565, key)
    return make_indexed_asset("test_sprite_anim", TEST_SPRITE_W,
                               TEST_SPRITE_H, frames, palette,
                               has_color_key=True, color_key=key)


# --------------------------------------------------------------------------
# Minimal PNG decoder: 8-bit truecolour (RGB or RGBA). A cousin of
# tools/kf_debug.py's png_decode(), extended to accept an alpha channel
# (dropped -- this format has no alpha, only a colour key) and every PNG
# filter type, since art sources are more likely to need both than a device
# screenshot (written by this codebase's own png_encode(), which never
# filters and never carries alpha) is.
# --------------------------------------------------------------------------

_PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def decode_png(data: bytes):
    """Returns (width, height, rgb_bytes) -- 3 bytes/pixel, row-major."""
    if data[:8] != _PNG_SIGNATURE:
        raise ValueError("not a PNG (bad signature)")
    pos = 8
    width = height = bit_depth = color_type = None
    idat = bytearray()
    while pos < len(data):
        (length,) = struct.unpack_from(">I", data, pos)
        tag = data[pos + 4:pos + 8]
        chunk = data[pos + 8:pos + 8 + length]
        pos += 8 + length + 4  # + CRC, unchecked
        if tag == b"IHDR":
            width, height, bit_depth, color_type = struct.unpack_from(
                ">IIBB", chunk, 0)
        elif tag == b"IDAT":
            idat += chunk
        elif tag == b"IEND":
            break
    if width is None:
        raise ValueError("PNG has no IHDR")
    if bit_depth != 8 or color_type not in (2, 6):
        raise ValueError(
            f"unsupported PNG: bit_depth={bit_depth} color_type={color_type} "
            "-- only 8-bit RGB (2) or RGBA (6) truecolour is supported")
    channels = 3 if color_type == 2 else 4
    stride = width * channels
    raw = zlib.decompress(bytes(idat))

    rgb = bytearray(width * height * 3)
    prev_row = bytearray(stride)
    p = 0
    for y in range(height):
        filter_type = raw[p]
        p += 1
        row = bytearray(raw[p:p + stride])
        p += stride
        _unfilter_row(filter_type, row, prev_row, channels)
        for x in range(width):
            o = x * channels
            d = (y * width + x) * 3
            rgb[d:d + 3] = row[o:o + 3]
        prev_row = row
    return width, height, bytes(rgb)


def _paeth(a: int, b: int, c: int) -> int:
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


def _unfilter_row(filter_type: int, row: bytearray, prev_row: bytearray,
                   channels: int) -> None:
    """In place, per the PNG spec's five filter types."""
    if filter_type == 0:
        return
    n = len(row)
    for i in range(n):
        a = row[i - channels] if i >= channels else 0
        b = prev_row[i]
        c = prev_row[i - channels] if i >= channels else 0
        if filter_type == 1:
            row[i] = (row[i] + a) & 0xFF
        elif filter_type == 2:
            row[i] = (row[i] + b) & 0xFF
        elif filter_type == 3:
            row[i] = (row[i] + ((a + b) // 2)) & 0xFF
        elif filter_type == 4:
            row[i] = (row[i] + _paeth(a, b, c)) & 0xFF
        else:
            raise ValueError(f"unsupported PNG filter type {filter_type}")


def make_png_sprite(name, path, color_key_hex):
    """Returns one packable entry dict, same shape as make_test_sprite()."""
    with open(path, "rb") as f:
        width, height, rgb = decode_png(f.read())
    pixels = bytearray()
    for i in range(0, len(rgb), 3):
        pixels += struct.pack("<H", rgb565(rgb[i], rgb[i + 1], rgb[i + 2]))
    key = None
    if color_key_hex:
        v = int(color_key_hex, 16)
        key = rgb565((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF)
    return {
        "name": name,
        "asset_type": ASSET_TYPE_SPRITE,
        "type_meta": _sprite_type_meta(width, height, key),
        "data": bytes(pixels),
    }


# --------------------------------------------------------------------------
# Packing
# --------------------------------------------------------------------------

def pack(assets):
    """`assets` is a list of dicts, each shaped like make_test_sprite()'s
    return value: {"name": str, "asset_type": int, "type_meta": 8 bytes,
    "data": bytes}. Type-agnostic on purpose -- see this file's own "More
    than one asset type" comment -- so a future audio-clip entry is just
    another dict with ASSET_TYPE_AUDIO_CLIP and its own type_meta, packed
    by this exact same function. Returns the complete .kfpack file as
    bytes."""
    if len(assets) > 0xFFFF:
        raise ValueError("too many entries for a uint16 entry_count")

    directory_offset = HEADER_BYTES
    directory_bytes = len(assets) * DIRECTORY_ENTRY_BYTES
    data_area_start = directory_offset + directory_bytes

    entries = []
    data_blocks = []
    offset = data_area_start
    for asset in assets:
        name = asset["name"]
        type_meta = asset["type_meta"]
        data = asset["data"]

        name_bytes = name.encode("ascii")
        if len(name_bytes) > NAME_BYTES - 1:
            raise ValueError(
                f"asset name '{name}' is {len(name_bytes)} bytes, longer "
                f"than the {NAME_BYTES - 1}-character limit (32-byte field, "
                "NUL included)")
        if len(type_meta) != TYPE_META_BYTES:
            raise ValueError(
                f"asset '{name}': type_meta is {len(type_meta)} bytes, "
                f"expected exactly {TYPE_META_BYTES}")

        entries.append((name_bytes, asset["asset_type"], type_meta, offset,
                         len(data)))
        data_blocks.append(data)

        offset += len(data)
        pad = (-offset) % ALIGN
        if pad:
            data_blocks.append(b"\x00" * pad)
            offset += pad

    out = bytearray()
    out += MAGIC
    out += struct.pack("<HHI", FORMAT_VERSION, len(assets), directory_offset)
    out += struct.pack("<I", 0)  # reserved
    assert len(out) == HEADER_BYTES

    for name_bytes, asset_type, type_meta, data_offset, data_len in entries:
        out += name_bytes + b"\x00" * (NAME_BYTES - len(name_bytes))
        out += struct.pack("<B", asset_type)
        out += struct.pack("<B", 0)   # reserved
        out += struct.pack("<H", 0)   # reserved
        out += type_meta
        out += struct.pack("<II", data_offset, data_len)
    assert len(out) == data_area_start

    for block in data_blocks:
        out += block

    return bytes(out)


def _describe(asset: dict) -> str:
    """One summary line for the --out report below. Branches on
    asset_type, same as hakoniwaos/src/assets.cpp's own decode step does --
    today that means ASSET_TYPE_SPRITE or ASSET_TYPE_SPRITE_INDEXED, but a
    future audio entry should describe itself here too rather than fall
    through to nothing."""
    if asset["asset_type"] == ASSET_TYPE_SPRITE:
        width, height, color_key, has_key, _ = struct.unpack(
            "<HHHBB", asset["type_meta"])
        key_desc = f"key=0x{color_key:04X}" if has_key else "no color key"
        return f"{asset['name']}: sprite {width}x{height} {key_desc}"
    if asset["asset_type"] == ASSET_TYPE_SPRITE_INDEXED:
        width, height, frame_count, palette_count_minus_1, flags = \
            struct.unpack("<HHHBB", asset["type_meta"])
        key_desc = "key=palette[0]" if flags & 0x01 else "no color key"
        return (f"{asset['name']}: indexed sprite {width}x{height} "
                f"{frame_count} frame(s) {palette_count_minus_1 + 1}-colour "
                f"palette {key_desc}")
    return f"{asset['name']}: asset_type={asset['asset_type']} " \
           f"({len(asset['data'])} bytes)"


def main(argv=None) -> int:
    p = argparse.ArgumentParser(
        description="Pack named sprites (and, later, audio clips) into a "
                     "Kamiframe .kfpack file.")
    p.add_argument("--test-sprite", action="store_true",
                    help="include the procedurally-generated test sprite, "
                         "named 'test_sprite'")
    p.add_argument("--png", action="append", default=[], metavar="NAME=PATH[:RRGGBB]",
                    help="include a sprite decoded from a PNG file, with an "
                         "optional hex colour key; may be given more than once")
    p.add_argument("--indexed", action="store_true",
                    help="emit --test-sprite as ASSET_TYPE_SPRITE_INDEXED "
                         "instead of raw RGB565")
    p.add_argument("--frames", type=int, default=1,
                    help="with --indexed and a value greater than 1, also "
                         "emit 'test_sprite_anim' with this many frames "
                         "(the blob shifted one pixel right per frame), as "
                         "an animation fixture")
    p.add_argument("-o", "--out", required=True, help="output .kfpack path")
    args = p.parse_args(argv)

    assets = []
    if args.test_sprite:
        assets.append(make_indexed_test_sprite() if args.indexed
                      else make_test_sprite())
        if args.indexed and args.frames > 1:
            assets.append(make_test_sprite_anim(args.frames))

    for spec in args.png:
        if "=" not in spec:
            p.error(f"--png expects NAME=PATH[:RRGGBB], got '{spec}'")
        name, rest = spec.split("=", 1)
        path, _, key = rest.partition(":")
        assets.append(make_png_sprite(name, path, key or None))

    if not assets:
        p.error("nothing to pack: pass --test-sprite and/or --png at least once")

    names = [a["name"] for a in assets]
    if len(set(names)) != len(names):
        p.error(f"duplicate asset name(s) in: {names}")

    data = pack(assets)
    with open(args.out, "wb") as f:
        f.write(data)

    print(f"wrote {args.out}: {len(assets)} entr{'y' if len(assets) == 1 else 'ies'}, "
          f"{len(data)} bytes", file=sys.stderr)
    for asset in assets:
        print(f"  {_describe(asset)}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
