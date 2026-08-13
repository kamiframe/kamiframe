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
 * process, only be logged and reported. GENERIC and screen-agnostic: the
 * main loop (sdl_main.cpp/app_main.cpp) calls this exactly once per real
 * frame, unconditionally, regardless of which kf.screen() is currently
 * active -- on_frame() is where creature.lua puts its own screen-agnostic
 * observations (announce_stage()/announce(), the hunger/happiness/energy
 * log lines), NOT where any one screen's pixels get declared. A screen's
 * own drawing belongs in that screen's OWN dedicated entry point instead
 * (kf_lua_port_home_frame()/_info_frame()/_settings_frame() below) --
 * see kf_lua_port_home_frame()'s own comment for why on_frame() used to get
 * this wrong for Home specifically, and what that cost on real hardware.
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

/* Calls the script's global on_home_frame(dt_ms) function, if it defined
 * one -- Home's own dedicated entry point, called ONLY from kf_lua_home_
 * screen.cpp's kf_lua_home_screen_frame(), itself only ever invoked while
 * Home is the active screen (kf_screen_nav.cpp's per-screen `update`
 * dispatch). Same shape as kf_lua_port_info_frame()/_settings_frame()
 * below, added for the identical reason theirs were: a screen's own
 * per-frame drawing must never run while some OTHER screen is the one
 * actually showing.
 *
 * A HARDWARE BUG, NOT JUST A DESIGN PREFERENCE: before this function
 * existed, Home's :show()/:hide()/:move() calls lived directly inside the
 * shared on_frame() above, guarded by `if kf.home_screen_active() then`.
 * That guard does not mean what it reads as -- kf.home_screen_active() is a
 * BUILD-TIME flag (true for the life of the process on a KF_HOME_SCREEN=lua
 * build, kf_lua_port_set_home_screen_active()'s own comment), not "is Home
 * the screen showing right now" -- so it passed on every call regardless of
 * the active screen. That would have been harmless if on_frame() only ever
 * ran while Home was active, but it does not: the main loop calls it
 * unconditionally, every real frame (see this function's own header comment
 * above), on top of kf_screen_nav_frame() already having called Home's own
 * update whenever Home actually was active. On a real device this put
 * Home's creature/poop/shrine objects back to `visible=true` every single
 * frame no matter which screen kf_lua_scene_activate_screen() had actually
 * selected -- frozen (the wander itself never advanced, since that only
 * happens inside kf_lua_home_screen_frame()) but sitting on top of Info and
 * Settings regardless. Splitting Home's block into its OWN entry point,
 * exactly like Info and Settings already had, closes this: on_frame() no
 * longer touches any screen's objects, so the main loop's unconditional
 * call is safe on every screen, and on_home_frame() only ever runs while
 * kf_lua_home_screen_frame() calls it, which only happens while Home is
 * active. */
void kf_lua_port_home_frame(uint32_t synthetic_frame_delta_ms);

/* Task 2 of the screens/clock/sleep plan (ADR
 * 0045): calls the script's global on_info_frame(dt_ms) function, if it
 * defined one -- the exact same shape as kf_lua_port_home_frame() above,
 * deliberately a SEPARATE entry point rather than a second call to
 * on_frame() itself.
 *
 * WHY A SEPARATE FUNCTION, NOT ANOTHER CALL TO on_frame(): a screen's own
 * per-frame drawing must never run while some OTHER screen is the one
 * actually showing -- kf_lua_port_home_frame()'s own comment above is the
 * account of what got this wrong for Home specifically before it existed.
 * Calling a screen's OWN dedicated entry point instead, kept separate from
 * every other screen's, is what keeps one screen's per-frame logic from
 * ever touching another's objects, without adding a "which screen is
 * currently active" query no other part of the Lua API needs. See
 * docs/architecture/adr-0045-info-screen-in-lua.md.
 *
 * Not dispatched through kf.on_button (unlike on_frame()): Info has no
 * interactive widgets of its own, matching kf_pet_info_screen.h's original
 * "no interactive widgets, no LVGL group" note about the screen it
 * replaced. */
void kf_lua_port_info_frame(uint32_t synthetic_frame_delta_ms);

