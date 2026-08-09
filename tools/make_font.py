#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright the Kamiframe contributors.
"""Generate the slice-two bitmap font as a C header.

Sprites now go through the real asset pipeline (tools/kf_pack_assets.py,
docs/architecture/adr-0033-asset-pipeline.md) instead of a baked-in header
-- but the font stays exactly as it was, checked in as a header, on
purpose: it is small, fixed, never swapped for a different font at
runtime, and gains nothing from living in a flash partition that
sprites and eventually sound genuinely need for their size and
variety. The glyphs below are drawn from scratch in this script, not
extracted from any existing font file or library, so there is no
third-party bitmap data to license.

Character set is deliberately small: space, 0-9, A-Z (uppercase only), and
the punctuation the constraint HUD needs (. , : - / % + ( )). That is 46
glyphs. Lowercase and the rest of ASCII punctuation are a later, mechanical
addition to GLYPHS below, not a redesign -- see docs/architecture/adr-0010.

Each glyph is 5 wide by 7 tall, authored as ASCII art ('#' = lit, '.' = not)
so a human can read and edit the shape directly, then packed into one byte
per row (bit 4 = leftmost pixel, bit 0 = rightmost, for a 5-bit-wide row).

Usage:
    python3 tools/make_font.py > hakoniwaos/src/font_data.h
    python3 tools/make_font.py --preview /tmp/font_preview.png   # sanity check only, not shipped
"""

import sys

GLYPH_W = 5
GLYPH_H = 7

# fmt: off
GLYPHS = {
    " ": ["....."] * 7,

    "0": [".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###."],
    "1": ["..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###."],
    "2": [".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####"],
    "3": [".###.", "#...#", "....#", "..##.", "....#", "#...#", ".###."],
    "4": ["...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#."],
    "5": ["#####", "#....", "####.", "....#", "....#", "#...#", ".###."],
    "6": ["..##.", ".#...", "#....", "####.", "#...#", "#...#", ".###."],
    "7": ["#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#..."],
    "8": [".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###."],
    "9": [".###.", "#...#", "#...#", ".####", "....#", "...#.", ".##.."],

    "A": ["..#..", ".#.#.", "#...#", "#...#", "#####", "#...#", "#...#"],
    "B": ["####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."],
    "C": [".###.", "#...#", "#....", "#....", "#....", "#...#", ".###."],
    "D": ["####.", "#...#", "#...#", "#...#", "#...#", "#...#", "####."],
    "E": ["#####", "#....", "#....", "####.", "#....", "#....", "#####"],
    "F": ["#####", "#....", "#....", "####.", "#....", "#....", "#...."],
    "G": [".###.", "#...#", "#....", "#.###", "#...#", "#...#", ".###."],
    "H": ["#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"],
    "I": [".###.", "..#..", "..#..", "..#..", "..#..", "..#..", ".###."],
    "J": ["..###", "...#.", "...#.", "...#.", "...#.", "#..#.", ".##.."],
    "K": ["#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#"],
    "L": ["#....", "#....", "#....", "#....", "#....", "#....", "#####"],
    "M": ["#...#", "##.##", "#.#.#", "#...#", "#...#", "#...#", "#...#"],
    "N": ["#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#", "#...#"],
    "O": [".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."],
    "P": ["####.", "#...#", "#...#", "####.", "#....", "#....", "#...."],
    "Q": [".###.", "#...#", "#...#", "#...#", "#.#.#", "#..#.", ".##.#"],
    "R": ["####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"],
    "S": [".####", "#....", "#....", ".###.", "....#", "....#", "####."],
    "T": ["#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."],
    "U": ["#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."],
    "V": ["#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#.."],
    "W": ["#...#", "#...#", "#...#", "#.#.#", "#.#.#", "#.#.#", ".#.#."],
    "X": ["#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#"],
    "Y": ["#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.."],
    "Z": ["#####", "....#", "...#.", "..#..", ".#...", "#....", "#####"],
}
# fmt: on

# Punctuation, defined separately so the alphabet block above stays a clean
# reference table.
GLYPHS["."] = [".....", ".....", ".....", ".....", ".....", ".....", "..#.."]
GLYPHS[","] = [".....", ".....", ".....", ".....", ".....", "..#..", ".#..."]
GLYPHS[":"] = [".....", ".....", "..#..", ".....", "..#..", ".....", "....."]
GLYPHS["-"] = [".....", ".....", ".....", "#####", ".....", ".....", "....."]
GLYPHS["/"] = ["....#", "...#.", "...#.", "..#..", ".#...", ".#...", "#...."]
GLYPHS["%"] = ["#...#", "#..#.", "...#.", "..#..", ".#...", ".#..#", "#...#"]
GLYPHS["+"] = [".....", "..#..", "..#..", "#####", "..#..", "..#..", "....."]
GLYPHS["("] = ["...#.", "..#..", ".#...", ".#...", ".#...", "..#..", "...#."]
GLYPHS[")"] = [".#...", "..#..", "...#.", "...#.", "...#.", "..#..", ".#..."]


