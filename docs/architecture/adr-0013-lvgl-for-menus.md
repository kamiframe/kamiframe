# ADR 0013: LVGL for menus, the custom engine for the pet

**Status:** Accepted, 2026-08-04
**Reversal cost:** Medium-high once menus are actually built against it, low right
now. Nothing in core depends on LVGL yet; this ADR is the decision plus the
plumbing that makes adopting it possible, not a menu system.

## Requirement

ADR 0010 and ADR 0011 already left the LVGL-versus-custom question open on
purpose (`docs/architecture/README.md`'s "open evaluations" list), deferred
until there was text on screen and a save file to evaluate against. Both
exist now. The planning notes named the two live options plainly: adopt an
existing, mature embedded GUI library, or keep extending the hand-rolled
engine this project has used since slice one.

The deciding conversation happened with the project owner directly rather
than as a solo evaluation, and it surfaced a sharper question than "which
library": not everything a virtual pet needs is the same *kind* of thing. A
hunger bar and a settings menu are exactly what a general-purpose UI library
is built for. A heart that floats up and fades when the pet is petted, a
pet's sprite dissolving from one growth stage into the next, layered
costume sprites -- these are closer to game effects than UI, and a
widget-tree library that creates and manages persistent on-screen objects is
not shaped for spawning and discarding a lot of small transient ones. That
distinction, not a single up-or-down vote on LVGL, is the actual decision.

## Decision: hybrid, split by what each half is actually good at

**LVGL owns menus, stat displays and settings.** Everything screen-based
that is naturally a persistent object being updated -- a hunger meter, a
button-driven settings list, a status screen -- goes through LVGL. This is
exactly its design center: labels, bars, buttons, simple layout, all
well-proven.

**The existing custom engine (`kf/blit.h`, `kf/font.h`, the dirty-rect list)
owns the pet itself** -- sprite compositing, growth-stage transitions,
transient effects, anything that is closer to an animated character than a
UI control. This is not new work invented for this ADR; it is exactly what
was already built and proven across ADR 0010 and ADR 0011, doing what it
was already doing.

Two research passes (web research against LVGL's own documentation and
source, and Espressif's own ESP32 integration materials, not assumption)
answered the questions that would have made this a bad trade if they'd come
back differently:

**Memory is not a real constraint on this hardware.** LVGL's own default
configuration reserves 64KB, and real-world deployments have reported
needing 80-140KB. Weighed against the roughly 5MB of PSRAM this project's
existing arenas leave unclaimed (`KF_POOL_PSRAM_BYTES` is 8MB; the Lua and
assets arenas together claim 3MB), even the pessimistic end of that range is
under 3% of what's free. The one number that does matter for performance is
the small buffer holding the pixels actively being sent to the panel (a few
KB to a few tens of KB) -- that one belongs in fast internal SRAM, same as
the main framebuffer, and Espressif's own tooling and forum precedent both
treat it that way. LVGL's larger object/style heap is not on that hot path
(it's touched a handful of times per redraw, not per pixel) and placing it
in PSRAM is a real, working, community-documented pattern, just not the
out-of-the-box default -- confirmed via LVGL's own allocator hooks
(`LV_MEM_ADR` / a custom pool-alloc macro), not guessed at.

**LVGL's own dirty-tracking is close enough to this project's that bridging
is a thin adapter, not a rewrite.** LVGL tracks up to a configurable number
of independently-mergeable invalid rectangles internally and calls a flush
callback once per unjoined rectangle -- conceptually the same shape as
`kf_fb_mark_dirty`'s merge-on-touch-or-overlap list from ADR 0011. The plan
is for LVGL's flush callback to call `kf_fb_mark_dirty()` once per
rectangle it's handed, so LVGL's own redraw tracking feeds the same
transfer-cost estimate everything else already reports through, rather than
existing as a second, disconnected accounting system the budget HUD can't
see.

**Trimming the widget set matters for readability, not memory.** The
"curate a small recommended feature set, document it, leave the rest
switched off" idea raised in the deciding conversation is genuinely the
right lever -- but for keeping the API surface approachable for a casual
contributor, not for shrinking RAM use, which the numbers above already
settle. LVGL's widget selection is a straightforward compile-time flag per
widget type (`LV_USE_LABEL`, `LV_USE_BAR`, and so on); anything switched off
is not compiled in and cannot appear in the docs or the API a new
contributor sees.

## What this slice actually builds

This ADR is the decision plus the mechanism, not a menu. Concretely:

- LVGL fetched as a pinned dependency via CMake, the same
  `FetchContent`-and-configure pattern `cmake/fetch_sdl.cmake` already
  established for SDL3.
- A curated `lv_conf.h`: only `lv_obj`, `lv_label`, `lv_image`, `lv_button`
  and `lv_bar` enabled to start -- deliberately not the widget catalog's
  full 37+ types. Extending this list later is a one-line flag change, not
  a redesign.
- A new arena, `KF_ARENA_LVGL`, carved from the PSRAM pool alongside the
  existing Lua and assets arenas, sized generously against the real-world
  80-140KB figures above rather than the bare-minimum documented one --
  same "measure honestly, don't cut it close" reasoning `kf/budget.h`
  already applies everywhere else. LVGL's allocator is pointed at this
  arena and nowhere else; it never touches the system heap, so it does not
  violate ADR 0008.
- Six small files under `simulator/src/lvgl/`, not `hakoniwaos/` --
  deliberate, not an oversight. This slice does not claim LVGL works on the
  ESP32 build; only the desktop and headless backends link it, the same
  "empty is more honest than untested" reasoning `ports/esp32/README.md`
  already uses. `kf_lvgl_pool` hands LVGL its memory pool via
  `kf_arena_alloc(KF_ARENA_LVGL, ...)`. `kf_lvgl_tick` advances LVGL's clock
  -- real elapsed time for SDL, a synthetic fixed step for headless, the
  same split ADR 0011 established for button debounce and for the same
  reason. `kf_lvgl_display` bridges LVGL's flush callback into
  `kf_fb_pixels()`/`kf_fb_mark_dirty()`. `kf_lvgl_input` is a keypad
  `lv_indev_t` reading `kf_app_buttons_held()`/`kf_app_buttons_pressed()` --
  two small new accessors on `kf/app.h` exposing core's existing debounced
  button state read-only, not a second debouncer (`kf/hal/input.h`'s rule:
  backends report raw state, core debounces once). `kf_lvgl_port` ties the
  above together plus LVGL's own log output, routed through `kf_log` like
  everything else. `kf_lvgl_proof_screen` is the trivial screen itself.
- A headless `--verify-lvgl` mode and a `lvgl_determinism_check` ctest
  target, bypassing `kf_app_init()`/`kf_app_frame()` entirely -- this
  exercises LVGL directly, the same way `--verify-storage-power` exercises
  storage/power directly, and shares no state with the golden-frame tests
  that guard the custom engine.

## Verified

- Clean `-Werror` rebuild (`-Wall -Wextra -Wshadow -Wconversion
  -Wsign-conversion -Wcast-qual -Wdouble-promotion -Werror`), everything:
  `hakoniwaos`, both desktop backends, and `kamiframe-sim` (SDL). LVGL's own
  vendored source is built with warnings suppressed deliberately (it is not
  code this project owns or holds to its own bar) via one line in
  `cmake/fetch_lvgl.cmake`; nothing under `simulator/` or `hakoniwaos/`
  needed a single exception.
- `tools/check_no_heap.py`: clean, unmodified. Checked, not assumed: the
  script only ever scans `hakoniwaos/src` and `hakoniwaos/include`, and
  LVGL is fetched by CMake into the build directory, never the source tree
  -- there was never vendored LVGL source for it to see. (A draft of this
  ADR assumed the script would need scoping to exclude LVGL; that was wrong,
  corrected here rather than left to be discovered later.)
- All 5 `ctest` targets, including the new `lvgl_determinism_check`, pass
  together. `--verify-lvgl` and the full suite both pass across 10 repeated
  runs under sustained artificial CPU load on all cores -- the same
  discipline, and the same category of bug (host-speed-dependent timing),
  that ADR 0011 exists to catch.
- `KF_ARENA_LVGL` in the arena report reads `262144 peak / 262144 cap
  (100%)`, confirming LVGL's pool allocator call is wired to
  `kf_arena_alloc(KF_ARENA_LVGL, ...)` and nowhere else, and that
  `LV_MEM_SIZE` and `KF_ARENA_LVGL_BYTES` genuinely can't drift apart (one
  `#include "kf/budget.h"` from `lv_conf.h`, not two numbers kept in sync by
  hand).
- A real screenshot, `kamiframe-sim` under Xvfb: the proof screen's label,
  progress bar and button all visible and rendering.

## Accepted cost

LVGL is a genuinely large dependency to pull in for five widget types. That
size cost is accepted deliberately: the alternative was rebuilding menu/bar/
button plumbing this project doesn't need to own, in a codebase whose
stated value is staying small enough to read end to end. The five widgets
in use are readable as a *thin, documented layer over* LVGL; LVGL itself is
not something a contributor is expected to read the way `kf/blit.cpp` is.

**A theme turned out to be necessary, not optional -- found by looking, not
by guessing.** The original plan applied no LVGL theme at all, reasoning
that five raw widgets don't need one. Building it revealed why that was
wrong: with no theme, `lv_obj`/`lv_label` have no base styling, and on this
project's black demo background that meant black text on a black screen --
LVGL was genuinely drawing (the headless checksum proved that from the
start) but a human looking at the simulator window could not see it. Fixed
by enabling `LV_USE_THEME_SIMPLE` -- "a very simple theme that is a good
starting point," LVGL's own description, not the full default theme with
its animations -- and applying it in `kf_lvgl_display_init()`. The curated
widget *catalog* is unaffected; this is a styling default, not a widget.

**The proof screen and the custom engine don't yet coordinate who owns
which pixels, and the screenshot shows it.** LVGL's screen root is an
opaque object covering the full 240x320 display, so once LVGL flushes its
white background it paints over whatever the demo drew there. The demo, in
turn, only repaints the small patch its sprite moves through each frame
(ADR 0011), so where the bouncing sprite's path crosses the LVGL screen
area, the demo's black patch wins that frame and LVGL's white doesn't come
back (LVGL only flushes when something in its own tree changes, and the
proof screen's widgets are static after the first frame). The result,
visible in the screenshot: a black diagonal band cut through the LVGL
panel, traced by the sprite's own path. Harmless for a placeholder that
nothing is built on top of, and exactly the kind of problem ADR 0013's
hybrid split defers rather than pretends doesn't exist: real menu screens
need an actual answer for which layer owns which region and when each
redraws, and this slice deliberately doesn't invent one with nothing real
to design it against yet.

The same root cause has a second, more visible trigger: pressing MENU
(Enter/Esc on desktop) toggles `kf/app.h`'s constraint HUD, and either way
it toggles, core calls `kf_demo_request_full_repaint()` so the demo can
clear the HUD's old pixels -- a full-screen repaint, not a small patch.
That wipes LVGL's entire panel the same way the sprite's path nicks the
edge of it, just all at once, and it does not come back on its own for the
same reason: nothing in LVGL's own object tree changed, so LVGL never
re-flushes. Confirmed by hand in the simulator, not just reasoned about.
No fix here for the same reason as above -- this is the coordination
problem stated generally, not two separate bugs.

## Windows/MSVC build break, found via CI

The first push of this slice broke the GitHub Actions Windows/MSVC build:
`error C2099: initializer is not a constant`, four times, all in vendored
LVGL v9.2.2 widget source (`lv_bar.c`, `lv_button.c`, `lv_image.c`,
`lv_label.c`) -- exactly the four widgets this slice's curated `lv_conf.h`
enables. This was never caught locally: this project's own sandbox has no
MSVC toolchain, only GCC (see BUILDING.md's "you do not need a GCC build
locally... GitHub Actions... catches the differences that matter" -- this is
the mirror image of that, an MSVC-only difference GCC can't catch either).

Reading LVGL's own source at the four flagged lines found the actual cause,
not a guess: `lv_obj_class_t` (`hakoniwaos`-adjacent, in LVGL's own
`lv_obj_class_private.h`) ends with four bit-field members packed into one
storage word -- `editable : 2`, `group_def : 2`, `instance_size : 16`,
`theme_inheritable : 1`. The base `lv_obj_class` in LVGL's `lv_obj.c` sets
`.editable` and `.group_def` explicitly in its designated initializer and
builds fine, everywhere. All four widgets that failed set only
`.instance_size` and leave the other three bit-fields to their implicit
zero default -- and MSVC's C11 designated-initializer front end apparently
cannot constant-fold a partially-specified bit-field storage word, where
GCC has no trouble with it. (LVGL's own `lv_bar.c` etc. are unmodified
project source from upstream, not something this project wrote --
this is a real MSVC front-end limitation running into ordinary, valid C.)

The fix adds explicit initializers for the three previously-implicit
bit-fields on all four widget classes (the exact same named-constant zero
values -- `LV_OBJ_CLASS_EDITABLE_INHERIT`, `LV_OBJ_CLASS_GROUP_DEF_INHERIT`,
`LV_OBJ_CLASS_THEME_INHERITABLE_FALSE` -- they already defaulted to),
matching the pattern LVGL's own `lv_obj_class` already uses without issue.
A semantic no-op on every platform: the actual field values are identical
before and after, on every compiler, so nothing about LVGL's runtime
behavior changes -- this exists purely to give MSVC's constant-folder a
fully-specified bit-field word to work with. Applied unconditionally (not
gated on the compiler), both because `CMAKE_C_COMPILER_ID` isn't reliably
readable this early in `FetchContent` configure (LVGL's own build hit
exactly this trap, fixed in
[lvgl/lvgl#7401](https://github.com/lvgl/lvgl/pull/7401)) and because
there's no cost to applying a no-op change on GCC/Clang too.

**First attempt, and why it's not what shipped.** The first version of this
fix was a `.patch` file applied via `git apply` in `PATCH_COMMAND`, verified
the same way described above -- applied cleanly against a genuinely fresh
`v9.2.2` clone, full rebuild clean, all 5 tests passing. It was real CI, not
this sandbox, that caught the problem: pushed to Windows CI, the build
failed with the exact same four `C2099` errors at the exact same lines, as
if the patch had never run at all. The patched files, committed to the
repo, were confirmed present and correct in that exact commit (checked
directly on GitHub). Investigating further found that `PATCH_COMMAND`'s
console output does not propagate to the outer `cmake -B` Configure log by
default -- confirmed by reproducing the same silence locally, where the
patch was independently confirmed (by inspecting the populated source
directly) to have actually applied. That makes CI logs alone useless for
telling a working patch step from a silently failing one, on this project
or seemingly any FetchContent-based one; whatever specifically stopped
`git apply` from taking effect on that Windows runner couldn't be pinned
down from the log for that reason, and a simulated CRLF-line-ending
checkout, the leading suspect, was tested directly and ruled out (`git
apply --ignore-whitespace` still succeeded against it, just messily).

**Second attempt, also not what shipped.** Rewrote the fix as
`cmake/patches/lvgl_msvc_c2099_fix.cmake`, run via `PATCH_COMMAND
${CMAKE_COMMAND} -P ...` -- the same four insertions, done with CMake's own
`string(FIND)` / `string(REPLACE)` / `file()` commands instead of `git
apply` against a `.patch` file. No external `git` process invoked
mid-patch, no diff-context matching, nothing line-ending-sensitive.
Unit-tested directly (fresh clone, confirmed the insertion, confirmed a
second run is a safe no-op, confirmed a `FATAL_ERROR` path triggers on a
deliberately wrong anchor), then verified through the real path -- a fresh
`FetchContent` clone via `cmake -B`, confirmed by inspecting the populated
source that the patch actually applied, full rebuild clean, all 5 tests
passing. Pushed to Windows CI, and it failed exactly the same way as the
first attempt: same four `C2099`s, same lines, as if nothing had run.

That result is the actually useful one: two unrelated patch *mechanisms* --
an external `git` process, and CMake's own `-P` script mode, sharing no code
-- both failed identically on the real Windows runner while both passed
repeatedly here, including retested against a from-scratch CMake 4.4.2
install specifically to rule out a version difference from this sandbox's
older CMake. The one thing both attempts had in common was wiring the fix
up as `PATCH_COMMAND`, which only ever executes inside `FetchContent`'s
separate populate "subbuild" -- a mechanism CMake's own changelog describes
as gaining a materially different code path in 3.30 (policy `CMP0168`,
switching between an `ExternalProject`-style subbuild and a direct
in-process implementation for Git downloads) and that could not be run
against an actual Windows + Visual Studio generator combination in this
sandbox to observe directly. That's the honest limit of what could be
pinned down: not a confirmed root cause, but strong enough converging
evidence -- two independent implementations failing the same way, on the
one thing they shared -- to stop trying a third variant of the same
mechanism.

**What shipped instead:** the same `cmake/patches/lvgl_msvc_c2099_fix.cmake`
script content, but no longer wired up as a `PATCH_COMMAND` at all.
`fetch_lvgl.cmake` now calls `kf_lvgl_apply_msvc_fix("${lvgl_SOURCE_DIR}")`
directly, as ordinary CMake script code, immediately after
`FetchContent_MakeAvailable(lvgl)` -- in the same configure pass as
everything else in that file, operating on the `lvgl_SOURCE_DIR` variable
`FetchContent_MakeAvailable` already sets. No subbuild, no separate step
execution, nothing generator- or CMake-version-dependent left in the loop.
Still idempotent (checks its own marker before touching a file) and still
fails loudly via `FATAL_ERROR` on an unexpected anchor -- unchanged from the
second attempt, just invoked differently. Since it's no longer gated behind
a one-time population step, it now runs on *every* configure, including
reconfigures against an already-populated tree -- strictly more robust, not
less: correctness gets re-checked every time instead of assumed after the
first run.

Verified the same way as the second attempt, plus the one thing the first
two couldn't show: `message(STATUS "LVGL MSVC fix: patched ...")` for each
of the four files now appears directly in the `cmake -B` Configure log,
on the first configure, and `"...already patched, skipping"` on a
reconfigure -- proof the fix is actually running, not just reasoning that
it should be. Full rebuild clean `-Werror`, all 5 `ctest` targets passing
with `lvgl_determinism_check`'s checksum unchanged, `check_no_heap.py`
clean. **Still not verified: an actual MSVC compile.** This sandbox has no
Windows toolchain, and that gap is exactly what let the first two attempts'
failures go unnoticed until real CI caught them -- so this one gets the
same treatment: pushed, and confirmed green by CI, not assumed fixed
because the reasoning is sound and it passed everywhere it could be tested.

## Later

- Extending the enabled widget set as real menu screens get designed.
- Deciding how the custom engine and LVGL divide the screen and coordinate
  redraws once both have real content on screen at once -- see the z-order
  finding above. Options include LVGL owning a fixed sub-region rather than
  the full screen, or an explicit paint order per frame; neither is designed
  yet because there is no real menu layout to design it against.
- Converting `kf/font.h`'s existing bitmap font into LVGL's font format, or
  deciding the two font systems stay separate on purpose (LVGL owns menu
  text, the hand-rolled font stays for the pet-facing HUD and any
  gameplay-integrated text) -- open, not yet decided.
- The actual pet-care menu screens themselves, once there's a pet with
  state worth navigating to.
