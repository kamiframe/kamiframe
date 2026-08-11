/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * Switches which screen is on top -- Home and Info so far, more later, any
 * number up to KF_SCREEN_NAV_MAX_SCREENS -- and owns the ONE input mapping
 * that does it: MENU advances to the next screen, wrapping back to Home; B
 * jumps straight back to Home from anywhere. See ADR 0022 for the original
 * two-screen mechanism, ADR 0044 for why it became register-by-name rather
 * than staying a fixed two-entry array, and ADR 0045 for why this file no
 * longer has anything to do with LVGL at all: Info moved to a kf.screen()
 * group over the retained scene (kf/scene.h), the same mechanism Home
 * already used, so the LVGL-specific half of this file's old job --
 * lv_screen_load()/lv_obj_invalidate() on a screen's LVGL root -- has no
 * caller left. This file now owns exactly one thing: WHICH name is
 * showing, for screens that are ALL declared through the retained scene.
 *
 * Deliberately reads kf_app_buttons_pressed() directly, the same debounced
 * edge state kf/app.cpp's own KF_BTN_MENU-toggles-the-HUD code reads --
 * NOT through kf_lvgl_input.cpp's keypad indev (relevant only when this
 * build is compiled with -DKF_ENABLE_LVGL=ON; see ADR 0045). Screen
 * switching and LVGL's own keypad-group focus system are kept orthogonal
 * on purpose: which screen is loaded has nothing to do with which widget
 * inside some OTHER, still-LVGL screen currently has keypad focus.
 *
 * ADR 0044: A REGISTRY, NOT A SCENE-PER-SCREEN. There is one retained scene
 * (kf/scene.h, KF_SCENE_MAX_OBJECTS slots) shared by every screen; a Lua
 * screen (sdk/lua/kf_lua_scene.cpp's "kf.Screen" group) is a NAME plus the
 * list of scene object ids declared through it, not a scene of its own.
 * This file owns exactly one thing about that: WHICH name is showing. It
 * does not know which kf_scene_ids belong to which name -- that bookkeeping
 * lives in kf_lua_scene.cpp, on the other side of a function-pointer
 * boundary (kf_screen_nav_install_lua_hooks() below) rather than a #include,
 * because kamiframe_lua_port must not link against whatever library holds
 * this file (simulator/CMakeLists.txt's own comment on that link direction
 * -- a hard dependency direction, not a preference) while kf.screen(name)
 * still has to reach kf_screen_nav_register()/_show() to register and
 * switch through the one place that is allowed to.
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

/* Brings up Home (whichever implementation this build wires up) and
 * registers Info's own per-frame update -- see kf_screen_nav_init.cpp's
 * own comment for why Info needs a registered update at all when its
 * objects are declared entirely in Lua. Call once, after
 * kf_pet_session_init() -- Home's own init function needs the pet session
 * ready the same way it always has.
 *
 * Home is the creature screen: it draws straight into the framebuffer
 * (KF_HOME_SCREEN=cpp) or is declared through examples/creature_demo/
 * creature.lua's kf.screen("home") group (KF_HOME_SCREEN=lua, the
 * default) -- either way it registers FIRST here and holds index 0, so
 * B-jumps-home and kf_screen_nav_debug_home() keep meaning what they mean.
 * The old bars-and-buttons LVGL Home (kf_pet_screen.cpp, only built under
 * -DKF_ENABLE_LVGL=ON) is UNREACHABLE from a running build through this
 * file, on purpose -- it still exists and is still exercised directly by
 * headless_main.cpp's own pet_screen_check, unaffected by anything here. */
void kf_screen_nav_init(void);

/* Wires kf.screen(name)/screen:show() (sdk/lua/kf_lua_scene.cpp) up to
 * this registry via function pointers -- see this header's own top
 * comment for why it cannot be a plain #include. Call once, before any
 * script that might call kf.screen() runs; kf_screen_nav_init() already
 * does this as its first step for the interactive build and for
 * headless_main.cpp's run_screen_nav_check(). A check that never calls
 * kf_screen_nav_init() at all (run_screen_group_check(), to stay clear of
 * the pet session and Home/Info) must call this itself before loading a
 * script that uses kf.screen(). Idempotent -- safe to call more than
 * once, later calls simply overwrite the same two pointers with the same
 * values. */
void kf_screen_nav_install_lua_hooks(void);

/* Reads this frame's MENU/B edges and switches screens if either fired,
 * then calls whichever screen is now active's own per-frame function --
 * only the active one; the inactive screens sit untouched until shown
 * again. A null `update` (see kf_screen_nav_register() above) is simply
 * skipped, not called through. `dt_ms` is passed straight through to the
 * active screen's update. Call once per frame, after
 * kf_pet_session_frame() has applied this frame's elapsed time -- see
 * ADR 0017's frame-ordering note, still the same requirement now that
 * more than one screen exists. */
void kf_screen_nav_frame(uint32_t dt_ms);

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
 * Does two things, in order, for any valid index: (1) tells the Lua scene
 * binding this index is now active, so it can hide every OTHER
 * kf.screen() group's objects and show this one's, applying its stored
 * background colour if it ever set one (a no-op if no Lua group is
 * registered under this index); (2) if this is Home, runs Home's own
 * screen-specific entry hook (kf_creature_screen_enter() or
 * kf_lua_home_screen_enter(), matching this build's KF_HOME_SCREEN) for
 * whatever it does beyond scene-group bookkeeping -- the error banner,
 * the creature presenter's animation cursor, nothing (1) already covers
 * on its own. Any OTHER screen (Info, or a future kf.screen() group
 * besides Home) needs nothing further: (1) already repainted it. Silently
 * does nothing for an index that is negative or >= kf_screen_nav_count(). */
void kf_screen_nav_show(int index);

/* ---------------------------------------------------------------------
 * DEBUG/TEST ONLY in the sense of "not part of the gameplay surface, not
 * reachable from a real MENU/B button press" -- NOT in the sense of
 * "unused by the interactive build". sdl_debug_window.cpp (compiled
 * unconditionally into kamiframe-sim, no #ifdef) calls
 * kf_screen_nav_debug_advance() and kf_screen_nav_debug_index() from its
 * own debug window, alongside headless_main.cpp's run_screen_nav_check()
 * and friends. Same effect as a real MENU/B press, just callable without
 * one. Both route through kf_screen_nav_show() too -- see that function's
 * own comment; there is no second switching path here, only two names for
 * the same edges a real button press produces. Deleting these breaks the
 * simulator's debug window, not just a headless check.
 * --------------------------------------------------------------------- */

/* Same effect as a MENU edge: advances to the next screen, wrapping back
 * to Home. A no-op if no screen has been registered yet. */
void kf_screen_nav_debug_advance(void);

/* Same effect as a B edge: jumps straight back to Home (index 0). A no-op
 * if Home is already active, matching the real button's behaviour, or if
 * no screen has been registered yet. */
void kf_screen_nav_debug_home(void);

/* Which screen index is active right now. Written for test assertions,
 * but sdl_debug_window.cpp also reads it directly (to label the active
 * screen in its own window) -- see kf_screen_nav_name() above for the
 * human-readable name callers usually want alongside it. */
int kf_screen_nav_debug_index(void);

#endif /* KF_SCREEN_NAV_H */
