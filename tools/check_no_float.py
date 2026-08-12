#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright the Kamiframe contributors.
"""Fail if core reaches for a float or a double.

hakoniwaos/ is float-free by design (kf/scene.h and kf/clock.h both say so
in their own header comments), but until this script existed nothing checked
it. That is not hypothetical: `1000000.0 / us` -- a genuine floating-point
division -- shipped in hakoniwaos/src/app.cpp's frame-budget log line and
went unnoticed until an audit caught it (see kf/scene.h's header comment for
the full story). It was rewritten as an integer tenths-of-fps calculation,
but nothing stopped the next one.

Why this matters on real hardware: the ESP32-S3 has a single-precision FPU,
but `double` is software-emulated -- slow, and worse, invisible in a review
unless someone is specifically looking for the word "double". A stray float
or double in a per-frame path is a silent, hard-to-find frame-budget cost.
kf/budget.h's own reasoning is that the FPU is for Lua; Core stays exact and
cheap using fixed-point / integer math instead.

This is a grep-based heuristic, same as tools/check_no_heap.py, not a
compiler. It cannot see everything a real type-checker would (see the
false-positive/false-negative notes below), but it is cheap, runs on every
platform, and catches the category of mistake that actually shipped.

What it flags, in non-comment code:
  - the `float` / `double` keyword (declarations, params, casts, return types)
  - <math.h>'s `float_t` / `double_t` typedefs
  - a decimal-point numeric literal, e.g. `1000000.0` or `0.5f` -- these are
    `double` (or `float`, with an `f`/`F` suffix) even with no keyword in
    sight, which is exactly how the app.cpp bug above slipped through
  - a printf-style `%f`/`%g`/`%e`-family format specifier, which only makes
    sense if a float or double is being passed to it

What it deliberately does NOT flag:
  - comment-only lines (the word "float" and "double" show up constantly in
    Core's own comments explaining why there ISN'T one -- see kf/pet.h,
    kf/creature.h, kf/scene.h, kf/clock.h). Same heuristic check_no_heap.py
    uses: a stripped line starting with `*`, `//`, or `/*` is a comment line
    and is skipped whole. This project's comment style consistently prefixes
    continuation lines with ` * `, so this catches the real prose. It will
    NOT catch a trailing `// float` comment tacked onto a code line -- same
    known limitation check_no_heap.py accepts for the same reason: cheap and
    good enough beats a real parser for a grep-based gate.
  - `%d`, `%u`, `%x`, `%s`, `%p`, `%c` and friends: the format-specifier
    pattern only matches the f/F/e/E/g/G family, not integer or string
    specifiers.

Usage:
    python3 tools/check_no_float.py [repo_root]
"""

import pathlib
import re
import sys

SCAN_DIRS = ["hakoniwaos/src", "hakoniwaos/include"]

PATTERNS = [
    (re.compile(r"\bfloat\b"), "float"),
    (re.compile(r"\bdouble\b"), "double"),
    (re.compile(r"\bfloat_t\b"), "float_t"),
    (re.compile(r"\bdouble_t\b"), "double_t"),
    # Decimal-point numeric literal: `1000000.0`, `0.5f`, `.5`, with an
    # optional exponent and an optional f/F/l/L suffix. This is the pattern
    # that actually shipped a bug (see module docstring): no `float` keyword
    # anywhere in sight, just a literal that is one by grammar.
    (re.compile(r"\b\d*\.\d+(?:[eE][+-]?\d+)?[fFlL]?\b"), "floating-point literal"),
    (re.compile(r"\b\d+\.\d*(?:[eE][+-]?\d+)?[fFlL]?\b"), "floating-point literal"),
    # printf-style float/double format specifier: %f, %.2f, %lf, %g, %e ...
    # Deliberately excludes %d %u %x %s %p %c -- those take integers/strings.
    (re.compile(r"%[-+0# ]*\d*(?:\.\d+)?(?:hh|h|ll|l|L)?[fFeEgG]"),
     "floating-point format specifier"),
]

ALLOW_MARKER = "kf-allow-float"


def main() -> int:
    root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
    problems = []
    missing_dirs = []
    scanned = 0

    for rel in SCAN_DIRS:
        base = root / rel
        if not base.is_dir():
            missing_dirs.append(rel)
            continue
        for path in sorted(base.rglob("*")):
            if path.suffix not in (".c", ".cpp", ".h", ".hpp"):
                continue
            scanned += 1
            text = path.read_text(encoding="utf-8", errors="replace")
            for lineno, line in enumerate(text.splitlines(), start=1):
                stripped = line.strip()
                if stripped.startswith(("*", "//", "/*")):
                    continue
                if ALLOW_MARKER in line:
                    continue
                for pattern, label in PATTERNS:
                    if pattern.search(line):
                        problems.append(
                            (path.relative_to(root), lineno, label,
                             stripped[:100]))
                        break

    if problems:
        print("Float use found in core. Core is fixed-point / integer only.\n")
        for path, lineno, label, snippet in problems:
            print(f"  {path}:{lineno}: {label}")
            print(f"      {snippet}")
        print("\nIf a line is a genuine, reviewed exception, add the comment "
              f"{ALLOW_MARKER} to it. Think hard before you do.")
        return 1

    if missing_dirs:
        print("check_no_float: expected scan directories are missing, "
              "the gate has nothing to check:")
        for rel in missing_dirs:
            print(f"  {rel}")
        return 1

    if scanned == 0:
        print("check_no_float: scanned 0 files, refusing to pass a no-op gate "
              f"(checked {', '.join(SCAN_DIRS)})")
        return 1

    print(f"check_no_float: core is float-free ({scanned} files scanned)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
