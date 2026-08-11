/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * Switches which screen is on top -- Home and Info so far, more later, any
 * number up to KF_SCREEN_NAV_MAX_SCREENS -- and owns the ONE input mapping
 * that does it: MENU advances to the next screen, wrapping back to Home; B
 * jumps straight back to Home from anywhere. See ADR 0022 for the original
 * two-screen mechanism and ADR 0044 for why it became register-by-name
 * rather than staying a fixed two-entry array.
 *
 * Deliberately reads kf_app_buttons_pressed() directly, the same debounced
 * edge state kf/app.cpp's own KF_BTN_MENU-toggles-the-HUD code reads --
 * NOT through kf_lvgl_input.cpp's keypad indev. Screen switching and LVGL's
 * own keypad-group focus system are kept orthogonal on purpose: which
 * screen is loaded has nothing to do with which widget inside that screen
 * currently has keypad focus, and mixing the two would mean MENU sometimes
 * cycles focus and sometimes changes screens depending on what LVGL's
 * group state happened to be -- see kf_lvgl_input.cpp's own header comment
 * for why MENU no longer feeds LV_KEY_NEXT there at all now that this file
 * owns it exclusively. This also means a screen genuinely does not need an
 * lv_group_t to be navigable -- kf_pet_info_screen.cpp has none -- since
 * nothing here ever asks LVGL's group system to do anything.
 *
 * ADR 0044: A REGISTRY, NOT A SCENE-PER-SCREEN. There is one retained scene
 * (kf/scene.h, KF_SCENE_MAX_OBJECTS slots) shared by every screen; a Lua
 * screen (sdk/lua/kf_lua_scene.cpp's "kf.Screen" group) is a NAME plus the
 * list of scene object ids declared through it, not a scene of its own.
 * This file owns exactly one thing about that: WHICH name is showing. It
 * does not know which kf_scene_ids belong to which name -- that bookkeeping
 * lives in kf_lua_scene.cpp, on the other side of a function-pointer
 * boundary (kf_screen_nav_install_lua_hooks() below) rather than a #include,
 * because kamiframe_lua_port must not link against kamiframe_lvgl_port
 * (simulator/CMakeLists.txt's own comment on that target -- a hard
 * dependency direction, not a preference) while kf.screen(name) still has
 * to reach kf_screen_nav_register()/_show() to register and switch through
 * the one place that is allowed to.
 */

#ifndef KF_SCREEN_NAV_H
#define KF_SCREEN_NAV_H

#include <cstdint>

/* Room for Home, Info, Settings and a handful of scratch/example screens
 * with headroom to spare -- this is a handheld with one MENU button to
 * cycle them with, not a desktop with a taskbar. Registering past this cap
 * logs once naming the limit and returns -1 rather than growing the table;
 * see kf_screen_nav_register()'s own comment. */
#define KF_SCREEN_NAV_MAX_SCREENS 8

/* Creates a new screen entry named `name` and returns its index, OR -- if
 * `name` already names a registered screen -- returns that screen's
 * existing index unchanged, `update` included: the first registration of a
 * name wins the update callback, every later call under the same name is a
 * pure fetch. This is what lets kf_screen_nav_init() register "home" with
 * its real per-frame update function BEFORE creature.lua ever runs, and
 * creature.lua's own kf.screen("home") (sdk/lua/kf_lua_scene.cpp, wired
 * through kf_screen_nav_install_lua_hooks() below) safely fetch that exact
 * same index moments later without silently replacing it with a null
 * update -- both call sites name the same screen, only one of them knows
 * what should run every frame while it is active.
 *
 * `update` may be nullptr -- most Lua-declared screens have no C-side
 * per-frame behaviour at all; kf_screen_nav_frame() below skips a null
 * update rather than calling through it. Returns -1, logging once naming
 * KF_SCREEN_NAV_MAX_SCREENS, if the table is already full of screens that
 * are NOT this name.
 *
 * Home must be the first name ANY caller registers in a given process --
 * see kf_screen_nav_init()'s own comment for why index 0 has to stay
 * meaning "Home" (B jumps there; kf_screen_nav_debug_home() jumps there). */
int kf_screen_nav_register(const char *name, void (*update)(uint32_t dt_ms));

/* The name a screen was registered under, or "?" for an index that is
 * negative or >= kf_screen_nav_count() -- a debug readout must never crash
 * over an index nobody has registered yet, and this makes that true by
 * construction instead of by a caller remembering to guard it (see
 * sdl_debug_window.cpp's own former screen_name() switch, deleted with
 * ADR 0044 because a third screen would have made it silently wrong rather
 * than obviously incomplete). */
const char *kf_screen_nav_name(int index);

/* How many screens are registered right now. */
int kf_screen_nav_count(void);

/* Brings up every screen this build knows about (currently: Home, Info)
 * and loads Home first. Call once, after kf_lvgl_port_init() and
 * kf_pet_session_init() -- both screens' init functions need the pet
 * session ready the same way kf_pet_screen_init() always has.
 *
 * Home is the creature screen: it draws straight into the framebuffer
 * (KF_HOME_SCREEN=cpp) or is declared through examples/creature_demo/
 * creature.lua's kf.screen("home") group (KF_HOME_SCREEN=lua, the
 * default) -- either way it registers first and holds index 0. The old
 * bars-and-buttons LVGL Home (kf_pet_screen.cpp) is UNREACHABLE from a
 * running build through this file, on purpose, not by accident of
 * something getting missed -- it still exists and is still exercised
 * directly by headless_main.cpp's own pet_screen_check, unaffected by
 * anything in this file. Nothing outside this file should call
 * kf_pet_screen_init()/kf_pet_info_screen_init()/kf_creature_screen_init()
 * directly, except that same check and creature_screen_check, which
 * deliberately keep exercising each screen in isolation. */
void kf_screen_nav_init(void);

/* Wires kf.screen(name)/screen:show() (sdk/lua/kf_lua_scene.cpp) up to
 * this registry via function pointers -- see this header's own top
 * comment for why it cannot be a plain #include. Call once, before any
 * script that might call kf.screen() runs; kf_screen_nav_init() already
 * does this as its first step for the interactive build and for
 * headless_main.cpp's run_screen_nav_check(). A check that never calls
 * kf_screen_nav_init() at all (run_screen_group_check(), to stay clear of
 * the pet session, LVGL and Home/Info that init also brings up) must call
 * this itself before loading a script that uses kf.screen(). Idempotent --
 * safe to call more than once, later calls simply overwrite the same two
 * pointers with the same values. */
void kf_screen_nav_install_lua_hooks(void);

/* Reads this frame's MENU/B edges and switches screens if either fired,
 * then calls whichever screen is now active's own per-frame function --
 * only the active one; the inactive screens sit untouched until shown
 * again, the same "why redraw what nobody can see" reasoning kf_pet_
 * screen.h's own per-frame contract already follows. A null `update` (see
 * kf_screen_nav_register() above) is simply skipped, not called through.
 * `dt_ms` is passed straight through to the active screen's update. Call
 * once per frame, in the exact same slot sdl_main.cpp used to call
 * kf_pet_screen_update() directly -- after kf_pet_session_frame() has
 * applied this frame's elapsed time, before kf_lvgl_port_pump() redraws
 * and flushes. See ADR 0017's frame-ordering note, still the same
 * requirement now that more than one screen exists. See also
 * kf_screen_nav_wants_lvgl() below: the caller must guard its own
 * kf_lvgl_port_pump() call with it, since pumping LVGL while a non-LVGL
 * screen is active would render nothing useful and cost a frame's worth
 * of LVGL bookkeeping for it. */
void kf_screen_nav_frame(uint32_t dt_ms);

/* Whether the currently active screen wants kf_lvgl_port_pump() called
 * this frame at all -- true for any screen with an LVGL root (Info,
 * today), false for one that draws straight into the framebuffer itself
 * (Home) or is declared through the retained scene (any kf.screen()
 * group). Callers (sdl_main.cpp, ports/esp32/main/app_main.cpp) must guard
 * their kf_lvgl_port_pump() call with this: pumping LVGL while Home is
 * active would run lv_timer_handler() over an empty widget tree for no
 * benefit, and -- the actual hazard, not just waste -- risks LVGL's own
 * idle "nothing changed" fast path deciding there is nothing to flush and
 * never repainting Info's stale pixels the next time IT becomes active.
 * See kf_screen_nav_show()'s own comment for the other half of that
 * hazard (the one this predicate alone does not cover) and
 * docs/architecture/adr-0017-pet-screen.md:143-188 for the failure shape
 * both exist to prevent. */
bool kf_screen_nav_wants_lvgl(void);

/* THE single place that switches "which screen is showing" -- both the
 * MENU/B edge inside kf_screen_nav_frame() above and screen:show()
 * (kf_lua_scene.cpp, through the hook installed by kf_screen_nav_install_
 * lua_hooks()) call this and nothing else. ADR 0044's whole design note:
 * two independent paths each deciding to switch screens is exactly the
 * class of bug ADR 0042 documented and ADR 0043 fixed (stale pixels from
 * the previous screen, surviving on some transition orders and not
 * others) -- one function, one owner, closes it by construction rather
 * than by convention.
 *
 * Does three things, in order, for any valid index: (1) tells the Lua
 * scene binding this index is now active, so it can hide every OTHER
 * kf.screen() group's objects and show this one's, applying its stored
 * background colour if it ever set one (a no-op if no Lua group is
 * registered under this index -- Info, still LVGL as of this task, has
 * none); (2) if this screen has an LVGL root, invalidates and loads it;
 * (3) otherwise, if this is Home, runs Home's own screen-specific entry
 * hook (kf_creature_screen_enter() or kf_lua_home_screen_enter(),
 * matching this build's KF_HOME_SCREEN) for whatever it does beyond
 * scene-group bookkeeping -- the error banner, the creature presenter's
 * animation cursor, nothing (1) already covers on its own. Any OTHER
 * screen with no LVGL root (a kf.screen() group besides Home) needs
 * nothing further: (1) already repainted it. Silently does nothing for an
 * index that is negative or >= kf_screen_nav_count(). */
void kf_screen_nav_show(int index);

/* ---------------------------------------------------------------------
 * DEBUG/TEST ONLY below this line, the same status kf_pet_session.h's own
 * "DEBUG ONLY" section has: not part of the gameplay surface, not called
 * by the interactive build. Their purpose is letting a headless check (or,
 * later, a debug-window button) drive screen switching directly rather
 * than fighting headless_input.cpp's single shared, frame-indexed button
 * script -- see headless_main.cpp's run_screen_nav_check() for the actual
 * caller. Same effect as a real MENU/B press, just callable without one.
 * Both route through kf_screen_nav_show() too -- see that function's own
 * comment; there is no second switching path here, only two names for the
 * same edges a real button press produces.
 * --------------------------------------------------------------------- */

/* Same effect as a MENU edge: advances to the next screen, wrapping back
 * to Home. A no-op if no screen has been registered yet. */
void kf_screen_nav_debug_advance(void);

/* Same effect as a B edge: jumps straight back to Home (index 0). A no-op
 * if Home is already active, matching the real button's behaviour, or if
 * no screen has been registered yet. */
void kf_screen_nav_debug_home(void);

/* Which screen index is active right now. For test assertions only --
 * nothing in the interactive build reads this; see kf_screen_nav_name()
 * above for the one debug/interactive callers should use instead. */
int kf_screen_nav_debug_index(void);

#endif /* KF_SCREEN_NAV_H */
