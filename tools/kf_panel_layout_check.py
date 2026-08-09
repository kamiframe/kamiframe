#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright the Kamiframe contributors.
"""Numeric layout verification for kf_panel.py -- no eyes on a screen needed.

kf_panel.py had a real, observed layout bug (an enormous window with most
controls invisible or clipped off the right edge under macOS dark mode).
Fixing "a layout looks right" without ever being able to see it requires
checking geometry numbers instead of pixels: this script builds the panel
in --demo mode, lets Tk lay everything out for real, then walks every
widget and asserts the properties a sane layout must have -- nothing
clipped, nothing off-window, nothing invisible, and every control the user
needs either directly visible or reachable by scrolling.

Usage:
    /usr/bin/python3 tools/kf_panel_layout_check.py

Must run under the system Python (/usr/bin/python3 on macOS) -- that's the
one with tkinter. The project's default `python3` may not have it; see
kf_panel.py's own error message for that same point.

Exits 0 if every check passes, non-zero (with FAIL rows and a summary) if
not.
"""

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


WINDOW_W, WINDOW_H = 560, 900  # must match the geometry() kf_panel.py sets

FAILURES = []


def fail(msg):
    FAILURES.append(msg)


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


def main():
    root = tk.Tk()
    app = kfp.PanelApp(root, target=("demo", None), baud=115200,
                        state_interval=1.0, verbose=False)

    # Let the demo device's fake "connect" round-trip (it's queued on the
    # worker thread and applied via root.after()), and let Tk finish
    # laying everything out, before measuring anything.
    _pump(root, predicate=lambda: app.connected, timeout_ms=3000)
    root.update_idletasks()

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
    # ----------------------------------------------------------------
    canvas = app.canvas
    scrollregion = canvas.cget("scrollregion")
    if not scrollregion:
        fail("canvas has no scrollregion set at all")
        sr = (0, 0, 0, 0)
    else:
        sr = tuple(float(v) for v in scrollregion.split())

    must_reach = _required_controls(app)
    for label, widget in must_reach.items():
        if widget is None:
            fail(f"required control missing entirely: {label}")
            continue
        if not widget.winfo_ismapped():
            fail(f"required control not mapped: {label}")
            continue
        if _is_descendant(widget, app.scroll_frame):
            # Position relative to the scrollable frame's own coordinate
            # space (canvas-content coordinates), which is what
            # scrollregion is expressed in -- reachable by scrolling
            # counts as satisfying the requirement.
            cx = widget.winfo_rootx() - app.scroll_frame.winfo_rootx()
            cy = widget.winfo_rooty() - app.scroll_frame.winfo_rooty()
            cx2 = cx + widget.winfo_width()
            cy2 = cy + widget.winfo_height()
            reachable = (cx >= sr[0] - 4 and cy >= sr[1] - 4 and
                         cx2 <= sr[2] + 4 and cy2 <= sr[3] + 4)
            if not reachable:
                fail(f"required control not within scrollregion: {label} "
                     f"widget=({cx},{cy},{cx2},{cy2}) scrollregion={sr}")
        else:
            # Outside the scroll area (the status label) -- must be
            # visible in the window right now, not just "reachable".
            wbox = bbox(widget)
            if not inside(wbox, root_box, tolerance=4):
                fail(f"required control (outside scroll area) is not "
                     f"within the window: {label} widget={wbox} "
                     f"window={root_box}")

    # ----------------------------------------------------------------
    # 5. The window's actual size must not exceed the geometry set.
    # ----------------------------------------------------------------
    actual_w = root.winfo_width()
    actual_h = root.winfo_height()
    if actual_w > WINDOW_W:
        fail(f"window wider than the geometry set: {actual_w} > {WINDOW_W}")
    if actual_h > WINDOW_H:
        fail(f"window taller than the geometry set: {actual_h} > {WINDOW_H}")

    min_w, min_h = root.wm_minsize()
    if min_w <= 0 or min_h <= 0:
        fail(f"minsize not set (got {min_w}x{min_h})")

    # ----------------------------------------------------------------
    # Print a readable table.
    # ----------------------------------------------------------------
    print(f"Window: requested {WINDOW_W}x{WINDOW_H}, actual "
          f"{actual_w}x{actual_h}, minsize {min_w}x{min_h}")
    print(f"Scrollregion: {sr}")
    print(f"Demo device connected: {app.connected}")
    print()
    header = f"{'class':<16} {'mapped':<7} {'x':>5} {'y':>5} {'w':>5} " \
             f"{'h':>5}  label"
    print(header)
    print("-" * len(header))
    for widget, cls, mapped, x, y, w, h, note in rows:
        if widget is root:
            label = "(root window)"
        else:
            label = _widget_label(app, widget) or ""
        if not label and cls in ("Frame", "TFrame"):
            continue  # anonymous layout frames -- not interesting to list
        print(f"{cls:<16} {str(mapped):<7} {x:>5} {y:>5} {w:>5} {h:>5}  {label}")

    print()
    print("Required controls:")
    for label, widget in must_reach.items():
        ok = widget is not None and widget.winfo_ismapped()
        print(f"  [{'ok' if ok else 'FAIL'}] {label}")

    print()
    if FAILURES:
        print(f"FAILED: {len(FAILURES)} problem(s) found")
        for f in FAILURES:
            print(f"  - {f}")
        root.destroy()
        return 1

    print("PASSED: all layout checks ok")
    root.destroy()
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
    label -- the actual assertion target for requirement #4."""
    out = {}
    if hasattr(app, "connect_btn"):
        out["Connect/Disconnect button"] = app.connect_btn
    for name, widget in app.pad_buttons.items():
        out[f"Button pad: {name}"] = widget
    out["Save Screenshot button"] = app.shot_btn
    for i, widget in enumerate(app.time_control_buttons):
        out[f"Time control: {widget.cget('text')}"] = widget
    out["Status label"] = app.status_label
    return out


if __name__ == "__main__":
    sys.exit(main())
