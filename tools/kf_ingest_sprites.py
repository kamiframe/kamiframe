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

PACKING ITSELF IS NOT REIMPLEMENTED HERE. Every entry that passes
validation is handed to tools/kf_pack_assets.py's own pack() -- imported,
not copied -- so there is exactly one place in the repo that knows the
.kfpack byte layout (see that file's own docstring, and ADR 0033). This
file's own contribution is two-fold: turning a transparent PNG into RGB565
with an explicit color key (kf_pack_assets.py's own format has no alpha
channel, only a color key -- see its "MORE THAN ONE ASSET TYPE" comment),
which validate_and_load() does per PNG; and grouping one animation's frames
-- kf_character_manifest.py's EntrySpec, one .kfpack entry -- into a single
8bpp palette-indexed asset, which build_entry() does per entry, quantizing
across all of that entry's frames at once via kf_pack_assets.quantize_rgb565()
so they share one palette (see build_entry()'s own docstring for why that
has to happen per entry, not per frame or globally).
"""

from __future__ import annotations

import argparse
import struct
import sys
import zlib
from dataclasses import dataclass
from pathlib import Path

from kf_character_manifest import (
    ALL_STAGE_KEYS,
    DEFAULT_MANIFEST_PATH,
    EntrySpec,
    SpriteSpec,
    iter_entries,
    load_manifest,
)

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
    entry_name: str = ""      # spec.entry_name, denormalized so
                               # verify_lossless() can group frames back
                               # into entries without re-touching the
                               # manifest
    rgb565: bytes | None = None  # populated when status == "ok": this
                                  # frame's pixels, RGB565, 2 bytes/pixel,
                                  # row-major -- the same bytes build_entry()
                                  # quantizes and verify_lossless() compares
                                  # the indexed pack back against


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

    return IngestResult(spec, "ok", f"{width}x{height}, "
                         f"{transparent_pixels}/{width * height} px keyed out",
                         entry_name=spec.entry_name, rgb565=bytes(pixels))


def build_entry(entry: EntrySpec, results_by_name: dict[str, list[IngestResult]]) -> dict | None:
    """Turns one EntrySpec's validated frames into one packable indexed
    asset. The palette is built ACROSS ALL FRAMES OF THIS ENTRY at once, not
    per frame: they share a payload and therefore must share a palette, or
    index 7 would mean one colour in frame 0 and another in frame 1.

    Per ENTRY, not per entity, and certainly not globally: measured, each of
    this roster's sprites uses 6 to 27 distinct colours while the union
    across just five entities is already 201, so a shared palette is a
    ceiling that would be hit late, after the format froze. A per-entry
    palette is ~64 bytes -- about 24KB across the whole projected roster,
    against 7.5MB of pixels.

    Returns None if this entry's frames did not all validate (a missing or
    invalid PNG anywhere in the animation) -- there is no such thing as a
    partial indexed entry, so the caller simply leaves it out of the pack,
    the same way an individual bad sprite was always left out before this
    task grouped frames into entries."""
    frames = results_by_name.get(entry.entry_name, [])
    if len(frames) != entry.frame_count:
        return None
    key565 = packer.rgb565(*COLOR_KEY_RGB)
    per_frame = [list(struct.unpack(f"<{len(r.rgb565) // 2}H", r.rgb565))
                 for r in frames]
    palette, index_frames = packer.quantize_rgb565(per_frame, key565)
    spec = frames[0].spec
    return packer.make_indexed_asset(entry.entry_name, spec.width, spec.height,
                                      index_frames, palette,
                                      has_color_key=True, color_key=key565)


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
        elif asset_type == packer.ASSET_TYPE_SPRITE_INDEXED:
            w, h, frame_count = struct.unpack_from("<HHH", data, entry_off + 36)
            pal_count = data[entry_off + 42] + 1
            pal_bytes_padded = ((pal_count * 2) + 3) & ~3
            expected = pal_bytes_padded + frame_count * w * h
            if data_bytes != expected:
                problems.append(
                    f"entry '{name}': data_bytes {data_bytes} != "
                    f"palette_bytes_padded + frame_count*w*h ({expected})")
        seen_names.add(name)

    missing = expected_names - seen_names
    if missing:
        problems.append(f"{len(missing)} expected name(s) not found in pack: "
                         f"{sorted(missing)[:5]}{'...' if len(missing) > 5 else ''}")
    return problems


def verify_lossless(data: bytes, results: list[IngestResult]) -> list[str]:
    """Expands every indexed entry in a freshly-written pack back to RGB565
    and compares it, pixel for pixel, against the RGB565 those same PNGs
    resolve to. Reads the pack's OWN bytes rather than reusing the packing
    bookkeeping -- the same reasoning verify_pack() already gives: the check
    cannot pass by the packer merely agreeing with itself.

    This is the whole justification for choosing 8bpp over 4bpp. If it ever
    reports a problem, the art has more than 256 colours in one entry or the
    quantiser has a bug; either way the pack is no longer a lossless
    re-encoding of the source and must not ship as one."""
    problems = []
    by_name: dict[str, list[IngestResult]] = {}
    for r in results:
        if r.status == "ok":
            by_name.setdefault(r.entry_name, []).append(r)

    version, entry_count, directory_offset = struct.unpack_from("<HHI", data, 4)
    for i in range(entry_count):
        e = directory_offset + i * packer.DIRECTORY_ENTRY_BYTES
        name = data[e:e + packer.NAME_BYTES].split(b"\x00", 1)[0].decode("ascii")
        asset_type = data[e + 32]
        if asset_type != packer.ASSET_TYPE_SPRITE_INDEXED:
            problems.append(f"entry '{name}': expected an indexed sprite, "
                             f"got asset_type {asset_type}")
            continue
        w, h, frames = struct.unpack_from("<HHH", data, e + 36)
        pal_count = data[e + 42] + 1
        off, nbytes = struct.unpack_from("<II", data, e + 44)
        pal_padded = ((pal_count * 2) + 3) & ~3
        palette = list(struct.unpack_from(f"<{pal_count}H", data, off))
        idx = data[off + pal_padded:off + nbytes]

        expected = by_name.get(name, [])
        if len(expected) != frames:
            problems.append(f"entry '{name}': pack says {frames} frame(s), "
                             f"{len(expected)} source PNG(s) fed it")
            continue
        for k, r in enumerate(expected):
            src = struct.unpack(f"<{w * h}H", r.rgb565)
            got = [palette[b] for b in idx[k * w * h:(k + 1) * w * h]]
            if list(src) != got:
                bad = next(j for j in range(w * h) if src[j] != got[j])
                problems.append(
                    f"entry '{name}' frame {k}: pixel {bad} expanded to "
                    f"0x{got[bad]:04X}, source is 0x{src[bad]:04X} -- the "
                    "indexed encoding is LOSSY, which it must never be")
    return problems


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("png_dir", type=Path, help="directory of PNGs named by the manifest convention")
    p.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST_PATH)
    p.add_argument("-o", "--out", type=Path, help=".kfpack path to write (only sprites that validate are packed)")
    p.add_argument("--id", help="only this entity id, e.g. chokimaru")
    p.add_argument("--stage", choices=list(ALL_STAGE_KEYS))
    p.add_argument("--strict", action="store_true",
                    help="write nothing if anything is missing or invalid (default: pack what validates, report the rest)")
    p.add_argument("--verify-lossless", action="store_true",
                    help="with --out: re-read the written pack's own bytes, expand every indexed "
                         "entry back to RGB565 through its own palette, and compare pixel for "
                         "pixel against the RGB565 the source PNGs decode to")
    args = p.parse_args(argv)

    raw = load_manifest(args.manifest)

    def entry_wanted(entry: EntrySpec) -> bool:
        # Every frame of one entry shares one entity/stage/design-brief
        # state (they are the same entity's same pose), so the first
        # frame speaks for the whole entry -- this is just iter_sprites()'
        # own per-spec filter, applied once per entry instead of once per
        # frame, so an --id/--stage filter still selects whole animations,
        # never half of one.
        first = entry.frames[0]
        if args.id and first.entity_id != args.id:
            return False
        if args.stage and first.stage != args.stage:
            return False
        if not first.has_design_brief:
            return False
        return True

    entry_targets = [e for e in iter_entries(raw) if entry_wanted(e)]
    targets = [frame for e in entry_targets for frame in e.frames]

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
        ok_by_name: dict[str, list[IngestResult]] = {}
        for r in ok:
            ok_by_name.setdefault(r.entry_name, []).append(r)
        assets = [a for a in (build_entry(e, ok_by_name) for e in entry_targets) if a is not None]

        if not assets:
            print("\nnothing validated -- not writing a pack", file=sys.stderr)
            return 1
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

        if args.verify_lossless:
            problems = verify_lossless(data, ok)
            if problems:
                print(f"lossless verification FAILED ({len(problems)} problem(s)):", file=sys.stderr)
                for prob in problems:
                    print(f"  - {prob}", file=sys.stderr)
                return 1
            pixels = sum(len(r.rgb565) // 2 for r in ok)
            print(f"lossless verification: OK ({len(assets)} entries, "
                  f"{pixels:,} pixels expanded and matched)")

    return 1 if (missing or errors) else 0


if __name__ == "__main__":
    sys.exit(main())
