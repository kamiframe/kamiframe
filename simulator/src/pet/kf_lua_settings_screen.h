/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * Task 4 of the screens/clock/sleep plan, extended by the sound-foundation
 * follow-up's volume setting: the Settings screen -- a global, device-wide
 * system clock AND volume the owner can read, edit and save, with a five-
 * field cursor (HOUR -> MINUTE -> AM/PM -> VOLUME -> SAVE) over the seven
 * hardware buttons. See kf_lua_settings_screen.cpp's own
 * header comment for why this screen reads its buttons directly in C++
 * rather than through kf.on_button(), and sdk/lua/kf_lua_port.h's
 * kf_lua_port_settings_frame() for how the edit state reaches Lua, which is
 * the only code that ever touches this screen's pixels.
 */

#ifndef KF_LUA_SETTINGS_SCREEN_H
#define KF_LUA_SETTINGS_SCREEN_H

#include <cstdint>

/* Creates this screen's own hidden error banner -- call once, from
 * kf_screen_nav_init(), the same point Home's kf_lua_home_screen_init()
 * is called from. A separate banner from Home/Info's shared one (kf_lua_
 * home_screen.cpp): one extra ungrouped scene object, cheap, and it keeps
 * this file self-contained rather than reaching into another file's
 * anonymous-namespace state. */
void kf_lua_settings_screen_init(void);

/* Resets the four-field editor from whatever the wall clock currently says
 * -- field back to HOUR, hour/minute/AM-PM read fresh via kf/clock.h, any
 * in-progress edit from a previous visit discarded. Call every time
 * navigation switches TO this screen (kf_screen_nav.cpp's show(), the
 * "index == the settings index" branch), so a cancelled edit never
 * resurfaces the next time the screen is opened -- see kf_lua_settings_
 * screen.cpp's own comment on why B never even reaches this file's frame
 * function, which is the other half of that same guarantee. */
void kf_lua_settings_screen_enter(void);

/* One frame: reads LEFT/RIGHT/UP/DOWN/A directly from kf_app_buttons_
 * pressed()/_held() (kf/app.h), advances the four-field cursor or the
 * highlighted field's value, hold-to-repeats UP/DOWN, saves on A-at-SAVE
 * (kf_lua_port_apply_clock()), then hands the current field/hour/minute/
 * AM-PM/save-result down into Lua's on_settings_frame() (kf_lua_port_
 * settings_frame()) so creature.lua can draw it, and commits the scene.
 * Registered as this screen's own per-frame update (kf_screen_nav_
 * register("settings", ...)), the same role kf_lua_home_screen_frame()/
 * kf_lua_info_screen_frame() play for the other two screens. */
void kf_lua_settings_screen_frame(uint32_t dt_ms);

/* ---------------------------------------------------------------------
 * DEBUG/TEST ONLY -- same convention as kf_screen_nav_debug_index()
 * (kf_screen_nav.h): reflects exactly the state the real per-frame update
 * above reads and writes, not a second copy of it. Lets a headless check
 * (simulator/src/headless/headless_main.cpp's run_settings_screen_check())
 * assert the cursor actually visited each field while driving it with
 * kf_app_debug_set_buttons(), rather than only checking the final saved
 * time and trusting the state machine got there the way it was supposed
 * to.
 * --------------------------------------------------------------------- */
int kf_lua_settings_screen_debug_field(void); /* 0=HOUR,1=MINUTE,2=AMPM,3=VOLUME,4=SAVE */
int kf_lua_settings_screen_debug_hour12(void); /* 1..12 */
int kf_lua_settings_screen_debug_minute(void); /* 0..59 */
int kf_lua_settings_screen_debug_volume(void); /* 0..4, KF_VOLUME_OFF..KF_VOLUME_4 */

#endif /* KF_LUA_SETTINGS_SCREEN_H */
