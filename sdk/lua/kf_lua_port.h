/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * Ties the Lua port glue together: the arena-backed allocator
 * (kf_lua_alloc.h), a sandboxed lua_State, the kf.* binding surface, and one
 * loaded script. One call to bring Lua up, one call per frame to run the
 * script's on_frame, one call to tear it down.
 *
 * This IS wired into ports/esp32/, not just the desktop and headless
 * backends: ADR 0028 is the decision that put it there --
 * ports/esp32/main/CMakeLists.txt compiles kf_lua_alloc.cpp and this file's
 * kf_lua_port.cpp straight into `main`, and app_main.cpp:216 calls
 * kf_lua_port_init() on boot, the same call sdl_main.cpp and
 * headless_main.cpp make on desktop. hakoniwaos/ (core) is the one boundary
 * this genuinely stays out of, on purpose: Lua is game/SDK glue that sits
 * on top of Core's HAL and arenas (ADR 0014), not part of Core itself.
 */

#ifndef KF_LUA_PORT_H
#define KF_LUA_PORT_H

#include <cstdint>

/* Brings the Lua VM up: kf_lua_alloc_init(), a sandboxed set of standard
 * libraries (see ADR 0014 for exactly which and why), the kf.* bindings,
 * then loads and runs `script_source` as its top-level chunk.
 *
 * `script_source` must be a NUL-terminated Lua chunk and must outlive this
 * call (it is handed to Lua's loader directly, not copied). `chunk_name` is
 * whatever Lua should call it in error messages (Lua's own convention: a
 * leading '@' means "this is a filename", '=' means "show verbatim",
 * anything else is quoted -- see luaL_loadbuffer's documentation via
 * lua_load).
 *
 * Returns false if the chunk failed to compile or its own top-level code
 * raised an error while running; either way the reason is logged through
 * kf_log, never thrown as a C++ exception (core builds with exceptions
 * off). A false return leaves the VM torn down, not half-initialised: call
 * kf_lua_port_shutdown() unconditionally either way if you want to retry
 * with different source, same as every other init/shutdown pair in this
 * codebase. */
bool kf_lua_port_init(const char *script_source, const char *chunk_name);

/* Calls the script's global on_frame(dt_ms) function, if it defined one, in
 * a protected call (lua_pcall) -- a script error here cannot bring down the
 * process, only be logged and reported.
 *
 * `synthetic_frame_delta_ms`: 0 means "use real elapsed time" (tracked
 * internally via kf_time_mono_us(), the same monotonic clock every other
 * frame-timing consumer in this project uses). Non-zero means "advance by
 * exactly this many milliseconds, not real time". Exactly kf_lvgl_port_pump
 * 's parameter, same convention, same reason: see kf_lvgl_tick.h's comment
 * for why real and synthetic time must never be conflated.
 *
 * A script that raises an error here is logged ONCE and on_frame is not
 * called again until the next kf_lua_port_init(): a script erroring every
 * frame should not spam the log 30 times a second. */
void kf_lua_port_frame(uint32_t synthetic_frame_delta_ms);

/* Task 2 of docs/superpowers/plans/2026-08-13-screens-clock-sleep.md (ADR
 * 0045): calls the script's global on_info_frame(dt_ms) function, if it
 * defined one -- the exact same shape as kf_lua_port_frame()/on_frame()
 * above, deliberately a SEPARATE entry point rather than a second call to
 * on_frame() itself.
 *
 * WHY A SEPARATE FUNCTION, NOT ANOTHER CALL TO on_frame(): creature.lua's
 * on_frame() unconditionally mutates Home's own scene objects (body:show()/
 * shrine:hide()/poop visibility) whenever kf.home_screen_active() is true --
 * a correct, harmless no-op every frame Home actually IS the active screen,
 * but a real bug the frame Info is active instead: those calls set the
 * `visible` flag directly, which fights kf_lua_scene_activate_screen()'s own
 * "hide every screen except the active one" bookkeeping and un-hides Home's
 * placeholder creature sprite on top of Info. Found rendering Info for the
 * first time, this task -- see docs/architecture/adr-0045-info-screen-in-
 * lua.md. Calling a screen's OWN dedicated entry point instead, kept
 * separate from every other screen's, is what keeps one screen's per-frame
 * logic from ever touching another's objects, without adding a "which
 * screen is currently active" query no other part of the Lua API needs.
 *
 * Not dispatched through kf.on_button (unlike on_frame()): Info has no
 * interactive widgets of its own, matching kf_pet_info_screen.h's original
 * "no interactive widgets, no LVGL group" note about the screen it
 * replaced. */
void kf_lua_port_info_frame(uint32_t synthetic_frame_delta_ms);

