#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright the Kamiframe contributors.
"""Fail if core reaches for the heap.

kf/poison.h already makes malloc a compile error under GCC and Clang, which
covers the ESP32 toolchain. It cannot see two things:

  - global operator new / delete, which are not identifiers
  - MSVC, which ignores #pragma GCC poison entirely

So this runs as well, in CI, on every push. Between the two, an accidental
heap allocation in core cannot reach main.

Core does not use the heap because the whole constraint story rests on fixed
arenas sized from kf/budget.h. An allocation that routes around them is a hole
in the enforcement, not a style preference.

Backends under simulator/ are exempt: allocating is their job.

Usage:
    python3 tools/check_no_heap.py [repo_root]
"""

import pathlib
import re
import sys

SCAN_DIRS = ["hakoniwaos/src", "hakoniwaos/include"]

# Word-boundary matches so kf_arena_alloc and similar do not trip it.
PATTERNS = [
    (re.compile(r"\bmalloc\s*\("), "malloc"),
    (re.compile(r"\bcalloc\s*\("), "calloc"),
    (re.compile(r"\brealloc\s*\("), "realloc"),
    (re.compile(r"\bfree\s*\("), "free"),
    (re.compile(r"\bstrdup\s*\("), "strdup"),
    (re.compile(r"(?<![\w:])new\s+[A-Za-z_]"), "operator new"),
    (re.compile(r"(?<![\w:])delete\s+[A-Za-z_]"), "operator delete"),
    (re.compile(r"(?<![\w:])delete\s*\[\s*\]"), "operator delete[]"),
    (re.compile(r"\bstd::(vector|string|map|unordered_map|set|list|deque|"
                r"shared_ptr|unique_ptr|make_shared|make_unique|function)\b"),
     "heap-allocating std:: type"),
]

ALLOW_MARKER = "kf-allow-heap"


def main() -> int:
    root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
    problems = []

    for rel in SCAN_DIRS:
        base = root / rel
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if path.suffix not in (".c", ".cpp", ".h", ".hpp"):
                continue
            # poison.h names the tokens it poisons, by necessity.
            if path.name == "poison.h":
                continue
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
        print("Heap use found in core. Core allocates from kf/arena.h only.\n")
        for path, lineno, label, snippet in problems:
            print(f"  {path}:{lineno}: {label}")
            print(f"      {snippet}")
        print("\nIf a line is a genuine, reviewed exception, add the comment "
              f"{ALLOW_MARKER} to it. Think hard before you do.")
        return 1

    print("check_no_heap: core is heap-free")
    return 0


if __name__ == "__main__":
    sys.exit(main())
