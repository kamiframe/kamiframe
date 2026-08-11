/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 */

#include "kf_screen_nav.h"

#include "kf_pet_info_screen.h"

#ifdef KF_HOME_SCREEN_LUA
#include "kf_lua_home_screen.h"
#else
#include "../pet/kf_creature_screen.h"
#endif

/* Allowed direction only: kamiframe_lvgl_port already depends on
 * kamiframe_lua_port (simulator/CMakeLists.txt), so this file can include
 * this header freely. The reverse include (kf_lua_scene.cpp including
 * THIS header) is what is forbidden -- see kf_lua_scene.h's own comment
 * on the function-pointer boundary that exists because of it. */
#include "../../../sdk/lua/kf_lua_scene.h"

#include "kf/app.h"
#include "kf/hal/log.h"

#include <lvgl.h>

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
 * `root`: what gets passed to lv_screen_load() -- nullptr for a screen
 * that draws straight into the framebuffer or the retained scene instead
 * of through an LVGL widget tree (Home, and any kf.screen() group).
 * `update`: called only while that screen is the active one, every frame;
 * may be nullptr -- most kf.screen()-declared screens have no C-side
 * per-frame behaviour of their own (see kf_screen_nav_register()'s own
 * comment). Home is index 0 on purpose -- B always jumps there, and
 * kf_screen_nav_init() registers it first -- matching kf_pet_screen.h's
 * original "the screen a running build shows by default" role, unchanged
 * from every prior slice even though a different screen now fills it. */
struct ScreenEntry {
    char name[kScreenNameMax + 1] = {};
    lv_obj_t *root = nullptr;
    void (*update)(uint32_t dt_ms) = nullptr;
};

ScreenEntry g_screens[KF_SCREEN_NAV_MAX_SCREENS];
size_t g_screen_count = 0;
size_t g_active = 0;

/* kf_pet_info_screen_update() takes no arguments (nothing about Info is
 * time-based); this just drops dt_ms on the floor so it fits ScreenEntry's
 * one shared update() shape. */
void update_info_screen(uint32_t /*dt_ms*/) { kf_pet_info_screen_update(); }

void show(size_t index) {
    g_active = index;

    /* Step 1, unconditionally, for every switch: Lua screen-group
     * bookkeeping. Hides every OTHER kf.screen() group's objects, shows
     * this index's group (if one is registered under it) and re-applies
     * its stored background colour, then force-repaints the retained
     * scene and commits if anything has ever been declared. A no-op,
     * changing nothing, if no group is registered under `index` -- Info,
     * still LVGL as of this task, is exactly that case. See kf_lua_
     * scene.h's own comment on kf_lua_scene_activate_screen(). */
    kf_lua_scene_activate_screen(static_cast<int>(index));

    if (g_screens[index].root != nullptr) {
        /* Switching TO an LVGL screen (Info, today): LVGL's own dirty
         * tracking still believes this screen's pixels are exactly what
         * it last flushed, even though the creature screen or the
         * retained scene has spent however long drawing over them since
         * -- LVGL has no way to know that happened, since it never went
         * through LVGL's draw path at all. Left alone, the coming
         * kf_lvgl_port_pump() would only redraw whatever LVGL's OWN
         * widget tree thinks changed, which can easily be nothing, and
         * leave debris on screen permanently.
         *
         * Both calls below are load-bearing, for two DIFFERENT reasons,
         * not one -- this is not belt-and-suspenders:
         *
         * - lv_obj_invalidate() here is a no-op the very FIRST time this
         *   runs: it returns early for an object that is not the active
         *   screen (lv_obj_pos.c), and Info is not active yet at this
         *   point in the call. That first full repaint instead comes from
         *   lv_screen_load() -> scr_load_internal()'s OWN trailing
         *   lv_obj_invalidate() call (lv_display.c), made once Info
         *   actually is the active screen.
         * - On every LATER switch back to Info, lv_screen_load()
         *   early-returns immediately when asked to load the screen that
         *   is already active (lv_display.c) -- scr_load_internal() never
         *   runs again, so its trailing invalidate never fires either.
         *   The explicit call here is what forces the repaint on THAT
         *   path.
         *
         * So the explicit call earns its keep on the second and later
         * visits to a given screen; the first visit is carried by
         * lv_screen_load()'s own internals. Order between the two calls
         * does not matter -- both just flag the object dirty for the next
         * kf_lvgl_port_pump(), which has not run yet either way. */
        lv_obj_invalidate(g_screens[index].root);
        lv_screen_load(g_screens[index].root);
        return;
    }

    if (index == 0u) {
        /* Home is still special beyond the scene-group bookkeeping step 1
         * already did: the error banner and the creature presenter's
         * animation cursor (KF_HOME_SCREEN=lua), or a full framebuffer
         * repaint from scratch (KF_HOME_SCREEN=cpp) -- see kf_lua_home_
         * screen.h's/kf_creature_screen.h's own comments on their entry
         * points for why. Any OTHER non-LVGL screen (a kf.screen() group
         * besides Home) needs nothing further here: step 1 above already
         * repainted it, and it has no C++-side entry hook of its own. */
#ifdef KF_HOME_SCREEN_LUA
        kf_lua_home_screen_enter();
#else
        kf_creature_screen_enter();
#endif
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
             * Home) must not silently blank it out with whatever it
             * happened to pass -- see this function's own header comment
             * in kf_screen_nav.h. */
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
    g_screens[index].root = nullptr;
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
    /* Wired up FIRST: creature.lua's own kf.screen("home") call (inside
     * kf_lua_port_init(), which runs after this whole function returns)
     * must find the hook already installed. Idempotent, so calling it
     * again from a test that also wants Home/Info brought up is harmless
     * -- see this function's own header comment in kf_screen_nav.h. */
    kf_screen_nav_install_lua_hooks();

    /* Home is the creature screen: it owns the framebuffer directly
     * (KF_HOME_SCREEN=cpp) or is declared through creature.lua's own
     * kf.screen("home") group (KF_HOME_SCREEN=lua, the default) -- either
     * way it registers FIRST here and holds index 0, so B-jumps-home and
     * kf_screen_nav_debug_home() keep meaning what they mean regardless
     * of which build this is. kf_creature_screen_init()/kf_lua_home_
     * screen_init() already paint the field's background themselves (see
     * each one's own header comment), the same "screen looks right from
     * its very first frame" convention kf_pet_screen_init()/kf_pet_info_
     * screen_init() already used, so nothing extra is needed here.
     *
     * kf_pet_screen.cpp (bars + care-action buttons on an LVGL screen) is
     * NOT initialised here any more and is therefore unreachable from a
     * running build -- deliberately, not an oversight: the stats band that
     * replaces it on the real screen is an explicit later plan (see
     * kf_screen_nav.h's own header comment). It still exists, still
     * compiles, and is still exercised directly by headless_main.cpp's
     * run_pet_screen_check(), unaffected by any of this. */
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

    kf_pet_info_screen_init();
    {
        const int info_index =
            kf_screen_nav_register("info", update_info_screen);
        if (info_index >= 0) {
            g_screens[static_cast<size_t>(info_index)].root =
                kf_pet_info_screen_root();
        }
    }

    g_active = 0;
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

bool kf_screen_nav_wants_lvgl(void) {
    return g_active < g_screen_count && g_screens[g_active].root != nullptr;
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
