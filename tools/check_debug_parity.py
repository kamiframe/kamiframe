#!/usr/bin/env python3
"""Fail the build when the desktop debug window and the KFDBG serial bridge
stop offering the same set of debug actions.

WHY A SOURCE-LEVEL CHECKER RATHER THAN A TEST.

The obvious way to check this would be a C++ test that enumerates both
dispatchers. It cannot: ports/esp32/main/kf_dbg_bridge.cpp is device-only code
that the desktop test binary does not link, and never will -- it talks to a
UART. So the only place that can see BOTH sides at once is a tool that reads
the source, which is the same shape as check_no_heap.py and check_no_float.py
and is wired into CI beside them.

WHAT IT ENFORCES.

Three sets, which must partition cleanly:

  TABLE       verbs in simulator/src/pet/kf_debug_actions.cpp's kActions --
              the portable debug actions both sides dispatch through.
  EXCEPTIONS  verbs declared in that same file as either device-only
              (kDeviceOnlyVerbs: they read hardware a desktop lacks) or
              parity-by-other-means (kOtherMeansVerbs).
  BRIDGE      verbs kf_dbg_bridge.cpp still handles with its own branch.
  DESKTOP     verbs sdl_debug_window.cpp reaches via run_shared*().
  HOSTTOOL    verbs tools/kf_debug.py actually sends down the wire.

THERE ARE THREE SURFACES, NOT TWO. The first version of this checker had only
the first two and passed clean while `python3 tools/kf_debug.py save` did not
exist -- the firmware and the desktop window agreed with each other, and the
tool a human actually uses to drive the firmware was left behind. Nobody types
raw KFDBG verbs down a serial line, so a verb with no subcommand here is, in
practice, a verb that does not exist. Rule 5 below is that lesson.

The rules:

  1. Every BRIDGE branch verb must be an EXCEPTION. A new KFDBG verb with its
     own branch and no declaration is the exact failure this exists to catch:
     a debug capability that exists on the device and silently does not exist
     on the desktop.
  2. No BRIDGE branch verb may also be in TABLE -- that would mean the branch
     shadows the table and the two could diverge in behaviour.
  3. Every TABLE verb must be reachable from DESKTOP. A table entry no button
     invokes is a device-only feature wearing a portable costume.
  4. Every DESKTOP verb must be in TABLE, which catches a typo'd verb string
     in a button (a dead button that logs at runtime and nowhere else).
  5. HOSTTOOL must reach every verb the firmware understands -- TABLE plus the
     device-only ones, since those have no desktop button by definition -- and
     must not send a verb no firmware branch handles.

WHAT IT DOES NOT ENFORCE, on purpose: that the exception lists are morally
correct. Someone can silence this by adding a verb to kOtherMeansVerbs with no
justification. That is fine -- the point is not to make the wrong thing
impossible, it is to make it a deliberate, reviewable edit in a file whose
whole header explains what those lists mean, rather than an omission nobody
sees. The four device-only verbs each carry their reasoning there.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

ACTIONS = ROOT / "simulator" / "src" / "pet" / "kf_debug_actions.cpp"
BRIDGE = ROOT / "ports" / "esp32" / "main" / "kf_dbg_bridge.cpp"
DESKTOP = ROOT / "simulator" / "src" / "sdl" / "sdl_debug_window.cpp"
HOSTTOOL = ROOT / "tools" / "kf_debug.py"


def read(path):
    if not path.is_file():
        sys.exit(f"check_debug_parity: missing {path.relative_to(ROOT)}")
    return path.read_text(encoding="utf-8")


def block(text, marker, path):
    """The braced initialiser that follows `marker`, as raw text.

    Deliberately anchored on the declaration rather than scanning the whole
    file: kf_debug_actions.cpp's header comment names several of these verbs
    in prose, and harvesting quoted uppercase words from anywhere in the file
    would happily match those and pass whatever happened to be written down.
    That is precisely how an earlier checker on this project managed to pass
    while testing nothing.
    """
    start = text.find(marker)
    if start < 0:
        sys.exit(f"check_debug_parity: {path.name} has no '{marker}' -- the "
                 f"checker cannot see the table it is meant to check, which "
                 f"is a failure, not a pass")
    open_brace = text.find("{", start)
    close_brace = text.find("};", open_brace)
    if open_brace < 0 or close_brace < 0:
        sys.exit(f"check_debug_parity: could not find the initialiser body "
                 f"for '{marker}' in {path.name}")
    return text[open_brace:close_brace]


def verbs_in(body):
    """Every "UPPERCASE" string literal in a table body, in order."""
    return [m for m in re.findall(r'"([A-Z][A-Z0-9_]*)"', body)]


def main():
    actions_src = read(ACTIONS)
    bridge_src = read(BRIDGE)
    desktop_src = read(DESKTOP)

    # kActions holds {"VERB", KIND, mutates, "summary", handler} -- the verb
    # is the first string of each entry and the summary is the second, so
    # take only the verb-shaped ones. Summaries are lowercase prose, which is
    # why the pattern above requires a leading uppercase letter.
    table = verbs_in(block(actions_src, "kActions[]", ACTIONS))
    device_only = verbs_in(block(actions_src, "kDeviceOnlyVerbs[]", ACTIONS))
    other_means = verbs_in(block(actions_src, "kOtherMeansVerbs[]", ACTIONS))

    bridge = sorted(set(re.findall(r'strcmp\(tok1,\s*"([A-Z][A-Z0-9_]*)"\)',
                                   bridge_src)))
    desktop = sorted(set(re.findall(r'run_shared[a-z0-9_]*\(\s*"([A-Z][A-Z0-9_]*)"',
                                    desktop_src)))
    # THE THIRD SURFACE. tools/kf_debug.py is how a human actually drives
    # KFDBG -- nobody types raw verbs down a serial line -- so a verb the
    # firmware understands and this tool has no subcommand for is, in
    # practice, a verb that does not exist. This was added the same day the
    # rest of this checker was, after the checker passed clean while
    # `kf_debug.py save` did not exist: two surfaces in agreement and the one
    # people use left behind. Matched on the KFDBG string each cmd_* sends,
    # not on the argparse subcommand name, because the wire verb is the thing
    # that has to line up with the table.
    hosttool = sorted(set(re.findall(r'"KFDBG ([A-Z][A-Z0-9_]*)',
                                     read(HOSTTOOL))))

    if not table:
        sys.exit("check_debug_parity: kActions parsed as empty -- refusing to "
                 "report success on a table this checker cannot read")
    if not bridge:
        sys.exit("check_debug_parity: found no KFDBG verb branches at all -- "
                 "refusing to report success on a file this checker cannot "
                 "read")

    exceptions = set(device_only) | set(other_means)
    problems = []

    for verb in bridge:
        if verb in table:
            problems.append(
                f"KFDBG {verb} has its own branch in kf_dbg_bridge.cpp AND is "
                f"in kf_debug_actions.cpp's table. The branch shadows the "
                f"table, so the two can drift in behaviour. Delete the branch "
                f"and let dispatch_shared_action() handle it.")
        elif verb not in exceptions:
            problems.append(
                f"KFDBG {verb} exists on the device and nowhere else. Either "
                f"add it to kActions in kf_debug_actions.cpp so the desktop "
                f"gets it too, or -- if it genuinely cannot exist on a "
                f"desktop -- declare it in kDeviceOnlyVerbs or "
                f"kOtherMeansVerbs there, with a reason.")

    for verb in table:
        if verb not in desktop:
            problems.append(
                f"{verb} is in kf_debug_actions.cpp's table but no desktop "
                f"button invokes it (no run_shared(\"{verb}\") in "
                f"sdl_debug_window.cpp). A portable action the desktop cannot "
                f"reach is a device-only action in disguise.")

    for verb in desktop:
        if verb not in table:
            problems.append(
                f"sdl_debug_window.cpp calls run_shared(\"{verb}\") but no "
                f"such verb is in kf_debug_actions.cpp's table. That button "
                f"is dead -- it logs an error at runtime and does nothing.")

    # Rule 5: the host tool reaches every verb the firmware understands.
    # Both the table verbs and the declared device-only ones -- RTC, SCANLINE
    # and VSYNC are exactly the sort of thing you need a host subcommand for,
    # since the desktop has no button for them by definition.
    reachable_on_device = set(table) | set(device_only)
    for verb in sorted(reachable_on_device):
        if verb not in hosttool:
            problems.append(
                f"the firmware understands KFDBG {verb} but tools/kf_debug.py "
                f"has no subcommand that sends it. Nobody types raw verbs "
                f"down a serial line, so in practice that verb does not "
                f"exist. Add a cmd_* function and its sub.add_parser() entry.")

    for verb in hosttool:
        if verb not in reachable_on_device and verb not in other_means:
            problems.append(
                f"tools/kf_debug.py sends KFDBG {verb}, which no firmware "
                f"branch handles and which is not in the table. That "
                f"subcommand gets an `err` reply.")

    overlap = set(device_only) & set(other_means)
    if overlap:
        problems.append(
            f"declared twice, as both device-only and parity-by-other-means: "
            f"{', '.join(sorted(overlap))}")

    if problems:
        print("check_debug_parity: the debug surfaces have drifted apart\n",
              file=sys.stderr)
        for p in problems:
            print(f"  - {p}\n", file=sys.stderr)
        return 1

    print(f"check_debug_parity: {len(table)} shared debug actions, "
          f"{len(bridge)} device-side branches all declared "
          f"({len(device_only)} device-only, {len(other_means)} by other "
          f"means); desktop reaches every shared action, kf_debug.py reaches "
          f"all {len(reachable_on_device)} the firmware understands")
    return 0


if __name__ == "__main__":
    sys.exit(main())
