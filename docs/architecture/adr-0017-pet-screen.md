# ADR 0017: The pet screen

**Status:** Accepted, 2026-08-04
**Reversal cost:** Low. `kf_pet_screen` is presentation only -- it reads
`kf_pet_session_state()` and calls `kf_pet_session_feed()`/`play()`/
`rest()`, owns no state of its own beyond LVGL widget handles. Deleting it
deletes a header/cpp pair and a link line; nothing else depends on it
existing.

## Requirement

Chris asked directly what was next after the Lua `pet.*` binding (ADR
0016), and asked it in a way worth taking literally: *"is there anything
more I can see in the visual simulation?"* The honest answer at that point
was no -- LVGL (ADR 0013), the pet framework (ADR 0015) and the Lua
binding (ADR 0016) had each landed as pure infrastructure, none of them
pointed at each other where a person looking at `kamiframe-sim` could see
it. All three pieces a pet stat screen needs already existed and had never
been wired together. Offered as one of three next-slice options (a real
Lua pet script, and evolution/life-stages content were the other two, both
still invisible without a screen to show them on) -- Chris picked this
one.

`kf_lvgl_proof_screen.h`'s own header comment named this moment
explicitly, months before it happened: *"Do not build on top of this;
delete it once real menu screens exist."* This is that real screen.

## Decision: read the session directly, don't delete the proof screen

**The screen reads `kf_pet_session_state()` and calls
`kf_pet_session_feed()`/`play()`/`rest()` directly** -- no new
intermediate layer. ADR 0016 already named this as the reason
`kf_pet_session` exists in the first place (*"the Lua binding, and later
a UI, both read `kf_pet_session_state()` rather than each inventing their
own copy"*), and this is that later, now. Three rows (hunger, happiness,
energy), each a name label, an `lv_bar`, and a live percentage label; three
buttons (Feed, Play, Rest) wired to the matching session call via
`lv_obj_add_event_cb`, added to LVGL's default input group the same way
the old proof screen's single button was -- MENU cycles focus, A/ENTER
activates whichever is focused, now exercised across three widgets instead
of one for the first time.

**Millipercent is shown as tenths of a percent on the bar, one decimal
digit on the label.** `kf_pet_millipercent` is 0..100000; `lv_bar`'s range
is a plain integer, so the value is divided by 100 to get a 0..1000 range
-- a visibly smooth bar with no float anywhere in this file. The label
uses `lv_label_set_text_fmt`, LVGL's own printf-style setter, rather than
`kf/app.cpp`'s hand-rolled digit-formatting: that HUD hand-rolls formatting
because it is Core, which `kf/poison.h` forbids from touching libc string
functions at all (see ADR 0008); this file is simulator-side presentation
code with no such constraint, so reaching for the library the display
toolkit already provides is the plain reading of the same principle that
made hand-rolling necessary over there.

**The old proof screen is not deleted, despite its own comment inviting
it.** `lvgl_determinism_check`'s existing golden-checksum test still
exercises it, unchanged, and rewriting an already-verified regression test
to chase a documentation comment's literal instruction was not worth the
risk this slice needed to take on. The practical effect is the same either
way: nothing calls `kf_lvgl_proof_screen_init()` any more (`sdl_main.cpp`
now calls `kf_pet_screen_init()` in its place), so nobody running
`kamiframe-sim` ever sees the proof screen again. Its files stay only so
its own regression test keeps testing what it has always tested. This is
the identical reasoning ADR 0016 already applied to
`lua_determinism_check` when the pet Lua binding landed: add a new,
narrowly-scoped check for the new thing, leave the old, already-verified
one alone.

**Boot and frame ordering both changed, and the change is real, not
cosmetic.** The pet session now has to come up before LVGL, not after:
`kf_pet_screen_init()` calls `kf_pet_screen_update()` once at the end of
its own init so the screen shows real values from its very first frame,
which means `kf_pet_session_init()` must already have run. Per-frame,
`kf_pet_session_frame()` and `kf_pet_screen_update()` both run BEFORE
`kf_lvgl_port_pump()`: the session has to apply this frame's elapsed time
before the screen reads it, and the screen has to push that into its
widgets before `pump`'s `lv_timer_handler()` call redraws and flushes --
otherwise the screen would always be showing last frame's numbers, one
frame behind. A button press is handled INSIDE `pump` (LVGL processes
input during `lv_timer_handler()`), so its effect is visible starting the
NEXT frame's update, not the same one -- one frame of input lag, at this
frame rate not something a person would notice.

**The new headless check is a checksum, not an exact-arithmetic invariant
-- deliberately the opposite choice from ADR 0016's Lua binding check.**
`run_pet_screen_check()` follows `run_lvgl_check()`'s own pattern (FNV-1a
over the rendered framebuffer) rather than `run_lua_pet_check()`'s
(compare a reported value against the live C++ state directly). Both are
right for what they are checking: the Lua check is a logic check --
does a script's call reach the same underlying value a fixed point of the
codebase reads -- with a value to compare that has no reason to touch
pixels at all. The screen check is fundamentally a *rendering* check --
does this set of widget calls draw the same pixels every time -- and every
other rendering check in this codebase (`headless_determinism`,
`headless_fullscreen`, `lvgl_determinism_check`) already answers that
question with a checksum, not by asserting anything about individual
pixel values. Matching the established pattern for the same category of
question, rather than picking whichever style happened to be used most
recently, is the actual reasoning -- not habit.

