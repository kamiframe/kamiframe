#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright the Kamiframe contributors.
"""Numeric layout verification for kf_panel.py -- no eyes on a screen needed.

kf_panel.py had a real, observed layout bug (an enormous window with most
controls invisible or clipped off the right edge under macOS dark mode).
Fixing "a layout looks right" without ever being able to see it requires
checking geometry numbers instead of pixels: this script builds the panel
for real, lets Tk lay everything out, then walks every widget and asserts
the properties a sane layout must have -- nothing clipped, nothing off-
window, nothing invisible, and every control the user needs either
directly visible or reachable by scrolling.

TWO PASSES, NOT ONE. --demo mode (`_build_connection_controls()` in
kf_panel.py) skips the Port/Rescan/Connect row entirely -- there is
nothing to connect to in demo mode, so it never gets built. That row is
also the WIDEST row in the whole panel and the exact shape that produced
the clipping bug this script exists to catch, so a demo-only run can
never see it: an audit gutted the Connect button entirely and this
script's output did not change. Pass 2 below builds the panel against
target_kind="serial" instead (no hardware needed -- the connect attempt
just fails quickly and harmlessly, the same way it would on a laptop with
nothing plugged in), so the Connection row actually gets measured.

Usage:
    /usr/bin/python3 tools/kf_panel_layout_check.py

Must run under the system Python (/usr/bin/python3 on macOS) -- that's the
one with tkinter. The project's default `python3` may not have it; see
kf_panel.py's own error message for that same point.

Exits 0 if every check passes, non-zero (with FAIL rows and a summary) if
not.
"""

import re
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

try:
    import tkinter as tk
except ImportError:
    print(
        "error: tkinter is not available in this Python install.\n"
        "Run this with /usr/bin/python3 on macOS (or another Python build "
        "that includes tkinter), not the project's default `python3`.",
        file=sys.stderr)
    sys.exit(1)

import kf_panel as kfp  # noqa: E402


def bbox(widget):
    """(x, y, x2, y2) in screen coordinates -- winfo_root{x,y} plus size."""
    x = widget.winfo_rootx()
    y = widget.winfo_rooty()
    return x, y, x + widget.winfo_width(), y + widget.winfo_height()


def inside(inner, outer, tolerance=2):
    """True if `inner` bbox lies within `outer` bbox, allowing a couple of
    pixels of tolerance for borders/highlight rings that legitimately draw
    a pixel or two outside a widget's own packed area."""
    ix1, iy1, ix2, iy2 = inner
    ox1, oy1, ox2, oy2 = outer
    return (ix1 >= ox1 - tolerance and iy1 >= oy1 - tolerance and
            ix2 <= ox2 + tolerance and iy2 <= oy2 + tolerance)


def walk(widget):
    yield widget
    for child in widget.winfo_children():
        yield from walk(child)


def classname(widget):
    return widget.winfo_class()


def _is_descendant(widget, ancestor):
    w = widget
    while w is not None:
        if w is ancestor:
            return True
        parent_name = w.winfo_parent()
        w = w.nametowidget(parent_name) if parent_name else None
    return False


def _pump(root, predicate=None, timeout_ms=3000, interval_ms=50):
    """Run the *real* Tk event loop for a bounded time.

    Deliberately uses root.mainloop() (quit via root.quit()) rather than
    repeated root.update() calls. root.update() processes the whole
    pending-event queue in one shot and, at least in the environment this
    script was verified in, that can spin indefinitely servicing a steady
    trickle of idle-driven layout events without root.mainloop()'s normal
    wait-for-next-event behaviour ever kicking in -- root.update() was
    observed to hang indefinitely (100% CPU, no progress) on exactly this
    window, while this after()+mainloop()+quit() pattern completes in
    well under a second. Quits early once `predicate()` is true (or
    always, if no predicate), otherwise after `timeout_ms`.
    """
    deadline = time.monotonic() + timeout_ms / 1000.0

    def tick():
        if (predicate is not None and predicate()) or time.monotonic() >= deadline:
            root.quit()
            return
        root.after(interval_ms, tick)

    root.after(interval_ms, tick)
    root.mainloop()


