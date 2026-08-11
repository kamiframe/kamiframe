#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright the Kamiframe contributors.
"""Embed a .lua file into a C++ header as a raw string literal.

Task 1 of docs/superpowers/plans/2026-08-12-lua-game-layer.md makes the
demo pet's script a real file on disk (examples/creature_demo/creature.lua)
instead of a C++ raw string literal a developer had to edit and recompile.
This tool is the other half of that: it turns a .lua file back into the
`inline constexpr const char *` + R"lua(...)lua" shape the build has always
compiled, so nothing about how the script reaches the VM changes -- only
where a human edits it.

The generated header is CHECKED IN, not produced at configure time: see the
plan's "Decisions already taken" section for why (a generator wired into
two build systems, including ESP-IDF's two-pass configure, is worse than a
regenerate-and-diff test). `lua_embed_check` (simulator/CMakeLists.txt)
runs this script with --check on every `ctest` run, which is what keeps a
checked-in header from silently drifting away from its .lua source.

Refuses to embed a script whose text contains the literal sequence
)lua" -- that sequence would close the generated raw string literal early
and produce a C++ compile error far away from the line that actually
caused it, so it is caught here instead, naming the offending line.

Usage:
    python3 tools/kf_embed_lua.py
        Regenerate every header in SCRIPTS below, in place. This is the
        whole loop for editing the demo creature: edit the .lua file, run
        this with no arguments, rebuild.

    python3 tools/kf_embed_lua.py --check
        Regenerate every header into a temp directory and diff each one
        against what is checked in. Exits 1 and names the mismatch (plus
        the command to fix it) if anything differs. What lua_embed_check
        runs.

    python3 tools/kf_embed_lua.py SCRIPT.lua OUTPUT.h --name NAME
        One-off generation for a script not yet listed in SCRIPTS below --
        NAME becomes the header guard, the variable names (jQuery-cased:
        "demo_creature" -> kKfLuaDemoCreatureScriptSource), and the Lua
        chunk name ("=NAME").
"""

import argparse
import filecmp
import pathlib
import sys
import tempfile

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent

# Every script the build embeds, in one place, so `python3
# tools/kf_embed_lua.py` with no arguments regenerates all of them rather
# than requiring a caller to remember each (script, header, name) triple
# by hand -- exactly the "never meet the generator" bar Task 1's brief sets
# for this tool. Add a new (lua path, header path, name) tuple here when a
# new script earns a checked-in header; kf_lua_pet_proof_script.h and
# kf_lua_proof_script.h are deliberately NOT here -- they are test
# fixtures, not game code, and Task 1 leaves them as C++ headers on
# purpose (see the plan's Task 1 requirements).
SCRIPTS = [
    (
        "examples/creature_demo/creature.lua",
        "sdk/lua/generated/kf_lua_demo_creature_script.h",
        "demo_creature",
    ),
]


class EmbedError(Exception):
    """A .lua source file cannot be safely embedded as-is."""


def pascal_case(name: str) -> str:
    """"demo_creature" -> "DemoCreature", matching the variable names the
    hand-written header this tool replaces already used."""
    return "".join(word.capitalize() for word in name.split("_") if word)


def render_header(lua_source: str, name: str, lua_rel_path: str) -> str:
    """Builds the header text for one script. `lua_rel_path` is only used
    in comments/error messages, so callers can pass whatever path they
    want a human to see."""
    for lineno, line in enumerate(lua_source.splitlines(), start=1):
        if ')lua"' in line:
            raise EmbedError(
                f'{lua_rel_path}:{lineno}: contains the sequence )lua", '
                'which would close the generated R"lua(...)lua" raw string '
                "literal early and produce a C++ compile error a hundred "
                "lines away from this one. Rename or rephrase this line."
            )

    guard = f"KF_LUA_{name.upper()}_SCRIPT_H"
    pascal = pascal_case(name)
    source_var = f"kKfLua{pascal}ScriptSource"
    chunk_var = f"kKfLua{pascal}ScriptChunkName"
    chunk_name = f"={name}"

    lines = [
        "/* SPDX-License-Identifier: Apache-2.0",
        " * Copyright the Kamiframe contributors.",
        " *",
        " * GENERATED FILE -- do not edit by hand. Regenerate with:",
        " *     python3 tools/kf_embed_lua.py",
        f" * from {lua_rel_path}. lua_embed_check",
        " * (simulator/CMakeLists.txt) regenerates every embedded header into a",
        " * temp directory and diffs it against what is checked in here on every",
        " * `ctest` run, so a hand edit -- or a stale header after the .lua source",
        " * changed -- fails the build instead of silently drifting.",
        " */",
        "",
        f"#ifndef {guard}",
        f"#define {guard}",
        "",
        f'inline constexpr const char *{source_var} = R"lua(',
        lua_source.rstrip("\n"),
        ')lua";',
        "",
        "/* Leading '=' is Lua's own convention for \"show this name verbatim in",
        ' * error messages" -- see kf_lua_proof_script.h\'s identical comment on its',
        " * own chunk name for why, unchanged here. */",
        f"inline constexpr const char *{chunk_var} =",
        f'    "{chunk_name}";',
        "",
        f"#endif /* {guard} */",
        "",
    ]
    return "\n".join(lines)