/* Task 4 of the screens/clock/sleep plan, extended by the sound-foundation
 * follow-up's volume setting: calls the script's global on_settings_
 * frame(dt_ms, field, hour, minute, ampm, saved, volume) -- the Settings
 * screen's own dedicated entry point, the same reasoning as kf_lua_port_
 * info_frame() above applied to a third screen: a screen's own per-frame
 * drawing must never run while some OTHER screen is the one actually
 * showing.
 *
 * The state arguments are the Settings editor's CURRENT, possibly unsaved,
 * values -- `field` is "hour"/"minute"/"ampm"/"volume"/"save" (which cursor
 * position is selected), `hour`/`minute`/`ampm` are what the clock fields
 * should currently show, `volume` is 0..4 (KF_VOLUME_OFF..KF_VOLUME_4,
 * kf/hal/audio.h) for what the volume field should currently show, and
 * `saved` is nil (no save attempted since the screen was entered), true, or
 * false (kf.set_clock()/kf_lua_port_apply_volume() below refused -- see
 * kf_lua_settings_screen.cpp's own commit_save() for why ONE save result
 * covers both). Passed IN by the caller (kf_lua_settings_screen.cpp,
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
                                 bool is_pm, int save_result, int volume);

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

/* Shared by kf.set_volume() (the Lua binding) and the Settings screen's own
 * SAVE action, the identical split kf_lua_port_apply_clock() above has for
 * the clock -- both must apply hardware output (kf_audio_set_volume()) AND
 * persist it (kf_settings_save(), kf/settings.h) the same way, so the two
 * can never disagree about what "the volume is N" means. `level` is 0..4
 * (KF_VOLUME_OFF..KF_VOLUME_4); out-of-range values are clamped, matching
 * kf_audio_set_volume()'s own documented contract. UNLIKE the clock, this
 * cannot fail against a "read-only" backend -- kf/settings.h's own store is
 * always writable in the same sense the pet's own save is -- so this
 * returns false only if the underlying kf_settings_save() call itself
 * failed (a storage error), matching kf_lua_port_apply_clock()'s own
 * "false, without raising" convention for the identical reason: the
 * Settings screen shows "SAVE FAILED" rather than silently doing
 * nothing. */
bool kf_lua_port_apply_volume(int level);

void kf_lua_port_shutdown();

/* The value most recently passed to kf.report() from Lua, and how many
 * times on_frame ran to completion without raising an error. Used by the
 * headless determinism check (kamiframe-headless --verify-lua) to assert
 * the whole pipeline -- VM, allocator, bindings -- behaves identically run
 * to run, and by the constraint HUD later. */
int64_t kf_lua_port_last_report();
uint32_t kf_lua_port_frame_count();

/* ---------------------------------------------------------------------
 * DEBUG/TEST ONLY -- same convention as kf_lua_settings_screen_debug_
 * field() and its siblings (kf_lua_settings_screen.h): a narrow read-only
 * window into state a real caller has no business reading, kept purely so
 * a headless check can prove what actually got drawn. kf.report()'s own
 * comment (kf_lua_port.cpp) already explains why Lua-to-C can only carry
 * one integer at a time; nothing on this side of the boundary can read a
 * live scene object's text or colour back at all (kf_lua_scene.cpp's own
 * comment on LuaSceneObject: kf/scene.h's Core API is write-only past
 * kf_scene_bounds()). examples/creature_demo/creature.lua's Settings
 * screen writes two plain globals every on_settings_frame() call --
 * kf_settings_debug_volume_label (the exact string shown beside VOLUME:
 * "OFF"/"25%"/"50%"/"75%"/"100%") and kf_settings_debug_volume_bars (the
 * meter's own state: 4 characters, one per bar, '1' lit/'0' dim left to
 * right, or the literal "MUTE" at OFF) -- and these two functions read
 * them straight back via lua_getglobal(), the ONLY way to prove the
 * Settings screen's volume meter actually drew something different for
 * each of the five levels rather than merely reaching the line that draws
 * it. Returns a pointer into a small static buffer, valid until the next
 * call to EITHER of this pair (two separate buffers, so calling both
 * before using either is safe) -- empty string before Settings has ever
 * run one frame, or if the global was somehow cleared. */
const char *kf_lua_port_debug_settings_volume_label();
const char *kf_lua_port_debug_settings_volume_bars();

/* ---------------------------------------------------------------------
 * Task 5 of the Lua game-layer plan: the same creature.lua file has to
 * behave differently depending on whether IT is the one drawing Home this
 * build (KF_HOME_ SCREEN=lua) or the C++ screen is (KF_HOME_SCREEN=cpp,
 * narration only) -- see kf.home_screen_active() in kf_lua_port.cpp for the
 * Lua-facing half of this and examples/creature_demo/creature.lua for how
 * the script uses it. A RUNTIME flag, not a second compiled copy of the
 * script: kf_lua_port_ init() seeds it from the build's KF_HOME_SCREEN_LUA
 * compile define, but kamiframe-headless's parity check overrides it
 * explicitly so ONE binary, built with ONE KF_HOME_SCREEN default, can
 * still drive both halves of the comparison.
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