def _requested_window_size():
    """(w, h) kf_panel.py itself requests via root.geometry("WxH") in
    _build_ui() -- parsed straight out of kf_panel.py's own source rather
    than kept as a second, independent literal here (the previous version
    of this script hardcoded "560, 900" a second time, with only a
    comment asking whoever changed kf_panel.py's call to remember to
    update this file too -- exactly the kind of thing that silently goes
    stale). Reading it back from Tk instead (root.geometry() after
    construction) was tried and rejected: Tk reports "1x1" until an idle
    cycle runs, and even after root.update_idletasks() it reports the
    NEGOTIATED size, not the requested one, if content needs less than
    900px tall -- neither is "what kf_panel.py asked for", which is the
    number this check actually needs."""
    src_path = Path(kfp.__file__)
    with open(src_path, "r", encoding="utf-8") as f:
        src = f.read()
    m = re.search(r'root\.geometry\("(\d+)x(\d+)"\)', src)
    if not m:
        raise ValueError(f'could not find root.geometry("WxH") in '
                          f'{src_path}')
    return int(m.group(1)), int(m.group(2))


def run_pass(target, label):
    """Builds the panel against `target`, runs every check, prints a
    report, and returns the list of failure strings (each already
    prefixed with `label` so a two-pass run's combined summary says which
    pass found what)."""
    root = tk.Tk()
    app = kfp.PanelApp(root, target=target, baud=115200,
                        state_interval=1.0, verbose=False)

    window_w, window_h = _requested_window_size()

    # Let whatever the connect attempt queues on the worker thread (the
    # demo device's fake round-trip, or a real port scan that will fail
    # harmlessly in this pass) settle, and let Tk finish laying everything
    # out, before measuring anything.
    if target[0] == "demo":
        _pump(root, predicate=lambda: app.connected, timeout_ms=3000)
    else:
        _pump(root, predicate=None, timeout_ms=800)
    root.update_idletasks()

    failures = []

    def fail(msg):
        failures.append(f"[{label}] {msg}")

    rows = []

    def record(widget, note=""):
        mapped = bool(widget.winfo_ismapped())
        w = widget.winfo_width()
        h = widget.winfo_height()
        x = widget.winfo_rootx()
        y = widget.winfo_rooty()
        rows.append((widget, classname(widget), mapped, x, y, w, h, note))

    for w in walk(root):
        record(w)

    # ----------------------------------------------------------------
    # 1. Every section frame and interactive control must be mapped.
    # ----------------------------------------------------------------
    for widget, cls, mapped, x, y, w, h, note in rows:
        if widget is root:
            continue
        if not mapped:
            # Not automatically a failure -- e.g. Tooltip popups, which
            # are Toplevels that only map on hover. Only flag it if it's a
            # widget we actually built as part of the always-present UI.
            if widget in _tracked_widgets(app):
                fail(f"not mapped: {cls} ({_widget_label(app, widget)})")

    # ----------------------------------------------------------------
    # 2. No mapped widget has zero width or height.
    # ----------------------------------------------------------------
    for widget, cls, mapped, x, y, w, h, note in rows:
        if widget is root or not mapped:
            continue
        if w <= 0 or h <= 0:
            fail(f"zero-sized: {cls} ({_widget_label(app, widget)}) "
                 f"w={w} h={h}")

    # ----------------------------------------------------------------
    # 3. Every widget's bbox lies inside its parent's bbox -- except the
    #    one deliberate exception: the scrollable inner frame is allowed
    #    to be taller than the canvas viewport that shows it (that's what
    #    makes it scrollable). Its width must still match the canvas
    #    (no horizontal overflow), and nothing may extend right of the
    #    root window.
    # ----------------------------------------------------------------
    root_box = (root.winfo_rootx(), root.winfo_rooty(),
                root.winfo_rootx() + root.winfo_width(),
                root.winfo_rooty() + root.winfo_height())

    for widget, cls, mapped, x, y, w, h, note in rows:
        if widget is root or not mapped:
            continue
        parent_name = widget.winfo_parent()
        parent = widget.nametowidget(parent_name) if parent_name else None
        wbox = (x, y, x + w, y + h)

        # No widget may extend right of the window -- this is the exact
        # symptom that was observed ("fragments of text clipped at the
        # extreme right").
        if wbox[2] > root_box[2] + 2:
            fail(f"extends past right edge of window: {cls} "
                 f"({_widget_label(app, widget)}) right={wbox[2]} "
                 f"window right={root_box[2]}")
        if wbox[0] < root_box[0] - 2:
            fail(f"extends past left edge of window: {cls} "
                 f"({_widget_label(app, widget)})")

        if parent is None or parent is root:
            continue

        if widget is app.scroll_frame:
            # The one exception: allowed to be taller than the canvas.
            # Must not be wider than it, though.
            pbox = bbox(parent)
            if wbox[2] > pbox[2] + 2:
                fail("scrollable content frame is wider than the canvas "
                     "-- would need horizontal scrolling")
            continue

        # Anything living inside the scrollable content frame (at any
        # depth) is compared against the actual root window bounds
        # already above; comparing it against its *immediate* parent
        # would be redundant with that and with Tk's own pack/grid
        # containment guarantees, so just double check it doesn't escape
        # its immediate parent sideways/upward in a way pack wouldn't
        # normally allow (defensive; would only fire from a `place()`
        # misuse).
        pbox = bbox(parent)
        if not inside(wbox, pbox, tolerance=4):
            fail(f"escapes its parent's bounds: {cls} "
                 f"({_widget_label(app, widget)}) widget={wbox} "
                 f"parent={pbox} parent_class={classname(parent)}")

    # ----------------------------------------------------------------
    # 4. Every control the user must be able to reach is either inside
    #    the window bounds right now, or provably reachable by scrolling
    #    (its bbox, relative to the scrollable content frame, lies within
    #    the canvas's scrollregion).
    #
    #    scrollregion is NOT trusted blindly here: it is set by kf_panel.py
    #    from canvas.bbox("all"), which is itself derived from the very
    #    widgets being checked -- comparing a widget's offset inside
    #    scroll_frame against a scrollregion computed from scroll_frame's
    #    own children is one source checked against itself, and cannot
    #    catch a scrollregion that is stale or simply wrong (e.g. an
    #    earlier <Configure> firing before the frame's final height was
    #    known). So the first thing this section does is compute the
    #    real, observed bounding box of every mapped descendant of
    #    scroll_frame directly from their own winfo_root{x,y}/width/height
    #    -- a second, independent measurement -- and fails loudly if
    #    scrollregion does not actually cover it.
    # ----------------------------------------------------------------
    canvas = app.canvas
    scrollregion = canvas.cget("scrollregion")
    if not scrollregion:
        fail("canvas has no scrollregion set at all")
        sr = (0, 0, 0, 0)
    else:
        sr = tuple(float(v) for v in scrollregion.split())

    frame_x0 = app.scroll_frame.winfo_rootx()
    frame_y0 = app.scroll_frame.winfo_rooty()
    observed_x2 = 0.0
    observed_y2 = 0.0
    for widget, cls, mapped, x, y, w, h, note in rows:
        if not mapped or not _is_descendant(widget, app.scroll_frame):
            continue
        observed_x2 = max(observed_x2, (x - frame_x0) + w)
        observed_y2 = max(observed_y2, (y - frame_y0) + h)
    if observed_y2 > sr[3] + 4:
        fail(f"scrollregion is shorter than its own content: scrollregion "
             f"bottom={sr[3]} but content actually extends to "
             f"y={observed_y2} -- some of it would be unreachable even "
             f"at full scroll")
    if observed_x2 > sr[2] + 4:
        fail(f"scrollregion is narrower than its own content: "
             f"scrollregion right={sr[2]} but content actually extends "
             f"to x={observed_x2}")

    must_reach = _required_controls(app)
    for label_, widget in must_reach.items():
        if widget is None:
            fail(f"required control missing entirely: {label_}")
            continue
        if not widget.winfo_ismapped():
            fail(f"required control not mapped: {label_}")
            continue
        if _is_descendant(widget, app.scroll_frame):
            # Position relative to the scrollable frame's own coordinate
            # space (canvas-content coordinates), which is what
            # scrollregion is expressed in -- reachable by scrolling
            # counts as satisfying the requirement. scrollregion's own
            # validity against real content was already checked above.
            cx = widget.winfo_rootx() - frame_x0
            cy = widget.winfo_rooty() - frame_y0
            cx2 = cx + widget.winfo_width()
            cy2 = cy + widget.winfo_height()
            reachable = (cx >= sr[0] - 4 and cy >= sr[1] - 4 and
                         cx2 <= sr[2] + 4 and cy2 <= sr[3] + 4)
            if not reachable:
                fail(f"required control not within scrollregion: {label_} "
                     f"widget=({cx},{cy},{cx2},{cy2}) scrollregion={sr}")
        else:
            # Outside the scroll area (the status label) -- must be
            # visible in the window right now, not just "reachable".
            wbox = bbox(widget)
            if not inside(wbox, root_box, tolerance=4):
                fail(f"required control (outside scroll area) is not "
                     f"within the window: {label_} widget={wbox} "
                     f"window={root_box}")

    # ----------------------------------------------------------------
    # 5. The window's actual size must not exceed the geometry set.
    #    window_w/window_h above were read back from root.geometry()
    #    itself -- not a second copy of kf_panel.py's "560x900" literal --
    #    so this can never silently drift out of sync with what
    #    kf_panel.py actually requests.
    # ----------------------------------------------------------------
    actual_w = root.winfo_width()
    actual_h = root.winfo_height()
    if actual_w > window_w:
        fail(f"window wider than the geometry set: {actual_w} > {window_w}")
    if actual_h > window_h:
        fail(f"window taller than the geometry set: {actual_h} > {window_h}")

    min_w, min_h = root.wm_minsize()
    if min_w <= 0 or min_h <= 0:
        fail(f"minsize not set (got {min_w}x{min_h})")

    # ----------------------------------------------------------------
    # Print a readable table.
    # ----------------------------------------------------------------
    print(f"=== Pass: {label} (target={target}) ===")
    print(f"Window: requested {window_w}x{window_h}, actual "
          f"{actual_w}x{actual_h}, minsize {min_w}x{min_h}")
    print(f"Scrollregion: {sr}  (observed content bbox: "
          f"(0, 0, {observed_x2:g}, {observed_y2:g}))")
    print(f"Connected: {app.connected}")
    print()
    header = f"{'class':<16} {'mapped':<7} {'x':>5} {'y':>5} {'w':>5} " \
             f"{'h':>5}  label"
    print(header)
    print("-" * len(header))
    for widget, cls, mapped, x, y, w, h, note in rows:
        if widget is root:
            widget_label = "(root window)"
        else:
            widget_label = _widget_label(app, widget) or ""
        if not widget_label and cls in ("Frame", "TFrame"):
            continue  # anonymous layout frames -- not interesting to list
        print(f"{cls:<16} {str(mapped):<7} {x:>5} {y:>5} {w:>5} {h:>5}  "
              f"{widget_label}")

    print()
    print("Required controls:")
    for label_, widget in must_reach.items():
        ok = widget is not None and widget.winfo_ismapped()
        print(f"  [{'ok' if ok else 'FAIL'}] {label_}")

    print()
    if failures:
        print(f"FAILED ({label}): {len(failures)} problem(s) found")
        for f in failures:
            print(f"  - {f}")
    else:
        print(f"PASSED ({label}): all layout checks ok")
    print()

    root.destroy()
    return failures