/* Task 4 of docs/superpowers/plans/2026-08-13-screens-clock-sleep.md: calls
 * the script's global on_settings_frame(dt_ms, field, hour, minute, ampm,
 * saved) -- the Settings screen's own dedicated entry point, the same
 * reasoning as kf_lua_port_info_frame() above applied to a third screen: a
 * screen's own per-frame drawing must never run while some OTHER screen is
 * the one actually showing.
 *
 * The four state arguments are the Settings editor's CURRENT, possibly
 * unsaved, values -- `field` is "hour"/"minute"/"ampm"/"save" (which cursor
 * position is selected), `hour`/`minute`/`ampm` are what the fields should
 * currently show, and `saved` is nil (no save attempted since the screen was
 * entered), true, or false (kf.set_clock() -- kf_lua_port_apply_clock()
 * below -- refused). Passed IN by the caller (kf_lua_settings_screen.cpp,
 * simulator/src/pet/, which owns the actual editor state and reads the
 * hardware buttons directly) rather than read by this file reaching UP into
 * that caller: kamiframe_lua_port (this file's own library) links BELOW
 * kamiframe_screen_port (the library that holds the editor), never the
 * other way around (simulator/CMakeLists.txt's own comment on that library),
 * so a callback/getter pointer -- the way kf_screen_nav.h's registration
 * hooks work for the opposite direction -- would invert a dependency this
 * codebase deliberately keeps one-directional. Passing the values as plain
 * arguments sidesteps needing one at all.
 *
 * Not dispatched through kf.on_button, unlike on_frame(): the Settings
 * screen reads LEFT/RIGHT/UP/DOWN/A/B directly in C++, the same way
 * kf_home_screen_input.h reads Home's five care buttons -- see kf_lua_
 * settings_screen.cpp's own header comment for why a shared, screen-unaware
 * kf.on_button registry would let a Settings keypress also fire while Home
 * is active (and vice versa). */
void kf_lua_port_settings_frame(uint32_t synthetic_frame_delta_ms,
                                 const char *field, int hour, int minute,
                                 bool is_pm, int save_result);

/* Shared by kf.set_clock() (the Lua binding, sdk/lua/kf_lua_port.cpp) and
 * the Settings screen's own SAVE action (kf_lua_settings_screen.cpp) -- both
 * must apply the IDENTICAL "preserve today's date and the seconds field,
 * overwrite only hour and minute, call kf_time_set_wall() directly" policy,
 * so the two can never disagree about what saving the clock means. `hour` is
 * 0..23 (matching kf.hour()'s own convention), `minute` is 0..59;
 * out-of-range values are clamped, not rejected -- see the .cpp. Returns
 * false, without raising, when the backend refuses (KF_ERR_UNAVAILABLE on a
 * read-only clock) -- Task 4's own documented contract for kf.set_clock(). */
bool kf_lua_port_apply_clock(int hour, int minute);

void kf_lua_port_shutdown();

/* The value most recently passed to kf.report() from Lua, and how many
 * times on_frame ran to completion without raising an error. Used by the
 * headless determinism check (kamiframe-headless --verify-lua) to assert
 * the whole pipeline -- VM, allocator, bindings -- behaves identically run
 * to run, and by the constraint HUD later. */
int64_t kf_lua_port_last_report();
uint32_t kf_lua_port_frame_count();

/* ---------------------------------------------------------------------
 * Task 5 of the Lua game-layer plan (docs/superpowers/plans/2026-08-12-
 * lua-game-layer.md): the same creature.lua file has to behave differently
 * depending on whether IT is the one drawing Home this build (KF_HOME_
 * SCREEN=lua) or the C++ screen is (KF_HOME_SCREEN=cpp, narration only) --
 * see kf.home_screen_active() in kf_lua_port.cpp for the Lua-facing half of
 * this and examples/creature_demo/creature.lua for how the script uses it.
 * A RUNTIME flag, not a second compiled copy of the script: kf_lua_port_
 * init() seeds it from the build's KF_HOME_SCREEN_LUA compile define, but
 * kamiframe-headless's parity check overrides it explicitly so ONE binary,
 * built with ONE KF_HOME_SCREEN default, can still drive both halves of
 * the comparison.
 * --------------------------------------------------------------------- */
void kf_lua_port_set_home_screen_active(bool active);
bool kf_lua_port_home_screen_active(void);

/* Whether on_frame is currently disabled after raising an error, and what
 * that error said -- kf_lua_port_frame()'s own pcall failure branch already
 * logs this once; these two let a screen implementation draw the SAME
 * message into the reserved band (Task 5's one-line error banner), so the
 * panel says what the console already said rather than freezing silently.
 * kf_lua_port_last_error() returns "" (never NULL) when nothing has failed. */
bool kf_lua_port_disabled_after_error(void);
const char *kf_lua_port_last_error(void);

#endif /* KF_LUA_PORT_H */
