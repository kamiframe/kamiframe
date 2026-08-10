/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * The Lua binding over kf/scene.h (Task 2) -- kf.sprite/text/box/background/
 * color/on_button/width/height/sprites, and the object metatable those
 * calls return userdata against. See docs/architecture/adr-0041-lua-
 * drawing-binding.md for the accessor convention, the userdata choice, and
 * every failure behaviour named below.
 *
 * A thin wrapper, deliberately: every call in kf_lua_scene.cpp does
 * argument checking and jQuery-style read/write dispatch, then hands off to
 * one kf/scene.h function. The scene and its differ live in hakoniwaos/,
 * heap-free and float-free on both targets; this file lives in sdk/lua/
 * because Lua's own arena (KF_ARENA_LUA) and the userdata this binding
 * allocates out of it are not core's problem -- see kf_lua_port.h's own
 * header comment for the identical reasoning about why Lua glue sits next
 * to core rather than inside it.
 */

#ifndef KF_LUA_SCENE_H
#define KF_LUA_SCENE_H

struct lua_State;

/* Registers every kf.* scene entry (sprite, text, box, background, color,
 * the WHITE/BLACK/RED/GREEN/BLUE/YELLOW constants, on_button, width,
 * height, sprites) into the `kf` global table -- which must already exist,
 * i.e. this runs after kf_lua_port.cpp's register_bindings() -- and creates
 * the shared "kf.SceneObject" metatable those calls return userdata
 * against. Also resets the on_button callback registry and the "has this
 * script declared anything" flag (see kf_lua_scene_declared_anything()
 * below): both are per-lua_State state that would otherwise dangle across
 * a kf_lua_port_shutdown()/kf_lua_port_init() pair. Call once per
 * kf_lua_port_init(). */
void kf_lua_scene_register(lua_State *L);

/* Reads kf_app_buttons_pressed() (hakoniwaos/src/app.cpp:496, the same
 * debounced press-edge mask kf_screen_nav_frame() and
 * handle_care_buttons() already read -- this is what keeps KFDBG BTN and
 * BTNHOLD exercising the Lua binding too, not a separate input path) and
 * calls whichever kf.on_button() handlers match, each in its own
 * lua_pcall so one bad handler cannot take down the frame or the ones
 * after it. Call from kf_lua_port_frame(), before on_frame -- a button
 * press should be visible to on_frame's own reads of pet.* this same
 * frame, not one frame later. */
void kf_lua_scene_dispatch_buttons(lua_State *L);

/* True once the current script has created at least one scene object or
 * set an explicit background, sticky for the life of this kf_lua_port_init
 * ()/_shutdown() pair (reset by kf_lua_scene_register(), never cleared
 * otherwise). This is NOT part of the kf.* surface a script sees -- it is
 * how the FRAME LOOP (sdl_main.cpp, app_main.cpp) decides whether calling
 * kf_scene_commit() this frame is safe.
 *
 * WHY THIS GUARD HAS TO EXIST: kf_scene_reset() is never called by this
 * task (that is Task 4's job) or by this file, so hakoniwaos/src/scene.cpp
 * 's own g_force_full_redraw starts true and stays true until the first
 * kf_scene_commit() the process ever makes -- by design, per
 * kf_scene_reset()'s own header comment, so the very first commit after
 * boot repaints correctly with no special-casing. Called from an
 * interactive loop where nothing has ever declared a scene, that same
 * "repaint everything" default would paint one solid KF_BLACK frame over
 * whatever the creature screen or LVGL just drew -- a real, visible flash,
 * not a hypothetical one. Skipping the call entirely until a script has
 * actually declared something keeps this task's promise that nothing
 * interactive changes a single rendered pixel. */
bool kf_lua_scene_declared_anything();

#endif /* KF_LUA_SCENE_H */