def main():
    all_failures = []
    # Pass 1: --demo mode. Fast (the fake device connects immediately),
    # and covers the button pad / screenshot / state / time controls the
    # same way regardless of connection kind.
    all_failures += run_pass(("demo", None), "demo")
    # Pass 2: a real (non-demo) target, so _build_connection_controls()
    # actually builds the Port/Rescan/Connect row -- see this file's
    # header comment for why --demo alone can never catch a bug in that
    # row. "auto" with no hardware plugged in fails the connect attempt
    # harmlessly and quickly; the layout is already final before that
    # attempt even resolves.
    all_failures += run_pass(("serial", "auto"), "serial (no hardware)")

    if all_failures:
        print(f"OVERALL FAILED: {len(all_failures)} problem(s) across both "
              f"passes")
        for f in all_failures:
            print(f"  - {f}")
        return 1

    print("OVERALL PASSED: all layout checks ok in both passes")
    return 0


def _widget_label(app, widget):
    """Best-effort human label for a widget, for readable output --
    matches it up against the named attributes PanelApp keeps."""
    for name, value in vars(app).items():
        if value is widget:
            return name
    if isinstance(widget, dict):
        return None
    # pad_buttons / time_control_buttons are collections
    for name in ("pad_buttons",):
        coll = getattr(app, name, None)
        if isinstance(coll, dict):
            for key, val in coll.items():
                if val is widget:
                    return f"{name}[{key}]"
    for name in ("time_control_buttons",):
        coll = getattr(app, name, None)
        if isinstance(coll, list):
            for i, val in enumerate(coll):
                if val is widget:
                    return f"{name}[{i}] ({val.cget('text')})"
    try:
        text = widget.cget("text")
        if text:
            return f"<{widget.winfo_class()} text={text!r}>"
    except Exception:
        pass
    return None