## What this slice actually builds

Three live need bars, three care-action buttons, wired into the real
`kamiframe-sim` boot and frame loop, backed by the actual `kf_pet_session`
a running simulator uses -- pressing Feed in the interactive build now
visibly moves a bar, immediately, using the exact same call path
`pet.feed()` uses from Lua and `run_pet_check()`'s offline-ageing proof
uses from C++.

## What deliberately is not built

No navigation, no second screen, no menu structure -- one screen, always
active, matching the scope of everything that exists to show it on so
far. No sprite or life-stage art; the bars and labels are the entire
visual vocabulary this slice has. No indicator of WHICH need is
low/critical beyond the bar's own length -- a warning colour or icon is a
design decision for whenever there is a reason (evolution, a care-mistake
mechanic) to react to one, not before.

## Verified

- Full clean rebuild (target `clean`, every object file recompiled from
  source), GCC 13, the same strict warning set as every prior slice,
  clean.
- `tools/check_no_heap.py`: clean -- this slice is entirely
  simulator-side, outside Core's heap ban anyway, but nothing here reaches
  for the heap regardless.
- A new `pet_screen_check` `ctest` target
  (`kamiframe-headless --verify-pet-screen`), an FNV-1a checksum of the
  framebuffer after 30 synthetic frames of a fresh pet's starting screen,
  the same shape as `lvgl_determinism_check`. Deterministic across 6
  repeated runs before being locked in as
  `KAMIFRAME_PET_SCREEN_GOLDEN_CHECKSUM`.
- All 9 `ctest` targets, this one included, pass on the clean rebuild
  above.
- `kamiframe-sim` runs cleanly under the dummy SDL video driver with the
  full new boot order (`kf_app_init` -> pet session -> LVGL -> pet screen
  -> Lua) and full new frame order (pet session frame -> pet screen update
  -> LVGL pump -> Lua frame), confirming the ordering requirements
  documented in `sdl_main.cpp` hold in practice, not just in the headless
  check.

## Found after delivery

Three real bugs, all caught by Chris actually running the interactive build
rather than trusting the headless checksum alone -- exactly the gap a
rendering checksum cannot close, since it only proves "the same pixels as
last time," never "the right pixels."

