/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * See kf_lua_settings_screen.h.
 *
 * Task 4 of docs/superpowers/plans/2026-08-13-screens-clock-sleep.md: the
 * Settings screen's own per-frame update -- the same role kf_lua_home_
 * screen.cpp's Home/Info functions play, applied to a third screen whose
 * job is different enough to earn its own file. Home and Info only ever
 * READ pet state and hand it to Lua; Settings OWNS a small state machine
 * (which of four fields is selected, and the hour/minute/AM-PM value being
 * edited but not yet saved) and reads the hardware buttons DIRECTLY, in
 * C++, the same way kf_home_screen_input.h reads Home's five care buttons
 * -- NOT through kf.on_button(), which is a single registry shared by every
 * screen (sdk/lua/kf_lua_scene.cpp's kf_lua_scene_dispatch_buttons()):
 * registering a Settings LEFT/RIGHT/UP/DOWN/A handler there would ALSO fire
 * while Home is active, because Home's own per-frame update is what calls
 * that dispatcher, and Home already binds A/UP/DOWN/LEFT/RIGHT to Feed/
 * Play/Rest/Bath/Flush (kf_home_screen_input.h). Reading kf_app_buttons_
 * pressed()/_held() straight from this file, exactly like kf_home_screen_
 * input.h already does, makes that collision structurally impossible
 * instead of relying on two screens' worth of script to coordinate a
 * shared registry by hand.
 *
 * Lua still owns every PIXEL: this file never touches a kf_scene_id.
 * kf_lua_port_settings_frame() (sdk/lua/kf_lua_port.cpp) hands the current
 * field/hour/minute/AM-PM/save-result down into Lua's on_settings_frame()
 * as plain arguments every frame, and examples/creature_demo/creature.lua
 * is the only code that ever calls kf_scene_set_text()/_set_colors() (via
 * the kf.screen("settings") object handles) for this screen -- the same
 * split Home already has between kf_home_screen_input.h (owns the buttons,
 * mutates pet state) and creature.lua's on_frame (owns the pixels, reads
 * pet.* to decide what they should say).
 *
 * B is never read here. kf_screen_nav_frame() (kf_screen_nav.cpp) checks
 * MENU/B FIRST, every frame, before calling whichever screen's update is
 * currently active -- so on the exact frame B is pressed, that check jumps
 * back to Home and THIS function never runs at all. That single ordering
 * property is the entire cancel path Task 4 requires ("cancel with B from a
 * modified state and assert the clock did not move"): nothing below can
 * have written anything on a frame it was never called on.
 */

#include "kf_lua_settings_screen.h"

#include "kf_error_banner.h"
#include "kf_lua_port.h"
#include "kf_lua_scene.h"

#include "kf/app.h"
#include "kf/clock.h"
#include "kf/hal/time.h"
#include "kf/scene.h"
#include "kf/types.h"

namespace {

kf_scene_id g_error_banner_id = 0;

/* The four cursor positions, in the order LEFT/RIGHT (and A on any field
 * but SAVE) move through them -- "HOUR -> MINUTE -> AM/PM -> SAVE", the
 * plan's own answer to "what does the time API look like". Plain ints
 * rather than an enum crossing the C boundary: kf_lua_port_settings_
 * frame()'s own `field` argument and the debug accessors below both just
 * need a small integer, and one shared int is easier to keep in sync
 * across this file than a type declared in two places. */
constexpr int kFieldHour = 0;
constexpr int kFieldMinute = 1;
constexpr int kFieldAmPm = 2;
constexpr int kFieldSave = 3;

constexpr const char *kFieldNames[4] = {"hour", "minute", "ampm", "save"};

/* Feel constants named in the plan's own answer to "what does the button
 * map look like": repeat starts ~400ms after UP/DOWN is first held, then
 * fires at ~8Hz (a 125ms period). Both are Chris's to tune once he has
 * tried the real buttons (Task 4's own "that judgement is the task's real
 * acceptance, not the check") -- named once, here, so trying a different
 * feel is a one-line edit rather than a hunt through the state machine
 * below. */
constexpr uint32_t kHoldDelayMs = 400;
constexpr uint32_t kHoldIntervalMs = 125;
/* Safety cap on how many repeat steps a single frame can apply, not a
 * tuned value: stops a pathological dt_ms spike from looping unboundedly.
 * Never expected to bind at a real 30-60fps frame rate. */
constexpr int kMaxRepeatStepsPerFrame = 8;

struct SettingsEdit {
    int field = kFieldHour;
    int hour12 = 12; /* 1..12 */
    bool is_pm = false;
    int minute = 0; /* 0..59 */
    uint32_t up_held_ms = 0;
    uint32_t down_held_ms = 0;
    /* -1: no save attempted since the screen was last entered. 0/1: the
     * result of the most recent A-on-SAVE press -- lets creature.lua's
     * on_settings_frame say "SAVE FAILED" rather than silently doing
     * nothing, per Task 4's own reason kf.set_clock() returns false
     * instead of raising. */
    int save_result = -1;
};

SettingsEdit g_edit;

/* Changes whichever field is currently selected by `delta` (+1/-1),
 * wrapping at each field's own edges -- HOUR and MINUTE wrap independently
 * of each other (moving the hour from 12 to 1 does not touch AM/PM, the
 * way a real digital clock's hour button does not silently flip noon to
 * midnight). AM/PM ignores `delta`'s sign entirely: "AM/PM toggles on
 * either" is the plan's own answer, so UP and DOWN do the identical thing
 * on that one field. */
void change_value(int delta) {
    switch (g_edit.field) {
    case kFieldHour:
        g_edit.hour12 += delta;
        if (g_edit.hour12 > 12) {
            g_edit.hour12 = 1;
        } else if (g_edit.hour12 < 1) {
            g_edit.hour12 = 12;
        }
        break;
    case kFieldMinute:
        g_edit.minute += delta;
        if (g_edit.minute > 59) {
            g_edit.minute = 0;
        } else if (g_edit.minute < 0) {
            g_edit.minute = 59;
        }
        break;
    case kFieldAmPm:
        g_edit.is_pm = !g_edit.is_pm;
        break;
    default: /* kFieldSave -- nothing to change */
        break;
    }
    /* Any edit invalidates whatever the last save attempt said -- a stale
     * "SAVED" sitting on screen while the owner is mid-edit on a NEW value
     * would be a lie the moment they change anything. */
    g_edit.save_result = -1;
}

/* Hold-to-repeat for UP/DOWN: the single-step-per-press case is handled by
 * the press-edge check in kf_lua_settings_screen_frame() below, entirely
 * separately from this -- this function only fires ADDITIONAL steps while
 * the button stays physically down (kf_app_buttons_held(), not the
 * press-edge mask), starting kHoldDelayMs after the button first went down
 * and then every kHoldIntervalMs after that. `held_ms` resets to 0 the
 * instant the button is not down, so releasing and re-pressing always
 * starts a fresh delay -- there is no "remembering" across separate
 * presses. */
void handle_hold(uint32_t held, kf_button btn, uint32_t dt_ms,
                  uint32_t &held_ms, int delta) {
    if ((held & static_cast<uint32_t>(btn)) == 0u) {
        held_ms = 0;
        return;
    }
    held_ms += dt_ms;
    int fired = 0;
    while (held_ms >= kHoldDelayMs && fired < kMaxRepeatStepsPerFrame) {
        change_value(delta);
        held_ms -= kHoldIntervalMs;
        ++fired;
    }
}

/* A on SAVE: builds the 24-hour value the edited 12-hour/AM-PM fields name
 * and hands it to the exact same helper kf.set_clock() itself calls
 * (kf_lua_port_apply_clock(), sdk/lua/kf_lua_port.cpp) -- so the Settings
 * screen and any third-party script calling kf.set_clock() directly can
 * never disagree about what "save" means. */
void commit_save() {
    int hour24 = g_edit.hour12 % 12; /* 12 -> 0 */
    if (g_edit.is_pm) {
        hour24 += 12;
    }
    g_edit.save_result =
        kf_lua_port_apply_clock(hour24, g_edit.minute) ? 1 : 0;
}

} // namespace

void kf_lua_settings_screen_init(void) {
    g_error_banner_id = kf_error_banner_create();
}

void kf_lua_settings_screen_enter(void) {
    kf_civil civil;
    kf_civil_from_epoch(kf_time_wall().epoch_seconds, &civil);
    g_edit.field = kFieldHour;
    g_edit.hour12 = civil.hour % 12;
    if (g_edit.hour12 == 0) {
        g_edit.hour12 = 12;
    }
    g_edit.is_pm = civil.hour >= 12;
    g_edit.minute = civil.minute;
    g_edit.up_held_ms = 0;
    g_edit.down_held_ms = 0;
    g_edit.save_result = -1;
}

void kf_lua_settings_screen_frame(uint32_t dt_ms) {
    const uint32_t pressed = kf_app_buttons_pressed();
    const uint32_t held = kf_app_buttons_held();

    if (pressed & KF_BTN_LEFT) {
        if (g_edit.field > kFieldHour) {
            --g_edit.field;
        }
    }
    if (pressed & KF_BTN_RIGHT) {
        if (g_edit.field < kFieldSave) {
            ++g_edit.field;
        }
    }
    if (pressed & KF_BTN_UP) {
        change_value(+1);
    }
    if (pressed & KF_BTN_DOWN) {
        change_value(-1);
    }
    handle_hold(held, KF_BTN_UP, dt_ms, g_edit.up_held_ms, +1);
    handle_hold(held, KF_BTN_DOWN, dt_ms, g_edit.down_held_ms, -1);

    if (pressed & KF_BTN_A) {
        if (g_edit.field == kFieldSave) {
            commit_save();
        } else {
            /* "A on any other field advances (so a player who only ever
             * presses A still gets through)" -- the plan's own answer. */
            ++g_edit.field;
        }
    }

    kf_lua_port_settings_frame(dt_ms, kFieldNames[g_edit.field],
                                g_edit.hour12, g_edit.minute, g_edit.is_pm,
                                g_edit.save_result);
    kf_error_banner_update(g_error_banner_id);
    if (kf_lua_scene_declared_anything()) {
        kf_scene_commit();
    }
}

int kf_lua_settings_screen_debug_field(void) { return g_edit.field; }
int kf_lua_settings_screen_debug_hour12(void) { return g_edit.hour12; }
int kf_lua_settings_screen_debug_minute(void) { return g_edit.minute; }
