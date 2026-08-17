/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 */

#include "kf_screen_nav.h"

/* kf_lua_home_screen.h is included UNCONDITIONALLY, not only under
 * KF_HOME_SCREEN_LUA, even though only the lua build calls its Home
 * entry points: it also declares kf_lua_info_screen_frame(), which
 * kf_screen_nav_init() below registers for Info in BOTH builds. Info has
 * been Lua-declared regardless of which Home is compiled in since ADR
 * 0045, and kf_lua_home_screen.cpp is in this library's source list
 * unconditionally (simulator/CMakeLists.txt), so the definition was
 * always there -- only the declaration was hidden. Guarding this include
 * is what made KF_HOME_SCREEN=cpp stop compiling at all
 * ("'kf_lua_info_screen_frame': undeclared identifier"), unnoticed
 * because no CI job builds that flag. */
#include "kf_lua_home_screen.h"

#ifndef KF_HOME_SCREEN_LUA
#include "kf_creature_screen.h"
#endif

/* Allowed direction only: whichever library holds this file already
 * depends on kamiframe_lua_port (simulator/CMakeLists.txt), so this file
 * can include this header freely. The reverse include (kf_lua_scene.cpp
 * including THIS header) is what is forbidden -- see kf_lua_scene.h's own
 * comment on the function-pointer boundary that exists because of it. */
#include "kf_lua_scene.h"
#include "kf_lua_settings_screen.h"

#include "kf/app.h"
#include "kf/hal/log.h"

#include <cstdio>
#include <cstring>

namespace {

constexpr const char *TAG = "screen-nav";

/* Longest screen name this table stores, not counting the NUL -- generous
 * for "home"/"info"/"settings" and any scratch/example name a third party
 * might pick; truncated silently past this like every other bounded copy
 * in this codebase (kf/scene.h's sprite/text names use the same
 * "clamp, do not crash" rule). */
constexpr size_t kScreenNameMax = 23;

/* One entry per registered screen -- see kf_screen_nav.h's own header
 * comment (the "ADR 0044: A REGISTRY, NOT A SCENE-PER-SCREEN" section) for
 * what this struct deliberately does NOT know: which kf_scene_ids belong
 * to a screen, or its background colour. That bookkeeping lives entirely
 * on the other side of kf_lua_scene_activate_screen(), called from
 * kf_screen_nav_show() below.
 *
 * `update`: called only while that screen is the active one, every frame;
 * may be nullptr -- most kf.screen()-declared screens have no C-side
 * per-frame behaviour of their own (see kf_screen_nav_register()'s own
 * comment). Home is index 0 on purpose -- B always jumps there, and
 * kf_screen_nav_init() registers it first. ADR 0045 dropped this struct's
 * `lv_obj_t *root` field: every screen this file knows about is now
 * declared through the retained scene (kf/scene.h), none through LVGL. */
struct ScreenEntry {
    char name[kScreenNameMax + 1] = {};
    void (*update)(uint32_t dt_ms) = nullptr;
};

ScreenEntry g_screens[KF_SCREEN_NAV_MAX_SCREENS];
size_t g_screen_count = 0;
size_t g_active = 0;

/* Set once by kf_screen_nav_init() below, from kf_screen_nav_register()'s
 * own return value -- NOT hardcoded to 2, even though "home", "info",
 * "settings" registering in that order today makes 2 the actual number:
 * show() below needs to know which index is Settings without assuming
 * registration order stays fixed forever, the same reasoning g_active
 * already follows by being an index rather than a name everywhere else in
 * this file. -1 (kSettingsIndexUnset) until kf_screen_nav_init() runs. */
constexpr int kSettingsIndexUnset = -1;
int g_settings_index = kSettingsIndexUnset;

void show(size_t index) {
    g_active = index;

    /* Releases every OTHER kf.screen() group's objects back to the scene,
     * restores this index's group (if one is registered under it) and
     * re-applies its stored background colour, then force-repaints the
     * retained scene and commits if anything has ever been declared. See
     * kf_lua_scene.h's own comment on kf_lua_scene_activate_screen().
     *
     * The NAME goes with the index, and this is the only place that knows
     * it: a screen registered here without a matching kf.screen() group
     * (the C++ Home under KF_HOME_SCREEN=cpp) is invisible to the Lua
     * binding's own bookkeeping, so kf.active_screen() can only be right
     * in both builds if the registry tells it the name rather than the
     * binding inferring one. */
    kf_lua_scene_activate_screen(static_cast<int>(index),
                                 g_screens[index].name);

    if (index == 0u) {
        /* Home is still special beyond the scene-group bookkeeping above:
         * the error banner and the creature presenter's animation cursor
         * (KF_HOME_SCREEN=lua), or a full framebuffer repaint from scratch
         * (KF_HOME_SCREEN=cpp) -- see kf_lua_home_screen.h's/kf_creature_
         * screen.h's own comments on their entry points for why. Any
         * OTHER screen (Info, or a future kf.screen() group besides Home)
         * needs nothing further here: the activate call above already
         * repainted it, and it has no C++-side entry hook of its own. */
#ifdef KF_HOME_SCREEN_LUA
        kf_lua_home_screen_enter();
#else
        kf_creature_screen_enter();
#endif
    } else if (g_settings_index != kSettingsIndexUnset &&
               index == static_cast<size_t>(g_settings_index)) {
        /* Task 4 of the screens/clock/sleep plan: Settings is special
         * beyond the scene-group bookkeeping above too, the same way Home
         * is -- resetting its four-field editor from whatever the wall
         * clock currently says, every time the screen is entered, so a
         * cancelled edit never resurfaces the next time the owner opens it.
         * kf_lua_scene_activate_screen() above already repainted the
         * screen's pixels; this only resets the C++-side edit state
         * kf_lua_settings_screen_frame() reads and writes from here on. */
        kf_lua_settings_screen_enter();
    }
}

void advance() {
    if (g_screen_count == 0u) {
        return;
    }
    show((g_active + 1u) % g_screen_count);
}

void go_home() {
    if (g_screen_count == 0u) {
        return;
    }
    if (g_active != 0u) {
        show(0u);
    }
}

} // namespace

