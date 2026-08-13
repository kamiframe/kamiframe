# ADR 0047: The Lua time API, and the Settings screen

**Status:** Accepted
**Date:** 2026-08-13

## Context

Task 4 of the screens/clock/sleep plan is the first
user-visible clock: a global Settings screen the owner can read, edit and
save, plus the Lua binding surface behind it. It builds directly on Task 3
(`kf/clock.h`, ADR 0046, stateless integer civil-time conversion) and Task 1
(`kf.screen()` named object groups, ADR 0044).

Chris's own words: *"The global settings screen can house a system clock to
sync sleep functions to and allow you to change the system clock. Use 12
hour format with am/pm. You can edit, and save the time after you change
it."* The audience constraint (`docs/sdk-style-guide.md`) applies in full:
*"easy to use for non-hardware devs. Like a WordPress developer or jQuery
developer would not have too much trouble."*

## Decision

### The four-function surface, and why no epoch reaches Lua

```
kf.time()          -- "9:05 AM", ready to draw. The only call most scripts need.
kf.hour()          -- 0..23, integer, local
kf.minute()        -- 0..59
kf.clock_set()     -- true once the clock has been set; false on a fresh device
kf.set_clock(hour, minute)  -- writes; the Settings screen calls this, your
                                game almost certainly should not
```

Every numeric argument goes through `luaL_checkinteger`, matching every other
numeric binding in this codebase. `kf.time()`/`kf.hour()`/`kf.minute()` all
convert `kf_time_wall().epoch_seconds` through `kf/clock.h`'s
`kf_civil_from_epoch()` — the SAME conversion Task 6's night-window
accounting will use — rather than each doing its own hour/minute arithmetic,
for the exact reason ADR 0046 gives: two implementations of "what hour is it
locally" is how the clock on screen and the hour the pet falls asleep end up
disagreeing.

An `int64_t` epoch second never crosses into Lua, in either direction. The
epoch stops being anyone's problem above the HAL — Core's `kf/clock.h` and
the HAL's `kf_time_wall()`/`kf_time_set_wall()` are the only things that ever
see one.

### `kf.time()`'s two named edge cases

**Uppercase 12-hour with AM/PM, no leading zero on the hour.** `"9:05 AM"`,
not `"09:05 AM"` — matches how a wall clock is actually read aloud, and every
character (digits, `:`, space, uppercase `A`/`M`/`P`) is in `kf/font.h`'s
glyph set. Midnight is `12:00 AM`, noon is `12:00 PM` — `kf/clock.h`'s civil
hour already gets this right (`hour % 12`, folding `0` to `12`); this binding
only adds the display convention on top.

**`"--:-- --"` on a device whose clock has never been set** — eight
characters, so nothing shifts in width once the clock IS set, and every one
of them (`-`, `:`, space) is in the font. Checked directly against
`kf_time_wall().valid`, not inferred from `epoch_seconds == 0`: epoch 0 is
also `1970-01-01T00:00:00`, a legitimate (if unlikely) civil time a real sync
could produce, so `.valid` is the only honest signal.

### `kf.set_clock()`: preserves date and seconds, never raises

`kf_lua_port_apply_clock(hour, minute)` (`sdk/lua/kf_lua_port.cpp`) is the
ONE place that implements "save the clock": read the current wall time,
convert to a `kf_civil`, overwrite only `.hour` and `.minute`, convert back,
call `kf_time_set_wall()`. `.year`/`.month`/`.day`/`.second` all come
straight from whatever the clock currently says — Task 4 has no date-setting
UI and does not need one to satisfy Chris's own request, which was time
only. On a device whose clock has never been set, "today" resolves to
1970-01-01 the first time anyone saves — a known, accepted limitation of an
hour/minute-only editor, not a bug, and stated here rather than left for
someone to discover.

Both `kf.set_clock()` (the Lua binding) and the Settings screen's own SAVE
action call this SAME function, so the two can never disagree about what
"save" means. It returns `false` — never raises — when
`kf_time_set_wall()` returns `KF_ERR_UNAVAILABLE` (a documented HAL return
for a read-only clock), specifically so a script, or this screen, can say so
on screen rather than crash.

### Nothing about the clock persists outside the RTC itself

No storage key, no config field, "ready for" internet sync or otherwise.
This is not a Task 4 decision — it is `kf/clock.h`'s own no-offset decision
(ADR 0046) carried through: the RTC holds LOCAL time directly, set by hand
on this screen, and Task 4 is the first thing to actually exercise that. A
field nothing sets is wrong the first time something reads it, and once a
field like that ships in the save format it has to be carried forever. When
an internet time sync lands later, it calls `kf_time_set_wall()` with a new
value — exactly what this screen already does — and never has to reinterpret
a stored timestamp, because nothing here ever attached timezone meaning to
one.

