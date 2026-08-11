/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * Task 5 of the Lua game-layer plan (docs/superpowers/plans/2026-08-12-lua-
 * game-layer.md): the Home implementation for KF_HOME_SCREEN=lua -- the
 * exact counterpart to kf_creature_screen_init()/_enter()/_frame() (kf_
 * creature_screen.h), except that the actual drawing is examples/
 * creature_demo/creature.lua's job, not this file's. What this file DOES
 * own: the hardware care buttons (kf_home_screen_input.h, so KF_HOME_
 * SCREEN=lua does not silently make Feed/Play/Rest/Bath/Flush dead
 * buttons), telling the shared Lua VM this build's script should draw
 * (kf_lua_port_set_home_screen_active()), running its frame, committing
 * the scene it declared, and the one-line error banner (kf_error_
 * banner.h).
 *
 * Deliberately thin: the actual `kf.sprite`/`kf.text`/`kf.box` declarations
 * live entirely in creature.lua, which is the point of this whole task --
 * this file's job is only to call the Lua VM at the right moment with the
 * right buttons already applied, the same role kf_creature_screen.cpp's
 * kf_creature_screen_frame() plays for the C++ path. */

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

/* One frame: reads the hardware care buttons (kf_home_screen_input.h),
 * advances the shared creature presenter (inside kf_lua_port_frame() --
 * see that function's own Task 5 comment), runs on_frame, updates the
 * error banner, and commits the scene if the script has ever declared
 * anything (kf_lua_scene_declared_anything()). Call once per frame while
 * this is the active screen -- the exact role kf_creature_screen_frame()
 * plays for the C++ path, same signature, same per-frame contract. */
void kf_lua_home_screen_frame(uint32_t dt_ms);

#endif /* KF_LUA_HOME_SCREEN_H */
