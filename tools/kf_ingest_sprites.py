#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright the Kamiframe contributors.
"""Validate a directory of generated PNGs against character_manifest.toml
and pack the ones that pass into a .kfpack, stdlib only.

For each sprite the manifest says should exist, this checks:
  - the file exists (by the shared naming convention, see
    kf_character_manifest.py's module docstring);
  - it decodes as an 8-bit RGB or RGBA PNG (a cousin of
    tools/kf_pack_assets.py's own decode_png(), extended here to keep the
    alpha channel -- see decode_png_rgba() below for why that can't just
    reuse the packer's decoder unchanged);
  - its dimensions match the manifest entry's width/height exactly;
  - it actually has transparency: an RGB (no alpha) PNG is rejected
    outright, and an RGBA PNG where every pixel is fully opaque is flagged
    too, since that almost always means the background was never removed.

PACKING ITSELF IS NOT REIMPLEMENTED HERE. Every sprite that passes
validation is handed to tools/kf_pack_assets.py's own pack() -- imported,
not copied -- so there is exactly one place in the repo that knows the
.kfpack byte layout (see that file's own docstring, and ADR 0033). This
file's only original contribution is turning a transparent PNG into the
{"name", "asset_type", "type_meta", "data"} shape pack() expects: replacing
transparent pixels with an explicit RGB565 color key (kf_pack_assets.py's
own format has no alpha channel, only a color key -- see its "MORE THAN ONE
ASSET TYPE" comment) and encoding the rest as RGB565, exactly the way
kf_pack_assets.make_png_sprite() already does for its own (alpha-less)
input.
"""

from __future__ import annotations

import argparse
import struct
import sys
import zlib
from dataclasses import dataclass
from pathlib import Path

from kf_character_manifest import DEFAULT_MANIFEST_PATH, SpriteSpec, iter_sprites, load_manifest

sys.path.insert(0, str(Path(__file__).parent))
import kf_pack_assets as packer  # noqa: E402  (see module docstring: reuse, not reinvent)

# The color key every ingested sprite is keyed to once its transparent
# pixels are resolved. Matches the magenta convention kf_pack_assets.py's
# own TEST_SPRITE_COLOR_KEY already uses in this codebase, for the same
# reason: a color no hand-drawn felt-tip creature would ever legitimately
# need (bible section 3's house style is flat, saturated fills, but this
# exact magenta is deliberately excluded from that palette by the prompt
# builder's own style block: "nothing else may use that exact color").
COLOR_KEY_RGB = (255, 0, 255)

# A pixel with alpha at or below this is treated as "background, to be
# keyed out." A pixel above it is treated as fully opaque foreground.
# There is no partial-alpha (soft edge) support in the pack format -- see
# ADR 0033, no such thing exists in ASSET_TYPE_SPRITE -- so this is a hard
# threshold, not a blend.
ALPHA_TRANSPARENT_THRESHOLD = 32

_PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


@dataclass
class IngestResult:
    spec: SpriteSpec
    status: str  # "ok", "missing", "error"
    detail: str = ""
    asset: dict | None = None  # populated when status == "ok"


def decode_png_rgba(data: bytes) -> tuple[int, int, bytes]:
    """Returns (width, height, rgba_bytes) -- 4 bytes/pixel, row-major.

    A near-twin of tools/kf_pack_assets.py's decode_png(), reusing that
    module's chunk parsing and its private row-unfiltering helpers
    (_unfilter_row / _paeth) rather than re-deriving the PNG filter math --
    the one thing genuinely worth not duplicating. The reason this can't
    just BE decode_png() is that decode_png() drops the alpha channel on
    purpose (its docstring: "dropped -- this format has no alpha, only a
    colour key"), and this tool's whole job is to look at that alpha
    channel before it's gone.
    """
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
            width, height, bit_depth, color_type = struct.unpack_from(">IIBB", chunk, 0)
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
    has_alpha = channels == 4
    stride = width * channels
    raw = zlib.decompress(bytes(idat))

    rgba = bytearray(width * height * 4)
    prev_row = bytearray(stride)
    p = 0
    for y in range(height):
        filter_type = raw[p]
        p += 1
        row = bytearray(raw[p:p + stride])
        p += stride
        packer._unfilter_row(filter_type, row, prev_row, channels)
        for x in range(width):
            o = x * channels
            d = (y * width + x) * 4
            if has_alpha:
                rgba[d:d + 4] = row[o:o + 4]
            else:
                rgba[d:d + 3] = row[o:o + 3]
                rgba[d + 3] = 255
        prev_row = row
    return width, height, bytes(rgba), has_alpha


def validate_and_load(spec: SpriteSpec, png_dir: Path) -> IngestResult:
    path = png_dir / spec.filename
    if not path.exists():
        return IngestResult(spec, "missing", f"no file at {path}")

    try:
        with open(path, "rb") as f:
            data = f.read()
        width, height, rgba, has_alpha = decode_png_rgba(data)
    except ValueError as e:
        return IngestResult(spec, "error", f"could not decode PNG: {e}")

    if (width, height) != (spec.width, spec.height):
        return IngestResult(
            spec, "error",
            f"is {width}x{height}, manifest expects {spec.width}x{spec.height}")

    if not has_alpha:
        return IngestResult(
            spec, "error",
            "has no alpha channel (RGB, not RGBA) -- a color key is "
            "expected here and there is nothing to key out")

    transparent_pixels = sum(
        1 for i in range(3, len(rgba), 4) if rgba[i] <= ALPHA_TRANSPARENT_THRESHOLD)
    if transparent_pixels == 0:
        return IngestResult(
            spec, "error",
            "has an alpha channel but every pixel is opaque -- the "
            "background was probably never removed")

    pixels = bytearray()
    key_r, key_g, key_b = COLOR_KEY_RGB
    for i in range(0, len(rgba), 4):
        r, g, b, a = rgba[i:i + 4]
        if a <= ALPHA_TRANSPARENT_THRESHOLD:
            r, g, b = key_r, key_g, key_b
        pixels += struct.pack("<H", packer.rgb565(r, g, b))

    key565 = packer.rgb565(*COLOR_KEY_RGB)
    asset = {
        "name": spec.sprite_name,
        "asset_type": packer.ASSET_TYPE_SPRITE,
        "type_meta": packer._sprite_type_meta(width, height, key565),
        "data": bytes(pixels),
    }
    return IngestResult(spec, "ok", f"{width}x{height}, "
                         f"{transparent_pixels}/{width * height} px keyed out", asset)