def generate_one(
    lua_path: pathlib.Path, header_path: pathlib.Path, name: str, repo_root: pathlib.Path
) -> None:
    try:
        lua_rel_path = lua_path.relative_to(repo_root).as_posix()
    except ValueError:
        lua_rel_path = str(lua_path)
    # newline="" on the read and newline="\n" on the write, both deliberate,
    # because this generator's output is BYTE-compared by cmd_check() below.
    #
    # Read: newline="" disables universal-newline translation, so a .lua file
    # is embedded exactly as it sits on disk. The repo pins every text file to
    # LF (.gitattributes `* text=auto eol=lf`), so that is what this sees on
    # every platform -- and if someone ever does check in a CRLF .lua, the
    # right outcome is that the drift test notices rather than that the two
    # platforms silently disagree about what was embedded.
    #
    # Write: newline="\n" stops Python's text mode translating \n to the
    # platform line ending. Without it, on Windows only, write_text() emits
    # CRLF while the checked-in header is LF, and filecmp's byte comparison
    # can NEVER pass -- which is exactly how this shipped: green on Linux and
    # macOS, red on the Windows CI job, with a message ("content differs from
    # its .lua source") that points at the .lua file rather than at the
    # platform. Nothing about the source had drifted at all.
    # Explicit open() rather than Path.read_text()/write_text() purely for
    # version reach: the newline= keyword arrived on write_text() in Python
    # 3.10 but not on read_text() until 3.13, so the tidier pathlib form built
    # fine on a 3.14 dev machine and died on CI with "Path.read_text() got an
    # unexpected keyword argument 'newline'". open() has taken newline= since
    # Python 3.0. Do not "simplify" this back.
    with open(lua_path, "r", encoding="utf-8", newline="") as f:
        source = f.read()
    header_text = render_header(source, name, lua_rel_path)
    header_path.parent.mkdir(parents=True, exist_ok=True)
    with open(header_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(header_text)


def cmd_regenerate(repo_root: pathlib.Path) -> int:
    for lua_rel, header_rel, name in SCRIPTS:
        lua_path = repo_root / lua_rel
        header_path = repo_root / header_rel
        generate_one(lua_path, header_path, name, repo_root)
        print(f"kf_embed_lua: wrote {header_path.relative_to(repo_root)}")
    return 0


def cmd_check(repo_root: pathlib.Path) -> int:
    """Regenerates every mapped header into a temp directory and diffs it
    against the checked-in copy. This is the drift test: it never writes
    into the working tree, so it is safe to run from ctest."""
    mismatches = []
    with tempfile.TemporaryDirectory(prefix="kf_embed_lua_check_") as tmp:
        tmp_path = pathlib.Path(tmp)
        for lua_rel, header_rel, name in SCRIPTS:
            lua_path = repo_root / lua_rel
            checked_in = repo_root / header_rel
            if not checked_in.exists():
                mismatches.append(
                    (checked_in, "does not exist -- never generated")
                )
                continue
            regenerated = tmp_path / checked_in.name
            generate_one(lua_path, regenerated, name, repo_root)
            if not filecmp.cmp(checked_in, regenerated, shallow=False):
                mismatches.append((checked_in, "content differs from its .lua source"))

    if mismatches:
        sys.stderr.write(
            "kf_embed_lua --check: the checked-in generated header(s) do not "
            "match what tools/kf_embed_lua.py produces from the .lua source "
            "today:\n"
        )
        for path, reason in mismatches:
            sys.stderr.write(f"  {path.relative_to(repo_root)}: {reason}\n")
        sys.stderr.write("Fix with: python3 tools/kf_embed_lua.py\n")
        return 1

    print("kf_embed_lua --check: every generated header matches its .lua source")
    return 0


def cmd_one_off(lua_path: pathlib.Path, header_path: pathlib.Path, name: str, repo_root: pathlib.Path) -> int:
    generate_one(lua_path, header_path, name, repo_root)
    print(f"kf_embed_lua: wrote {header_path}")
    return 0


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("lua_path", nargs="?", help="a .lua file, for one-off generation")
    parser.add_argument("header_path", nargs="?", help="where to write the generated header")
    parser.add_argument("--name", help="embed name for one-off generation, e.g. demo_creature")
    parser.add_argument(
        "--check",
        action="store_true",
        help="regenerate into a temp dir and diff against what is checked in; used by lua_embed_check",
    )
    args = parser.parse_args(argv)

    if args.check:
        if args.lua_path or args.header_path or args.name:
            parser.error("--check takes no other arguments")
        try:
            return cmd_check(REPO_ROOT)
        except EmbedError as exc:
            sys.stderr.write(f"kf_embed_lua: {exc}\n")
            return 1

    if args.lua_path or args.header_path or args.name:
        if not (args.lua_path and args.header_path and args.name):
            parser.error("one-off generation needs LUA_PATH, HEADER_PATH and --name together")
        try:
            return cmd_one_off(
                pathlib.Path(args.lua_path).resolve(),
                pathlib.Path(args.header_path).resolve(),
                args.name,
                REPO_ROOT,
            )
        except EmbedError as exc:
            sys.stderr.write(f"kf_embed_lua: {exc}\n")
            return 1

    try:
        return cmd_regenerate(REPO_ROOT)
    except EmbedError as exc:
        sys.stderr.write(f"kf_embed_lua: {exc}\n")
        return 1


if __name__ == "__main__":
    sys.exit(main())
