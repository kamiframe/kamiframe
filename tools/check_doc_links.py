#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright the Kamiframe contributors.
"""Find dangling references in the docs: broken relative links, and ADR
numbers named in prose that were never written.

This project's plans and ADRs get copied forward into task briefs verbatim
(see CLAUDE.md's "If you are the operator" section) -- a stale link or a
reference to an ADR number that does not exist is not cosmetic, it gets
re-served to whoever reads it next. This script exists because that has
already happened at least twice: docs/architecture/README.md's own index
has drifted out of date three separate times (see that file's own closing
note), and a planning-doc reference to `08-phase1-slice1-decisions.md`
pointed at a file that was never in this repo.

What it checks, across every `*.md` file in the repo (except `_archive/`
and anything under a build directory):

  1. Relative markdown links, `[text](path)` or `[text](path#anchor)` --
     resolved relative to the linking file's own directory, flagged if the
     target does not exist. Links starting with a URL scheme (http://,
     https://, mailto:) or a bare `#anchor` are skipped, they are not
     filesystem references.
  2. `ADR 00NN` mentions in prose -- cross-checked against the ADR files
     that actually exist under docs/architecture/. Flags a mention of an
     ADR number with no corresponding `adr-00NN-*.md` file. (0037 and 0038
     are known-skipped numbers -- see docs/architecture/README.md's own
     note -- and are not flagged.)

This is a heuristic, not a proof: it does not follow symlinks specially,
does not understand HTML `<a href>` tags, and a mention like "ADR 0030"
inside a code block or a quoted diff would still be checked (false
positives there are possible but have not been observed in practice).

Usage:
    python3 tools/check_doc_links.py [repo_root]
"""

import pathlib
import re
import sys

LINK_RE = re.compile(r"\[[^\]]*\]\(([^)]+)\)")
ADR_MENTION_RE = re.compile(r"\bADR\s+(\d{4})\b")
ADR_FILE_RE = re.compile(r"^adr-(\d{4})-")

SKIP_DIR_NAMES = {"_archive", "build", "build-desktop", "node_modules", ".git"}
KNOWN_SKIPPED_ADRS = {37, 38}


def iter_markdown_files(root: pathlib.Path):
    for path in sorted(root.rglob("*.md")):
        if any(part in SKIP_DIR_NAMES for part in path.parts):
            continue
        yield path


def non_code_lines(text: str):
    """Yield (lineno, line) pairs, skipping fenced ``` code blocks.

    Plans and ADRs quote C++ snippets constantly, and a lambda capture list
    like `[&ok](bool cond, ...)` parses as a markdown link to a file named
    "bool" if code blocks are not excluded. Indented (4-space) code blocks
    are not tracked here -- they are rare in this repo's docs and the ones
    that exist are inside already-fenced blocks -- so this is a heuristic,
    not a full Markdown parser, same tradeoff the rest of this script makes.
    """
    in_fence = False
    for lineno, line in enumerate(text.splitlines(), start=1):
        stripped = line.strip()
        if stripped.startswith("```"):
            in_fence = not in_fence
            continue
        if in_fence:
            continue
        yield lineno, line


def check_links(root: pathlib.Path, files):
    problems = []
    for path in files:
        text = path.read_text(encoding="utf-8", errors="replace")
        for lineno, line in non_code_lines(text):
            for match in LINK_RE.finditer(line):
                target = match.group(1).strip()
                if not target:
                    continue
                if target.startswith(("http://", "https://", "mailto:", "#")):
                    continue
                if target.startswith("<") and target.endswith(">"):
                    target = target[1:-1]
                # Strip a trailing anchor and any "title" suffix.
                target = target.split(" ", 1)[0]
                target = target.split("#", 1)[0]
                if not target:
                    continue
                resolved = (path.parent / target).resolve()
                if not resolved.exists():
                    problems.append(
                        (path.relative_to(root), lineno, target))
    return problems


def known_adr_numbers(root: pathlib.Path):
    numbers = set()
    arch_dir = root / "docs" / "architecture"
    if not arch_dir.is_dir():
        return numbers
    for path in arch_dir.glob("adr-*.md"):
        m = ADR_FILE_RE.match(path.name)
        if m:
            numbers.add(int(m.group(1)))
    return numbers


def check_adr_mentions(root: pathlib.Path, files, known):
    problems = []
    for path in files:
        text = path.read_text(encoding="utf-8", errors="replace")
        for lineno, line in non_code_lines(text):
            for match in ADR_MENTION_RE.finditer(line):
                number = int(match.group(1))
                if number in known or number in KNOWN_SKIPPED_ADRS:
                    continue
                problems.append(
                    (path.relative_to(root), lineno, number, line.strip()[:100]))
    return problems


def main() -> int:
    root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
    files = list(iter_markdown_files(root))

    if len(files) == 0:
        print("check_doc_links: scanned 0 markdown files, refusing to pass "
              "a no-op gate")
        return 1

    link_problems = check_links(root, files)
    known = known_adr_numbers(root)
    adr_problems = check_adr_mentions(root, files, known)

    if link_problems or adr_problems:
        if link_problems:
            print("Dangling relative links found:\n")
            for path, lineno, target in link_problems:
                print(f"  {path}:{lineno}: -> {target}")
        if adr_problems:
            if link_problems:
                print()
            print("ADR numbers mentioned but never written:\n")
            for path, lineno, number, snippet in adr_problems:
                print(f"  {path}:{lineno}: ADR {number:04d}")
                print(f"      {snippet}")
        return 1

    print(f"check_doc_links: {len(files)} markdown files scanned, "
          f"{len(known)} ADRs known, nothing dangling")
    return 0


if __name__ == "__main__":
    sys.exit(main())