**A permanent black trail consuming the whole screen.** Chris: *"the blog
still traces a black color over everything in the UI"*. Root cause: `sdl_
main.cpp` still passed `KF_DEMO_SPRITE` to `kf_app_init()`, the same
placeholder bouncing-sprite mode every earlier slice used, and nothing
about building a real screen on top of it had changed that. `kf/demo.cpp`'s
sprite repaints only the small patch it moved off of each frame (`kf_fill_
rect(d.previous, d.background)`) -- cheap by design, see `kf/demo.h`'s own
doc comment -- but LVGL's partial-render mode only re-flushes pixels where
its OWN object tree changed (`kf_lvgl_display.cpp`), so wherever the
sprite's bounce path crossed a static widget, that widget's pixels were
gone for good: nothing was ever going to redraw them again. ADR 0013's
"Accepted cost" section named this exact coordination gap and deliberately
left it open, pending a real screen to design against -- this was that
screen, and the black diagonal band ADR 0013's own screenshot showed as a
curiosity on the placeholder proof screen turned into the pet screen's
dominant visual feature within seconds of it running.

Fixed by adding a third `kf_demo_mode`, `KF_DEMO_NONE` (`kf/demo.h`,
`kf/demo.cpp`): `kf_demo_update()`/`kf_demo_draw()` become unconditional
no-ops, so nothing but LVGL ever touches the framebuffer again once a real
screen owns it. `sdl_main.cpp`'s default changed from `KF_DEMO_SPRITE` to
`KF_DEMO_NONE`; `--stress` still opts into `KF_DEMO_FULLSCREEN` on purpose,
as a deliberate stress tool, accepting the same lack of coordination as a
known cost of asking for it explicitly rather than hitting it by default.
`headless_main.cpp`'s own default mode is untouched -- `headless_
determinism`/`headless_dirty_area`/`headless_fullscreen` test the demo
mechanism itself, not anything LVGL owns, and must keep exercising it.

An earlier, narrower attempt at this fix (invalidating LVGL's screen only
on the `KF_BTN_MENU` edge that toggles the debug HUD, since that specific
toggle also forces a `kf_demo_request_full_repaint()`) was written, built,
verified against all 9 `ctest` targets, and then discarded once a real
screenshot -- taken via `Xvfb` + `xdotool` + ImageMagick `import`, not
assumed -- showed the black band present from the very first rendered
frame, before any button had been pressed at all. That ruled out "only the
HUD toggle causes this" as the actual shape of the bug: the sprite bounces
continuously regardless of any button press, so a fix scoped to one
trigger could only ever chase the symptom, not the cause. `KF_DEMO_NONE`
was the fix that actually matched what was observed.

**No visible way to tell which button is focused.** Once the black trail
was gone, cycling `MENU` produced no visible change at all -- three grey
buttons, no way to tell which one `Z`/`J` (`KF_BTN_A`) was about to
activate. `LV_THEME_SIMPLE` (`kf_lvgl_display.cpp`, chosen in ADR 0013)
does not style `LV_STATE_FOCUSED` at all; the old proof screen's single-
button group never exposed this, since with only one focusable object,
"the focused one" was never ambiguous even though it was never marked
either. Fixed with a small `lv_style_t` (`kf_pet_screen.cpp`'s `g_focus_
style`) applied via `lv_obj_add_style(button, &g_focus_style, LV_STATE_
FOCUSED)` on all three buttons -- independent of the theme, the standard
LVGL mechanism for this, not a theme change.

**Which button ends up focused was not "Feed," and the reason was a real
bug, not a preference.** Confirmed by screenshot that a totally fresh
launch, no keys pressed, focused "Play" -- not the first-created "Feed," the
naive expectation, and not reproducible from reading `lv_group.c` alone.
Diagnosed with `KF_LOGI` calls (temporary, removed before delivery) printing
`lv_obj_get_state()` for all three buttons directly, sidestepping a
confusing red herring from printing raw `lv_group_get_focused()` pointers
first, which appeared to implicate the wrong object because a stale
pointer value looked plausible without actually being the right diagnostic.
The real cause: `lv_button`'s own `lv_obj_class` sets `group_def = LV_OBJ_
CLASS_GROUP_DEF_TRUE` (`lv_button.c`), which `lv_obj_class.c`'s init path
reads to auto-add every new button to whatever group is CURRENTLY DEFAULT
the instant it's created -- already true here, since `kf_lvgl_input_init()`
sets the default group before `kf_pet_screen_init()` ever runs. This
screen's original code (delivered, then corrected here) also called `lv_
group_add_obj()` explicitly for all three buttons, matching the old proof
screen's pattern -- redundant, since auto-registration had already done it,
and not a harmless no-op: `lv_group_add_obj()` first calls `lv_group_
remove_obj()` on the object ("be sure the object is removed from its
current group," `lv_group.c`'s own comment), and removing the CURRENTLY
FOCUSED object forces an immediate refocus onto some other member before
the object is re-inserted at the list's tail. Three redundant add calls in
a row each independently perturbed focus and list order; which button
ended up focused was whichever member that churn happened to land on, not
a designed choice. Fixed by deleting the explicit `lv_group_add_obj()`
calls entirely -- the buttons are already reachable the instant they're
created, and nothing else needs to run.

**Net effect on the golden checksum:** `KAMIFRAME_PET_SCREEN_GOLDEN_
CHECKSUM` moved twice in `simulator/CMakeLists.txt`, once for the focus
style (any button's focused-vs-not pixels are now genuinely different) and
once for the group-registration fix (Feed, not Play, is now the one
rendered focused at start) -- both real, deliberate, checked-in changes to
what the screen looks like, each reverified deterministic across 6 repeated
runs before being locked in, the same discipline as the original value.

All three fixes reverified together: a genuine `clean` target full rebuild,
all 9 `ctest` targets passing, `check_no_heap.py` clean, and a real
interactive smoke test under `Xvfb` -- fresh launch showing Feed focused
with no black trail, `MENU` visibly cycling the blue focus highlight
through Feed/Play/Rest, and `Z` on a focused button visibly moving that
need's bar and label (`Happy` observed going 99.9% -> 100.0% on a `Play`
press), each step captured as a screenshot rather than assumed from the
log. One thing this surfaced that is not a bug: `MENU` is `KF_BTN_MENU` on
both the physical keypad and this desktop build's keyboard mapping
(`sdl_input.cpp`), and it drives two independent things at once by design
-- `kf_lvgl_input.cpp` maps it to `LV_KEY_NEXT` (cycle focus) while `kf/
app.cpp` independently toggles the off-by-default constraint HUD on the
same edge. Both are correct, pre-existing, intentional behaviour; sharing
one key just means cycling button focus also flickers the debug overlay
on and off, worth knowing rather than worth changing.

## Later

- A warning state (colour, icon) for a critically low need, once there is
  a reason to react to one.
- Sprite/art for the pet itself, once there is any -- this screen is
  numbers and bars, nothing else.
- Navigation to a second screen, once there is a second thing worth
  showing.
- Retiring `kf_lvgl_proof_screen.h`/`.cpp` for real, if `lvgl_determinism_
  check` is ever rewritten against the pet screen instead -- not needed
  now, see "Decision" above for why that rewrite was not this slice's job.