### The Settings screen: declared in Lua, edited in C++

`examples/creature_demo/creature.lua` declares `kf.screen("settings")`
unconditionally (like Info, not gated on `kf.home_screen_active()` — the
clock is a device setting, not part of any one creature's screen), registered
third by `kf_screen_nav_init()` so MENU cycles HOME -> INFO -> SETTINGS ->
HOME. Nine scene objects: a title, three field captions (HOUR/MIN/AM-PM),
three editable values, a SAVE row, and a BACK hint — well under the 14-object
budget the brief allotted, plus one more (its own error banner) outside the
screen group, for 10 contributed to the scene's live-object count. See
"Not verified" below for how the whole system's count (43 of 64) was
measured.

**A colour background** (`kf.color(20, 24, 32)`), never a sprite — a sprite
background forces a whole-screen repaint on any dirty frame (`kf/scene.h`),
and a ticking clock would trigger one every second for nothing.

**Editing lives in `simulator/src/pet/kf_lua_settings_screen.cpp`, in C++, not
through `kf.on_button()`.** This is the one design choice in this task that
was not obvious from the brief, so it gets the fullest explanation:

`kf_lua_scene_dispatch_buttons()` (`sdk/lua/kf_lua_scene.cpp`) is a SINGLE
registry, shared by every screen — it does not know which screen is active,
only which buttons were pressed this frame. It is called from whichever
screen's own per-frame update happens to be the active one right now. Home's
per-frame update (`kf_lua_home_screen_frame()` -> `kf_lua_port_frame()`)
already calls it. If Settings registered its LEFT/RIGHT/UP/DOWN/A handlers
through that same registry, pressing those SAME physical buttons while HOME
is active — which already means Feed/Play/Rest/Bath/Flush
(`kf_home_screen_input.h`) — would ALSO silently run the Settings handler,
mutating whatever edit state was left over from the last time Settings was
open. That is exactly the kind of cross-talk this project's own hazard list
warns about: quiet, hard to notice, and capable of corrupting a save the
owner never even opened Settings to make.

So the Settings editor reads `kf_app_buttons_pressed()`/`kf_app_buttons_
held()` directly, the identical pattern `kf_home_screen_input.h` already
uses for Home's five care buttons. That makes the collision structurally
impossible rather than relying on every future screen's script to coordinate
a shared registry by hand. Lua still owns every pixel: the editor never
touches a `kf_scene_id`. `kf_lua_port_settings_frame()` (`sdk/lua/kf_lua_
port.cpp`) hands the current field/hour/minute/AM-PM/save-result down into
Lua's `on_settings_frame(dt_ms, field, hour, minute, ampm, saved)` as plain
arguments every frame — the same "a screen's own dedicated entry point"
shape `on_info_frame()` already established — and `creature.lua` is the only
code that ever calls `kf_scene_set_text()`/`_set_colors()` for this screen.

**The four-field cursor**: HOUR -> MINUTE -> AM/PM -> SAVE, `LEFT`/`RIGHT`
move between them (clamped at the ends, not wrapping), `UP`/`DOWN` change the
highlighted field's value (HOUR wraps 1..12, MINUTE wraps 0..59, AM/PM
toggles on either), `A` on SAVE commits and `A` on any other field advances,
`B` cancels. **`B` is never read inside this file at all** —
`kf_screen_nav_frame()` (`kf_screen_nav.cpp`) checks MENU/B FIRST, every
frame, before calling whichever screen's update is currently active, so on
the exact frame `B` is pressed it jumps back to Home and the Settings editor
never runs that tick. That single ordering property, which already existed
for MENU/B navigation, is the entire cancel path: nothing in the editor can
have written anything on a frame it was never called on.

**Hold-to-repeat** via `kf_app_buttons_held()`: a single step applies on the
press edge, then — while the button stays down — repeat starts after 400ms
and fires at roughly 8Hz (125ms). Both numbers are feel, not physics, named
once as constants so trying a different value is a one-line edit; Chris's own
judgement on hardware is Task 4's real acceptance, not the desktop check.

**The highlighted field is shown by inverting its text colours**
(`kf_scene_set_colors()`), no new drawing primitive needed, exactly as the
plan specified.

**The clock updates by setting the same string every frame.** Confirmed, not
assumed: `run_settings_screen_check()`'s own live-object-count and
button-driven assertions exercise `kf_scene_commit()`'s differ under real
repeated identical `:set()`/`:color()` calls (every quiet frame between
button presses), and nothing in this task needed to work around a differ
that only compared "was a setter called" — the existing differ already
compares content, unchanged since Task 2 of the Lua game-layer plan.

