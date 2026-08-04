#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright the Kamiframe contributors.
"""Generate the slice-one test sprite as a C header.

There is no asset pipeline yet, so the sprite is generated once and checked
in as a header. That keeps the build dependency-free and makes the first
thing on screen reproducible.

The sprite is a plain geometric blob drawn from code on purpose. It is not a
Kamiframe creature and carries no design intent: the creature class name is
deliberately undecided and the demo pet's artwork will be separately
copyrighted. Do not treat this as a character.

Usage:
    python3 tools/make_test_sprite.py > examples/hello_sprite/sprite_data.h
"""

import sys

W = 32
H = 32
COLOR_KEY = (255, 0, 255)  # magenta, never drawn


def rgb565(r: int, g: int, b: int) -> int:
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | ((b & 0xF8) >> 3)


def pixel(x: int, y: int):
    cx, cy = (W - 1) / 2.0, (H - 1) / 2.0 + 2.0
    dx, dy = x - cx, y - cy

    # Body: a squat ellipse.
    body = (dx * dx) / (13.5 * 13.5) + (dy * dy) / (12.0 * 12.0)
    if body > 1.0:
        return COLOR_KEY

    # Outline ring.
    if body > 0.82:
        return (40, 46, 72)

    # Eyes.
    for ex in (-5.0, 5.0):
        ed = (x - (cx + ex)) ** 2 / 6.0 + (y - (cy - 3.0)) ** 2 / 8.0
        if ed <= 1.0:
            return (28, 32, 54)
        if ed <= 2.2:
            return (250, 250, 252)

    # Cheeks.
    for ex in (-8.0, 8.0):
        cd = (x - (cx + ex)) ** 2 / 5.0 + (y - (cy + 2.5)) ** 2 / 3.0
        if cd <= 1.0:
            return (240, 150, 160)

    # Body shading: lighter toward the top left.
    shade = 1.0 - (body * 0.30) - (dy / float(H)) * 0.25
    shade = max(0.55, min(1.0, shade))
    base = (150, 210, 190)
    return tuple(int(c * shade) for c in base)


def main() -> int:
    rows = []
    for y in range(H):
        values = []
        for x in range(W):
            r, g, b = pixel(x, y)
            values.append("0x%04X" % rgb565(r, g, b))
        rows.append(values)

    out = sys.stdout
    out.write("/* SPDX-License-Identifier: Apache-2.0\n")
    out.write(" * Copyright the Kamiframe contributors.\n")
    out.write(" *\n")
    out.write(" * GENERATED FILE. Do not edit by hand.\n")
    out.write(" * Regenerate: python3 tools/make_test_sprite.py"
              " > examples/hello_sprite/sprite_data.h\n")
    out.write(" *\n")
    out.write(" * A geometric test blob, %dx%d, RGB565, magenta color key.\n" % (W, H))
    out.write(" * Not a Kamiframe creature and not a design. It exists so the\n")
    out.write(" * framebuffer has something to draw before there is an asset\n")
    out.write(" * pipeline.\n")
    out.write(" */\n\n")
    out.write("#ifndef KF_TEST_SPRITE_H\n#define KF_TEST_SPRITE_H\n\n")
    out.write('#include "kf/types.h"\n\n')
    out.write("#define KF_TEST_SPRITE_WIDTH  %d\n" % W)
    out.write("#define KF_TEST_SPRITE_HEIGHT %d\n" % H)
    out.write("#define KF_TEST_SPRITE_COLOR_KEY 0x%04X\n\n"
              % rgb565(*COLOR_KEY))
    out.write("static const kf_color kf_test_sprite_pixels"
              "[KF_TEST_SPRITE_WIDTH * KF_TEST_SPRITE_HEIGHT] = {\n")
    for values in rows:
        for i in range(0, W, 8):
            out.write("    " + ", ".join(values[i:i + 8]) + ",\n")
    out.write("};\n\n#endif /* KF_TEST_SPRITE_H */\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
