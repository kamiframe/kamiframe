/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * The Lua binding over kf/scene.h (Task 2) -- kf.sprite/text/box/background/
 * color/on_button/width/height/sprites, and the object metatable those
 * calls return userdata against. See docs/architecture/adr-0041-lua-
 * drawing-binding.md for the accessor convention, the userdata choice, and
 * every failure behaviour named below. kf.screen(name) (ADR 0044, Task 1
 * of the screens/clock/sleep plan) adds named
 * GROUPS of these same objects, over the same one scene -- see this
 * header's own section below.
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

#include <cstdint>

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

/* Drops this file's reference to the lua_State kf_lua_scene_register()
 * captured. Call from kf_lua_port_shutdown() BEFORE lua_close(), so a
 * screen switch that lands between the shutdown and the next
 * kf_lua_port_init() -- the simulator's debug window can advance screens
 * with no script loaded at all -- finds a null state and does nothing,
 * rather than walking a closed one's registry. */
void kf_lua_scene_unregister(void);

/* Reads kf_app_buttons_pressed() (hakoniwaos/src/app.cpp:496, the same
 * debounced press-edge mask kf_screen_nav_frame() and
 * kf_home_screen_handle_care_buttons() already read -- this is what keeps
 * KFDBG BTN and BTNHOLD exercising the Lua binding too, not a separate
 * input path) and
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

/* ---------------------------------------------------------------------
 * ADR 0044: kf.screen(name) -- named groups of scene objects over the ONE
 * shared retained scene, and the function-pointer boundary that lets
 * simulator/src/lvgl/kf_screen_nav.cpp's registry call back into this
 * file without kamiframe_lua_port linking against kamiframe_lvgl_port
 * (simulator/CMakeLists.txt forbids that direction; kf_screen_nav.h's own
 * header comment has the full reasoning).
 * --------------------------------------------------------------------- */

/* Matches kf_screen_nav_register()'s and kf_screen_nav_show()'s exact
 * signatures (kf_screen_nav.h) -- declared independently here, not by
 * including that header, which is the whole point: this file only needs
 * to know the SHAPE of the two functions it will be handed, never their
 * name or which library defines them. */
typedef int (*kf_screen_nav_register_fn)(const char *name,
                                          void (*update)(uint32_t dt_ms));
typedef void (*kf_screen_nav_show_fn)(int index);

/* Called once by kf_screen_nav_install_lua_hooks() (kf_screen_nav.cpp,
 * which CAN include this file's header -- kamiframe_lvgl_port already
 * depends on kamiframe_lua_port the other way) to hand this file the two
 * registry entry points kf.screen() and screen:show() need. Before this is
 * called, kf.screen() raises a Lua error naming the gap rather than
 * calling through a null pointer -- see lua_kf_screen()'s own comment. */
void kf_lua_scene_set_screen_nav(kf_screen_nav_register_fn register_fn,
                                  kf_screen_nav_show_fn show_fn);

/* Called by kf_screen_nav_show() (the allowed direction) as ITS first
 * step, for every switch, whether triggered by MENU/B or by a script's own
 * screen:show().
 *
 * RELEASES every kf.screen() group that is not `index` -- their scene
 * objects are handed back and their slots freed (ADR 0061) -- and then, if
 * a group IS registered under `index`, RESTORES that group's objects from
 * the shadow state its userdata has always carried, re-applies its stored
 * background colour if it ever called screen:background() (a no-op
 * otherwise -- see ADR 0044's "the trap is two owners" discussion for why
 * a screen that never set one inherits whatever is already there rather
 * than a new default), then kf_scene_force_repaint() and, if anything has
 * ever been declared, an immediate kf_scene_commit() so a caller that
 * inspects the panel right after the switch -- screen_nav_check does
 * exactly this -- sees the result without waiting for a frame.
 *
 * `name` is the NAVIGATION REGISTRY's name for this index (kf_screen_nav_
 * name()), which is what kf.active_screen() reports from here on. It is
 * recorded even when no kf.screen() group is registered under `index`,
 * which is the case that matters: under KF_HOME_SCREEN=cpp Home is a real
 * screen with no Lua group, and a kf.active_screen() that could never
 * answer "home" is what made the play picker's UP a dead button in that
 * build. The release pass runs in that case too -- the outgoing screen's
 * slots have to come back whether or not the incoming one wants them. */
void kf_lua_scene_activate_screen(int index, const char *name);

/* Called once by kf_lua_port_init() (sdk/lua/kf_lua_port.cpp), right after
 * a script's top-level code finishes running -- i.e., right after every
 * kf.screen() call that script will ever make has already created its
 * group. Does the SAME release-and-restore bookkeeping kf_lua_scene_
 * activate_screen() does -- deliberately WITHOUT that function's own
 * kf_scene_force_repaint()/kf_scene_commit(), because kf_lua_port_init()
 * has no guarantee a framebuffer exists yet (several headless checks
 * exercise pet.* / on_frame logic with none at all, and kf_scene_commit()
 * asserts one). Nothing it does draws, so that is safe.
 *
 * See this function's own definition for the bug this closes: a script
 * with two or more kf.screen() groups (Home and Info, since Task 2 of the
 * screens/clock/sleep plan) would otherwise leave every group's objects
 * overlapping on screen until the first real switch. Since ADR 0061 it
 * carries the extra weight of being where a cartridge's non-showing
 * screens actually hand their scene slots back after the top-level code
 * that declared them has run. Releases even when no group is registered
 * under `active_index` -- under KF_HOME_SCREEN=cpp, Home is a real screen
 * with no Lua group, and the screens it replaced still have to let go. */
void kf_lua_scene_hide_other_screens(int active_index);

/* Exactly what kf.active_screen() returns to a script -- the navigation
 * registry's name for whichever screen is showing, or "" before anything
 * has ever been activated (never null, so a caller can strcmp it without
 * a guard, matching what the Lua side already promises).
 *
 * Written for the regression test that pins ADR 0061's other half: this
 * name must be right even for a registered screen that has NO kf.screen()
 * group, because that is what Home is under KF_HOME_SCREEN=cpp, and a
 * version of this that could only ever name a Lua group is what made the
 * play picker's UP a dead button in that build. No CI job builds
 * KF_HOME_SCREEN=cpp, so the test reproduces the shape instead -- see
 * run_screen_group_check(). */
const char *kf_lua_scene_active_screen_name(void);

/* The registry index last passed to kf_lua_scene_activate_screen() or
 * kf_lua_scene_hide_other_screens() that actually matched a registered
 * group -- i.e. whichever screen is genuinely active right now, from
 * this file's own point of view. 0 (Home) before either has ever been
 * called, matching kf_screen_nav_init()'s own "Home is index 0" starting
 * point.
 *
 * Added for kf_lua_port_load() (Task 3 of the Nibble-and-the-game-session
 * plan): a SECOND (or later) script loaded into an already-running VM
 * needs to re-run kf_lua_scene_hide_other_screens() for the identical
 * "newly declared groups start visible" reason kf_lua_port_init() already
 * does for the first one -- but by the time a later chunk loads, Home is
 * not necessarily still the active screen, and hardcoding 0 there was a
 * real bug: it silently re-activated Home, undoing whatever a chunk
 * loaded after a real navigation had just done. This getter is what lets
 * kf_lua_port_load() pass the CURRENT active index instead of assuming
 * it is always 0. */
int kf_lua_scene_active_registry_index(void);

#endif /* KF_LUA_SCENE_H */