def _tracked_widgets(app):
    """Widgets this script actually cares about the mapped-state of --
    named attributes plus collections, not every anonymous layout Frame."""
    out = set()
    for name, value in vars(app).items():
        if isinstance(value, tk.Widget):
            out.add(value)
    for coll_name in ("pad_buttons",):
        coll = getattr(app, coll_name, None)
        if isinstance(coll, dict):
            out.update(coll.values())
    for coll_name in ("time_control_buttons",):
        coll = getattr(app, coll_name, None)
        if isinstance(coll, list):
            out.update(coll)
    return out


def _required_controls(app):
    """Every control the user must be able to reach, by plain-English
    label -- the actual assertion target for requirement #4.

    connect_btn/port_combo only exist for a non-demo target
    (kf_panel.py's _build_connection_controls() returns early in --demo
    mode). getattr(app, "connect_btn", None) is used rather than a plain
    hasattr() guard that quietly OMITS the key when the attribute is
    missing: omitting it is exactly the bug that let the Connect button
    ship broken once (a demo-only test run never even asked the
    question). For a non-demo target these two are load-bearing --
    passing None through means the caller's existing "required control
    missing entirely" check (run_pass()'s `if widget is None: fail(...)`)
    actually fires instead of silently skipping them. For a demo target
    they are legitimately absent by design, so they are left out of the
    dict entirely there (asking the question and getting a real "missing"
    failure would be wrong for demo mode -- kf_panel.py's own choice, not
    a bug)."""
    out = {}
    if app.target_kind != "demo":
        out["Connect/Disconnect button"] = getattr(app, "connect_btn", None)
        out["Port combobox"] = getattr(app, "port_combo", None)
    for name, widget in app.pad_buttons.items():
        out[f"Button pad: {name}"] = widget
    out["Save Screenshot button"] = app.shot_btn
    for i, widget in enumerate(app.time_control_buttons):
        out[f"Time control: {widget.cget('text')}"] = widget
    out["Status label"] = app.status_label
    return out


if __name__ == "__main__":
    sys.exit(main())