int kf_screen_nav_register(const char *name, void (*update)(uint32_t dt_ms)) {
    for (size_t i = 0; i < g_screen_count; ++i) {
        if (std::strcmp(g_screens[i].name, name) == 0) {
            /* Fetch, not create: the first registration of this name wins
             * the update callback, so a later call (typically kf.screen()
             * fetching what kf_screen_nav_init() already registered for
             * Home or Info) must not silently blank it out with whatever
             * it happened to pass -- see this function's own header
             * comment in kf_screen_nav.h. */
            return static_cast<int>(i);
        }
    }
    if (g_screen_count >= KF_SCREEN_NAV_MAX_SCREENS) {
        KF_LOGE(TAG,
                "cannot register screen '%s' -- already at the "
                "%d-screen limit (KF_SCREEN_NAV_MAX_SCREENS)",
                name, KF_SCREEN_NAV_MAX_SCREENS);
        return -1;
    }
    const size_t index = g_screen_count++;
    std::snprintf(g_screens[index].name, sizeof(g_screens[index].name), "%s",
                  name);
    g_screens[index].update = update;
    return static_cast<int>(index);
}

const char *kf_screen_nav_name(int index) {
    if (index < 0 || static_cast<size_t>(index) >= g_screen_count) {
        return "?";
    }
    return g_screens[static_cast<size_t>(index)].name;
}

int kf_screen_nav_count(void) { return static_cast<int>(g_screen_count); }

void kf_screen_nav_install_lua_hooks(void) {
    kf_lua_scene_set_screen_nav(kf_screen_nav_register, kf_screen_nav_show);
}