def validate():
    for ch, rows in GLYPHS.items():
        assert len(rows) == GLYPH_H, f"{ch!r} has {len(rows)} rows, want {GLYPH_H}"
        for row in rows:
            assert len(row) == GLYPH_W, f"{ch!r} row {row!r} has wrong width"
            assert set(row) <= {"#", "."}, f"{ch!r} row {row!r} has bad chars"


def rows_to_bits(rows):
    out = []
    for row in rows:
        byte = 0
        for i, c in enumerate(row):
            if c == "#":
                byte |= 1 << (GLYPH_W - 1 - i)
        out.append(byte)
    return out


def emit_header(out):
    first = ord(" ")
    last = ord("~")  # 0x7E, end of printable ASCII
    validate()

    out.write("/* SPDX-License-Identifier: Apache-2.0\n")
    out.write(" * Copyright the Kamiframe contributors.\n")
    out.write(" *\n")
    out.write(" * GENERATED FILE. Do not edit by hand.\n")
    out.write(" * Regenerate: python3 tools/make_font.py > hakoniwaos/src/font_data.h\n")
    out.write(" *\n")
    out.write(" * One row byte per scanline, bit 4 = leftmost of %d pixels.\n" % GLYPH_W)
    out.write(" * Indexed by (char - KF_FONT_FIRST_CHAR). Characters with no\n")
    out.write(" * glyph defined below are all-zero, which kf_text_draw treats\n")
    out.write(" * as blank -- see kf/font.h. Private to font.cpp: nothing else\n")
    out.write(" * should include this directly.\n")
    out.write(" */\n\n")
    out.write("#ifndef KF_FONT_DATA_H\n#define KF_FONT_DATA_H\n\n")
    out.write("#include <stdint.h>\n\n")
    out.write("#define KF_FONT_FIRST_CHAR %d /* '%s' */\n" % (first, chr(first)))
    out.write("#define KF_FONT_LAST_CHAR  %d /* '%s' */\n" % (last, chr(last)))
    out.write("#define KF_FONT_GLYPH_COUNT (KF_FONT_LAST_CHAR - KF_FONT_FIRST_CHAR + 1)\n\n")
    out.write("static const uint8_t kf_font_glyphs[KF_FONT_GLYPH_COUNT][%d] = {\n" % GLYPH_H)

    defined = 0
    for code in range(first, last + 1):
        ch = chr(code)
        rows = GLYPHS.get(ch)
        if rows is None:
            bits = [0] * GLYPH_H
        else:
            bits = rows_to_bits(rows)
            defined += 1
        label = ch if ch not in ('\\', "'") else ("BACKSLASH" if ch == '\\' else "APOSTROPHE")
        out.write("    { " + ", ".join("0x%02X" % b for b in bits) +
                   " }, /* 0x%02X %s */\n" % (code, repr(ch)))

    out.write("};\n\n")
    out.write("#endif /* KF_FONT_DATA_H */\n")
    sys.stderr.write("make_font: %d of %d printable-ASCII glyphs defined\n" %
                      (defined, last - first + 1))


def emit_preview(path):
    from PIL import Image

    validate()
    chars = sorted(GLYPHS.keys())
    cols = 12
    rows_n = (len(chars) + cols - 1) // cols
    cell = 8  # scaled pixel size in the preview
    margin = 1
    img_w = cols * (GLYPH_W + margin) * cell
    img_h = rows_n * (GLYPH_H + margin) * cell
    img = Image.new("RGB", (img_w, img_h), (20, 20, 28))
    px = img.load()

    for idx, ch in enumerate(chars):
        col = idx % cols
        row = idx // cols
        ox = col * (GLYPH_W + margin) * cell
        oy = row * (GLYPH_H + margin) * cell
        for gy, line in enumerate(GLYPHS[ch]):
            for gx, c in enumerate(line):
                color = (230, 235, 245) if c == "#" else (40, 44, 58)
                for dy in range(cell):
                    for dx in range(cell):
                        px[ox + gx * cell + dx, oy + gy * cell + dy] = color

    img.save(path)
    sys.stderr.write("make_font: preview written to %s (%d glyphs)\n" % (path, len(chars)))


def main() -> int:
    if len(sys.argv) >= 3 and sys.argv[1] == "--preview":
        emit_preview(sys.argv[2])
        return 0
    emit_header(sys.stdout)
    return 0


if __name__ == "__main__":
    sys.exit(main())