### The MENU reservation

`kf.on_button("menu", ...)` is never bound by this screen or by
`creature.lua` anywhere. MENU is consumed by the screen navigation registry
(`kf_screen_nav.cpp`) to cycle screens; nothing in the code stops a script
from binding it anyway, so this is a documented rule, not an enforced one —
stated in `docs/sdk-style-guide.md`'s new "The one button that is not the
game's" section, and here, so a future task does not rediscover it by having
a handler fire on every screen change.

## The proof

- `run_settings_screen_check()` (`simulator/src/headless/headless_main.cpp`,
  `--verify-settings-screen`), in order:
  1. `kf.time()` on an unset clock (`kf_host_time_set_wall_unset()`, a new
     desktop-only test hook, `simulator/src/host/host_time.h`) returns
     exactly `"--:-- --"` — proved through the REAL Lua binding via
     `kf.report()`/`kf_lua_port_last_report()` (there is no C accessor for a
     scene object's text, so this is the only way to check the actual
     string without inventing one), not by testing the underlying C
     arithmetic directly.
  2. `kf.time()` at `09:05:00` formats as exactly `"9:05 AM"`, the same way.
  3. Home + Info + Settings, loaded together for real (not
     `run_screen_group_check()`'s synthetic two-screen fixture): 3 screens
     registered in order home/info/settings, and `kf_scene_live_object_
     count()` is exactly 43 — closing risk 5 from the plan's own risk table.
  4. The editor, driven with `kf_app_debug_set_buttons()` (a new debug-only
     hook, `kf/app.h`, matching `kf_screen_nav_debug_advance()`'s own "same
     effect as a real press, just callable without one" convention — nothing
     in this check calls `kf_app_frame()`, so nothing else populates the
     debounced button state): two MENU edges reach Settings; three UP
     presses move HOUR from 12 to 3 (asserted via
     `kf_lua_settings_screen_debug_hour12()`); three RIGHT presses visit
     MINUTE, AM/PM and SAVE in order (asserted via `kf_lua_settings_screen_
     debug_field()` at each step); `A` on SAVE moves `kf_time_wall()` by
     **exactly** `3 * 3600` seconds and nothing else (same date, same
     seconds field).
  5. Cancel: re-entering Settings after the save starts from the new 3
     o'clock (not the pre-save 12, proving `kf_lua_settings_screen_enter()`
     actually re-reads the clock); one more UP press modifies HOUR to 4;
     `B` returns to Home and the wall clock's epoch is **unchanged** —
     the cancel path Task 4 names as "the one nobody tests and the one that
     silently corrupts a clock."
- Anti-vacuity, verified by hand: the live-object-count constant
  (`kSettingsCheckExpectedObjectCount`) was temporarily set to a wrong value
  and the check failed by name at that exact assertion, not by crashing or
  silently passing; restored afterward.
- `ctest --test-dir build`: **45/45** (44 baseline + `settings_screen_check`).
  `headless_determinism`, `headless_fullscreen` and `screen_nav_check`'s
  golden checksums are all unmoved — this task draws a new screen but never
  changes a pixel Home or Info already owned, so none of the three had
  reason to move, and none did.
- `python3 tools/check_no_heap.py .`: clean (36 files scanned). Nothing this
  task added lives in `hakoniwaos/`.
- `python3 tools/kf_debug_selftest.py`: clean, unaffected by this task.
- ESP-IDF cross-compile (`-DKF_PANEL=ili9341`): clean, zero warnings.
  `kamiframe-firmware.bin`: 508,720 bytes, 68% of the app partition free.

## Not verified

**The DS3231 write path is still unproved on silicon** — `kf_time_set_wall()`
writes through to the DS3231 on the ESP32 backend (`esp_time.cpp`), but that
write has never executed against real hardware. That is Task 5's bench
test, with a physical unplug and a negative case (the coin cell removed),
not this task's.

**Chris has not yet tried the physical buttons.** The four-field editor, the
400ms/125ms hold-repeat feel, and the field order are all correct by the
desktop check above, but "correct" and "feels right" are different claims —
Task 4's own words: *"Cross-compile. Then flash and let Chris set the time
with the physical buttons and say whether the repeat rate and the field
order feel right. That judgement is the task's real acceptance, not the
check."* That step has not happened yet.

**The 43-of-64 object count is this build's number, not a permanent one.**
It will move if a later task adds objects to any of the three screens; the
check's own named constant is what will catch a silent overrun, not this
document.
