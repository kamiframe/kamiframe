# ADR 0022: Menu/screen navigation

**Status:** Accepted, 2026-08-05
**Reversal cost:** Low. `kf_screen_nav` is a thin switcher -- it owns which
of two pre-built `lv_obj_t` screens is loaded and calls that screen's own
`*_update()`, nothing else. Deleting it and going back to a single direct
`kf_pet_screen_init()`/`update()` call in `sdl_main.cpp` is a few-line
revert; deleting `kf_pet_info_screen` on top of that is one more
header/cpp pair and a link line, the same low cost ADR 0017 already
described for `kf_pet_screen` itself.

## Requirement

Chris, after the debug window and its time-scrubbing timeline landed:
*"So let's work on the next features/design now. What items make hte most
sense for the next portion to pick from?"* Offered four options grounded in
what ADR 0015, ADR 0021 and ADR 0013 had each already named as deferred
work -- a menu/screen system, care-mistake tracking, personality traits, a
random event scheduler -- he picked the menu/screen system.

This is the same moment ADR 0013 named as future work when LVGL was first
chosen (*"the actual pet-care menu screens themselves, once there's a pet
with state worth navigating to"*) and ADR 0017 named again when the pet
screen shipped (*"Navigation to a second screen, once there is a second
thing worth showing"*). There is now a pet with real state -- stage, age,
evolution branch -- that Home's needs-and-buttons screen has no room for
and no reason to show; that is the second thing.

## Decision: a dedicated switcher, orthogonal to LVGL's keypad-group focus system

**MENU (edge) advances to the next screen, wrapping back to Home; B
(edge) jumps straight back to Home from anywhere.** A small, fixed,
ordered list of screens (`kf_screen_nav.cpp`'s `g_screens[]`) with Home at
index 0 -- both the interactive build's starting screen and always what B
returns to. Two screens today (Home, Info); the list is written to grow.

**`kf_screen_nav.cpp` reads `kf_app_buttons_pressed()` directly, the same
debounced edge state `kf/app.cpp`'s own MENU-toggles-the-HUD code reads --
not through `kf_lvgl_input.cpp`'s keypad indev.** Screen switching and
LVGL's keypad-group focus system are kept genuinely orthogonal: which
screen is loaded has nothing to do with which widget inside it currently
has keypad focus. The alternative -- feeding MENU into the keypad protocol
as some new key LVGL interprets as "next screen" -- would need LVGL group
machinery to know about something that is not a widget-focus concept at
all, and would still need an escape hatch for B. Reading the button
straight from the same debounced source Core itself reads is simpler and
matches an existing precedent in the same file family, not a new pattern.

**`kf_lvgl_input.cpp`'s keypad indev no longer maps `KF_BTN_MENU` to
anything.** Before this slice it mapped to `LV_KEY_NEXT` (cycle focus) --
already redundant, since `LV_KEY_UP`/`DOWN`/`LEFT`/`RIGHT` (bound to the
D-pad) already move focus to the previous/next group member under LVGL's
own default group behaviour. Leaving MENU's redundant mapping in place
once it also switches screens would mean the same press sometimes cycled
focus and sometimes changed screens, depending on incidental LVGL group
state -- confusing to reason about and to debug. The D-pad's existing
focus-cycling inside Home is completely unaffected; Feed/Play/Rest are
navigated exactly as ADR 0017 left them, verified by the fact that
`pet_screen_check`'s existing golden checksum did not need to move for
this slice.

**Info has no `lv_group_t` at all.** It has no interactive widgets --
stage name, time in the current stage, and (once there is one) the
evolution branch, read-only -- matching real hardware with no
touchscreen (`kf_lvgl_input.h`'s own header comment) and no reason to
invent a clickable on-screen "back" affordance a physical device could
never have. A screen not needing a group to be navigable is a direct
consequence of the previous decision: nothing in this file ever asks
LVGL's group system to do anything.

**Info reads only public `kf_pet_state` fields, never `kf_pet_session.h`'s
"DEBUG ONLY" `kf_pet_session_debug_age_seconds()`.** That accessor's own
header comment reserves it for the simulator's debug window, not
gameplay, and total-lifetime age additionally needs summed-up config
stage durations that exist only as an unexported helper inside
`kf_pet_session.cpp`. Time *in the current stage*
(`state->stage_elapsed_seconds`) is public state already, needs no config
lookup, and answers the question a player actually has -- "how long has
my pet been like this, is an evolution close" -- without borrowing a tool
built for a different job.

**`kf_pet_screen.cpp` itself is untouched.** Home still builds its
widgets directly onto `lv_screen_active()`, the screen LVGL creates
automatically at `lv_init()` -- `kf_screen_nav_init()` just captures that
pointer immediately after calling `kf_pet_screen_init()`, before Info's
`lv_obj_create(nullptr)` call creates a second, separate screen object
that never touches the active one. `headless_main.cpp`'s existing
`pet_screen_check` still calls `kf_pet_screen_init()`/`update()` directly,
completely unaware this file exists, and its golden checksum did not
move -- the smallest possible blast radius for wiring a second screen in.

## What this slice actually builds

Two real screens: Home (needs + care actions, ADR 0017, unchanged) and
Info (stage, time-in-stage, evolution branch once picked), switchable with
MENU/B, wired into the real `kamiframe-sim` boot and frame loop in the
same slot `kf_pet_screen_init()`/`update()` used to occupy. A
`screen_nav_check` headless test proving the switch mechanism itself --
starts on Home, `kf_screen_nav_debug_advance()` (MENU's effect) reaches
Info deterministically, `kf_screen_nav_debug_home()` (B's effect) returns
to Home and is a no-op when already there -- using new debug/test-only
entry points rather than teaching `headless_input.cpp`'s one shared,
frame-indexed button script a MENU window, which would have disturbed
several other checks' already-locked golden checksums for an unrelated
reason.

## What deliberately is not built

No multi-item menu list, no settings screen, no third screen yet -- two
screens and a MENU/B toggle is the whole navigation surface this slice
needed to prove the mechanism works and give Chris's evolution work
somewhere to be seen. No on-screen "back" button or any other pointer-only
affordance -- see the "no group" decision above for why that would not
match real hardware. No warning colour or icon on Info for an imminent
evolution -- ADR 0017 already deferred the equivalent decision for Home's
need bars for the same reason: a design decision for whenever there is a
reason to react to one, not before.

## Verified

- Full clean rebuild (target `clean`, every object file recompiled from
  source), GCC 13, the same strict warning set as every prior slice,
  clean.
- `tools/check_no_heap.py`: clean.
- All 12 `ctest` targets pass, including the new `screen_nav_check` and
  the untouched `pet_screen_check` (confirming Home's rendering did not
  regress).
- A real interactive smoke test under `Xvfb` + `fluxbox` + `xdotool`,
  real key events (not synthetic button injection): fresh launch starts
  on Home with Feed focused exactly as before; a real `Return` keypress
  (`KF_BTN_MENU`) switches to Info, showing "egg" / "Time in stage: 30s"
  -- live values, not placeholders, matching `KF_PET_SESSION_FLUSH_
  SECONDS`'s 30-second live-tick batching exactly; a real `X` keypress
  (`KF_BTN_B`) returns to Home with all three needs still at 100%, each
  step captured as a screenshot rather than assumed from the log. MENU
  also visibly flickered the constraint HUD on, the same accepted
  shared-key side effect ADR 0017's "Found after delivery" section
  already documented and named as "worth knowing rather than worth
  changing" -- unchanged by this slice, confirmed still true rather than
  assumed.
- Clean shutdown via Ctrl-C (`SIGINT`) verified separately from the
  close-button path, which this slice does not touch at all.

## Found after delivery

Chris, actually using the interactive build rather than reading a
description of it: *"Using the enter key displays that black debug text
at the top which gets in the way of everything. Maybe move that debug
text off into the debug menu itself?"* -- the same gap a headless
checksum cannot close that ADR 0017's own "Found after delivery" section
already named: a rendering checksum proves "the same pixels as last
time," never "is this actually pleasant to use."

**Root cause: MENU became a far more frequently pressed button than it
used to be, and it was never exclusively this slice's to redefine.**
`KF_BTN_MENU` toggles Core's on-device constraint HUD
(`kf/app.cpp`'s `draw_hud()`) on every edge, by design, since ADR 0010 --
genuine Core behaviour, compiled identically for ESP32, not a simulator
quirk this file introduced. ADR 0017's own "Found after delivery" section
had already found and accepted this exact sharing, back when MENU only
mattered for the old proof screen's rarely-touched focus cycling: *"worth
knowing rather than worth changing."* This slice made MENU the primary
way to move between Home and Info -- something worth pressing constantly,
not rarely -- and the same accepted quirk turned into real, reported
friction the moment that changed.

**Fix: an additive input path, not a Core change.** `KF_BTN_MENU`'s
behaviour is untouched -- changing what a real device's one MENU button
does would mean the simulator no longer accurately models real hardware,
exactly what CLAUDE.md's first architecture non-negotiable rules out. What
changed instead is that `kf_screen_nav_debug_advance()` -- already built
for `screen_nav_check`'s headless test -- is now also reachable from a
"Next Screen" button on the debug window (`sdl_debug_window.cpp`).
Clicking it calls the switch directly; `kf_app_frame()` never sees a
`KF_BTN_MENU` edge as a result, so `hud_visible` never flips. The keyboard
still does both at once, unchanged, matching real hardware; the debug
window is now a way to drive screen navigation that never goes through
that shared button at all.

**Chris also asked for the debug text itself somewhere he could see it
without pressing MENU.** Rather than relocating `draw_hud()`'s actual
rendering (same problem as above -- it draws into the one framebuffer a
real device also has, so moving it would misrepresent what MENU does on
hardware), the debug window grew a second, right-hand column that reads
the exact same public accessors `draw_hud()` itself does --
`kf_app_last_frame()`, `kf_app_frame_summary()`, `kf_arena_get_stats()` --
and renders them there, permanently, every frame, no toggle needed. Not a
shadow copy of Core's numbers: if Core's accounting changes, this column's
numbers change with it automatically, same as the real HUD would. The
window widened from 400 to 660px to fit it (`kLeftColumnW`/
`kRightColumnW`, split out specifically so the existing left-column
layout -- buttons, timeline -- did not silently stretch across the extra
space) -- Chris asked to make sure there was room, and there is, with
space to spare.

**Verified**, same discipline as the original slice: full clean rebuild,
zero new warnings; all 12 `ctest` targets still pass (this touches only
`sdl_debug_window.cpp`/`.h`, which no headless check links against, so
none of their golden checksums were even at risk); `check_no_heap.py`
clean; a real interactive pass under `Xvfb` -- clicking "Next Screen"
switches Home to Info with no HUD text drawn over it, the debug window's
own "screen:" readout confirms the switch, a second click cycles back to
Home cleanly, and the right-hand column shows live fps/frame-time/dirty%/
arena figures the whole time; then, separately, a real `Return` keypress
confirmed the keyboard path still does both at once exactly as before --
Info screen AND the HUD overlay -- proving the fix is additive, not a
change to what MENU itself does.

## Later

- A real menu (a list of more than two destinations), once there is a
  third screen worth reaching that a simple MENU-cycles-forward toggle
  stops being the right shape for.
- Total lifetime age on Info, once there is a real reason to want it on
  the gameplay surface specifically (not just the debug window's
  timeline) -- would need `elapsed_before_stage()`'s config-duration
  arithmetic exposed somewhere Info can reach it without depending on
  `kf_pet_session.h`'s DEBUG-only accessor.
- A warning state for an imminent evolution, once there is a design for
  what that should look like.
- Revisit whether `KF_BTN_MENU`'s HUD-toggle behaviour should stay shared
  at all, now that it has caused real friction twice (ADR 0017, and this
  addendum) -- a genuine device/input-design question, and Chris's to
  weigh in on, not something to change unilaterally a third time.

## Superseded in part

**"`kf_pet_screen.cpp` itself is untouched... Home still builds its widgets
directly onto `lv_screen_active()`"** described the mechanism as this ADR
built it; it no longer describes Home. Since ADR 0021 (creature-on-screen)
and ADR 0044/0045 (register-by-name navigation, LVGL optional), Home is
`kf_creature_screen.cpp` or a `kf.screen("home")` Lua group, and
`kf_screen_nav_init()` no longer calls `kf_pet_screen_init()` at all —
`kf_pet_screen.cpp` is reachable only through `pet_screen_check`'s own
direct call, gated behind `-DKF_ENABLE_LVGL=ON`. The two-screen switching
mechanism this ADR introduced (register, advance, debug entry points) is
not superseded — that shape is exactly what ADR 0044 generalised from a
fixed two-entry array to register-by-name.