def verify_pack(data: bytes, expected_names: set[str]) -> list[str]:
    """An independent minimal parse of a freshly-written .kfpack: header,
    then directory, then a spot check of one payload's byte length against
    what its own directory entry claims. Deliberately does NOT reuse
    pack()'s own bookkeeping (offsets computed here are re-derived from the
    file's bytes, not carried over from packing) -- ADR 0033's own
    verification did the same thing for the same reason: "the check cannot
    pass by the parser merely agreeing with itself." Returns a list of
    problems (empty if none)."""
    problems = []
    if data[:4] != packer.MAGIC:
        return [f"bad magic: {data[:4]!r}"]
    version, entry_count, directory_offset = struct.unpack_from("<HHI", data, 4)
    if version != packer.FORMAT_VERSION:
        problems.append(f"unexpected format_version {version}")

    seen_names = set()
    file_len = len(data)
    for i in range(entry_count):
        entry_off = directory_offset + i * packer.DIRECTORY_ENTRY_BYTES
        name_bytes = data[entry_off:entry_off + packer.NAME_BYTES]
        name = name_bytes.split(b"\x00", 1)[0].decode("ascii")
        (asset_type,) = struct.unpack_from("<B", data, entry_off + 32)
        data_offset, data_bytes = struct.unpack_from("<II", data, entry_off + 44)
        if data_offset % 4 != 0:
            problems.append(f"entry '{name}': data_offset {data_offset} not 4-byte aligned")
        if data_offset + data_bytes > file_len:
            problems.append(f"entry '{name}': payload runs past end of file")
        if asset_type == packer.ASSET_TYPE_SPRITE:
            w, h = struct.unpack_from("<HH", data, entry_off + 36)
            if data_bytes != w * h * 2:
                problems.append(
                    f"entry '{name}': data_bytes {data_bytes} != w*h*2 "
                    f"({w}x{h} => {w * h * 2})")
        seen_names.add(name)

    missing = expected_names - seen_names
    if missing:
        problems.append(f"{len(missing)} expected name(s) not found in pack: "
                         f"{sorted(missing)[:5]}{'...' if len(missing) > 5 else ''}")
    return problems


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("png_dir", type=Path, help="directory of PNGs named by the manifest convention")
    p.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST_PATH)
    p.add_argument("-o", "--out", type=Path, help=".kfpack path to write (only sprites that validate are packed)")
    p.add_argument("--id", help="only this entity id, e.g. chokimaru")
    p.add_argument("--stage", choices=["egg", "baby", "child", "teen", "adult"])
    p.add_argument("--strict", action="store_true",
                    help="write nothing if anything is missing or invalid (default: pack what validates, report the rest)")
    args = p.parse_args(argv)

    raw = load_manifest(args.manifest)
    targets = []
    for spec in iter_sprites(raw):
        if args.id and spec.entity_id != args.id:
            continue
        if args.stage and spec.stage != args.stage:
            continue
        if not spec.has_design_brief:
            continue
        targets.append(spec)

    results = [validate_and_load(spec, args.png_dir) for spec in targets]
    ok = [r for r in results if r.status == "ok"]
    missing = [r for r in results if r.status == "missing"]
    errors = [r for r in results if r.status == "error"]

    print(f"checked {len(targets)} sprite(s) against {args.png_dir}")
    print(f"  ok: {len(ok)}")
    if missing:
        print(f"  missing: {len(missing)}")
        for r in missing:
            print(f"    - {r.spec.filename}: {r.detail}")
    if errors:
        print(f"  invalid: {len(errors)}")
        for r in errors:
            print(f"    - {r.spec.filename}: {r.detail}")

    unexpected = sorted(
        f.name for f in args.png_dir.glob("*.png")
        if f.name not in {r.spec.filename for r in results}
    ) if args.png_dir.is_dir() else []
    if unexpected:
        print(f"  unexpected files (no matching manifest entry): {len(unexpected)}")
        for name in unexpected:
            print(f"    - {name}")

    if args.strict and (missing or errors):
        print("\n--strict set: not writing a pack while any sprite is missing or invalid", file=sys.stderr)
        return 1

    if args.out:
        if not ok:
            print("\nnothing validated -- not writing a pack", file=sys.stderr)
            return 1
        assets = [r.asset for r in ok]
        data = packer.pack(assets)
        args.out.parent.mkdir(parents=True, exist_ok=True)
        with open(args.out, "wb") as f:
            f.write(data)
        print(f"\nwrote {args.out}: {len(assets)} entries, {len(data)} bytes")

        problems = verify_pack(data, {a["name"] for a in assets})
        if problems:
            print(f"pack verification FAILED ({len(problems)} problem(s)):", file=sys.stderr)
            for prob in problems:
                print(f"  - {prob}", file=sys.stderr)
            return 1
        print(f"pack verification: OK ({len(assets)} entries independently re-parsed and matched)")

    return 1 if (missing or errors) else 0


if __name__ == "__main__":
    sys.exit(main())
