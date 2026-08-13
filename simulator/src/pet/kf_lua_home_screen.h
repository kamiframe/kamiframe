/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 * Task 5 of the Lua game-layer plan: the Home implementation for
 * KF_HOME_SCREEN=lua -- the exact counterpart to
 * kf_creature_screen_init()/_enter()/_frame() (kf_ creature_screen.h),
 * except that the actual drawing is examples/ creature_demo/creature.lua's
 * job, not this file's. What this file DOES own: the hardware care buttons
 * (kf_home_screen_input.h, so KF_HOME_ SCREEN=lua does not silently make
 * Feed/Play/Rest/Bath/Flush dead buttons), telling the shared Lua VM this
 * build's script should draw (kf_lua_port_set_home_screen_active()),
 * running its frame, committing the scene it declared, and the one-line
 * error banner (kf_error_ banner.h).
 * Deliberately thin: the actual `kf.sprite`/`kf.text`/`kf.box` declarations
 * live entirely in creature.lua, which is the point of this whole task --
 * this file's job is only to call the Lua VM at the right moment with the
 * right buttons already applied, the same role kf_creature_screen.cpp's
 * kf_creature_screen_frame() plays for the C++ path.
 * ADR 0045 (Task 2 of the screens/clock/sleep plan) added
 * kf_lua_info_screen_frame() below, the same idea applied to Info: no
 * buttons, no presenter, just the shared VM's on_info_frame() (its OWN
 * dedicated entry point, not the generic on_frame() -- see kf_lua_ port.h's
 * own comment on why Home needed the identical treatment) and the error
 * banner, registered as Info's own per-frame update (kf_screen_ nav.cpp) so
 * its text objects keep refreshing while it is the active screen. This file
 * also moved out of simulator/src/lvgl/ that same task, alongside
 * kf_screen_nav.cpp and kf_error_banner.cpp: none of the three has ever had
 * an LVGL dependency of its own -- see ADR 0045 for the full reasoning. */

#ifndef KF_LUA_HOME_SCREEN_H
#define KF_LUA_HOME_SCREEN_H

#include <cstdint>

/* Brings the Lua-drawn Home up: creates the error banner (hidden) and tells
 * the shared Lua VM that THIS build's script owns Home
 * (kf_lua_port_set_home_screen_active(true)) -- the script itself was
 * already loaded and run once by kf_lua_port_init() before this is ever
 * called (see sdl_main.cpp/app_main.cpp's boot sequence), so by the time
 * this runs, creature.lua's top-level kf.sprite()/kf.text()/kf.box() calls
 * have already declared every object it will ever create; nothing here
 * re-declares them. Call once, at kf_screen_nav_init() time, matching kf_
 * creature_screen_init()'s own role for the C++ path. */
void kf_lua_home_screen_init(void);

/* Call every time navigation switches BACK to Home under KF_HOME_SCREEN=lua
 * (kf_screen_nav.cpp's show(), the "index == 0" branch) -- the counterpart
 * to kf_creature_screen_enter() for a screen that does not re-declare its
 * objects on every visit. creature.lua's kf.sprite()/kf.text()/kf.box()
 * calls ran once, at script load, and the ids they returned are still held
 * by the script for the life of the process; kf_scene_reset() would
 * invalidate every one of them. kf_scene_force_repaint() (kf/scene.h,
 * Task 6 of the Lua game-layer plan) is the primitive that exists
 * specifically so this call can force the whole panel to repaint -- which
 * is what stops whatever screen was showing before Home (Info's own
 * kf.screen() group, since ADR 0045, or an LVGL screen under
 * -DKF_ENABLE_LVGL=ON) from leaving pixels behind in rows this scene's own
 * diff would otherwise consider unchanged -- without touching a single
 * object's identity. See docs/architecture/adr-0043-lua-home-default.md
 * and ADR 0042's "Known gap" section, which this closes. */
void kf_lua_home_screen_enter(void);

/* One frame: reads the hardware care buttons (kf_home_screen_input.h),
 * advances the shared creature presenter, runs on_home_frame (kf_lua_port_
 * home_frame() -- see that function's own comment for why this is a
 * dedicated entry point rather than a call to the generic on_frame()),
 * updates the error banner, and commits the scene if the script has ever
 * declared anything (kf_lua_scene_declared_anything()). Call once per frame
 * while this is the active screen -- the exact role kf_creature_screen_
 * frame() plays for the C++ path, same signature, same per-frame contract. */
void kf_lua_home_screen_frame(uint32_t dt_ms);

/* Info's own per-frame contract -- same shape, no buttons or presenter to
 * advance. Registered by kf_screen_nav_init() as "info"'s update callback,
 * unconditionally (unlike kf_lua_home_screen_frame() above, this is not
 * behind KF_HOME_SCREEN_LUA: Info does not care which build owns Home).
 * See this header's own top comment. */
void kf_lua_info_screen_frame(uint32_t dt_ms);

#endif /* KF_LUA_HOME_SCREEN_H */