void kf_screen_nav_init(void) {
    /* Wired up FIRST: creature.lua's own kf.screen("home")/kf.screen("info")
     * calls (inside kf_lua_port_init(), which runs after this whole
     * function returns) must find the hook already installed. Idempotent,
     * so calling it again from a test that also wants Home/Info brought up
     * is harmless -- see this function's own header comment in
     * kf_screen_nav.h. */
    kf_screen_nav_install_lua_hooks();

    /* Home is the creature screen: it owns the framebuffer directly
     * (KF_HOME_SCREEN=cpp) or is declared through creature.lua's own
     * kf.screen("home") group (KF_HOME_SCREEN=lua, the default) -- either
     * way it registers FIRST here and holds index 0, so B-jumps-home and
     * kf_screen_nav_debug_home() keep meaning what they mean regardless
     * of which build this is. kf_creature_screen_init()/kf_lua_home_
     * screen_init() already paint the field's background themselves (see
     * each one's own header comment), so nothing extra is needed here.
     *
     * kf_pet_screen.cpp (bars + care-action buttons on an LVGL screen,
     * only built under -DKF_ENABLE_LVGL=ON) is NOT initialised here any
     * more and is therefore unreachable from a running build -- still
     * exercised directly by headless_main.cpp's own run_pet_screen_check(),
     * unaffected by any of this. */
#ifdef KF_HOME_SCREEN_LUA
    /* Task 5 of the Lua game-layer plan: creature.lua itself declares
     * every scene object it will ever create (through its own
     * kf.screen("home") group, ADR 0044) once kf_lua_port_init() loads
     * and runs the script's top-level code, AFTER this function returns.
     * kf_lua_home_screen_init() only flips the runtime flag the script
     * reads to decide it should have declared them at all
     * (kf.home_screen_active()) and creates the error banner. */
    kf_lua_home_screen_init();
    kf_screen_nav_register("home", kf_lua_home_screen_frame);
#else
    kf_creature_screen_init();
    kf_screen_nav_register("home", kf_creature_screen_frame);
#endif

    /* Info (ADR 0045) is declared entirely in Lua now -- creature.lua's
     * own kf.screen("info") call, unconditional (unlike Home, it does not
     * check kf.home_screen_active()), will fetch this same index moments
     * from now. Registered here, ahead of that call, purely so it has a
     * per-frame update: kf_lua_info_screen_frame() runs the shared Lua
     * VM's on_info_frame() -- Info's OWN dedicated entry point, kept
     * deliberately separate from Home's on_home_frame() and the generic
     * on_frame() the main loop calls every frame regardless of active
     * screen (see kf_lua_port.h's own comment on kf_lua_port_home_frame()
     * for why this separation matters: it is what a real hardware bug
     * traced back to) -- so Info's text objects keep refreshing (the
     * ticking "time in stage" duration, in particular) while Info is the
     * active screen. Registering it here rather than letting kf.screen(
     * "info") register it bare (update = nullptr) is the same "pre-
     * register with the real per-frame function" move kf_screen_nav.h's
     * own header comment already describes for Home. */
    kf_screen_nav_register("info", kf_lua_info_screen_frame);

    /* Settings (Task 4): registered THIRD, so MENU cycles HOME -> INFO ->
     * SETTINGS -> HOME, per the plan's own requirement. kf_lua_settings_
     * screen_init() first, matching Home's own order above (init creates
     * this screen's error banner before anything can possibly show it),
     * then register with a real per-frame update the same way Home and
     * Info both already do -- creature.lua's own kf.screen("settings")
     * call (unconditional, like Info's, not gated on kf.home_screen_
     * active()) will fetch this same index moments from now. The return
     * value is g_settings_index, not discarded like Info's: show() above
     * needs to know which index this is, to call kf_lua_settings_screen_
     * enter() on the way in -- Info needs no such hook, so it never
     * bothered keeping its own index around. */
    kf_lua_settings_screen_init();
    g_settings_index =
        kf_screen_nav_register("settings", kf_lua_settings_screen_frame);

    g_active = 0;
    /* Home is active from this moment, but nothing has gone through
     * show() to say so, and under KF_HOME_SCREEN=cpp nothing ever will
     * until the player navigates away and back -- Home has no kf.screen()
     * group in that build for a later activation to name. Recording the
     * name here is what makes kf.active_screen() answer "home" from boot
     * in BOTH builds, which is what the play picker's own UP gate reads.
     * Safe this early: no group is registered yet (creature.lua's top-
     * level code has not run), so this records the name, finds nothing to
     * release or restore, and returns without touching a pixel. */
    kf_lua_scene_activate_screen(0, g_screens[0].name);
    KF_LOGI(TAG, "%d screens ready, starting on Home", kf_screen_nav_count());
}

void kf_screen_nav_frame(uint32_t dt_ms) {
    const uint32_t pressed = kf_app_buttons_pressed();
    if (pressed & KF_BTN_MENU) {
        advance();
    } else if (pressed & KF_BTN_B) {
        go_home();
    }
    if (g_active < g_screen_count && g_screens[g_active].update != nullptr) {
        g_screens[g_active].update(dt_ms);
    }
}

void kf_screen_nav_show(int index) {
    if (index < 0 || static_cast<size_t>(index) >= g_screen_count) {
        return;
    }
    show(static_cast<size_t>(index));
}

void kf_screen_nav_debug_advance(void) { advance(); }

void kf_screen_nav_debug_home(void) { go_home(); }

int kf_screen_nav_debug_index(void) { return static_cast<int>(g_active); }
