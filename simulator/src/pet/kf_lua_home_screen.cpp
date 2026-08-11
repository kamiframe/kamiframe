/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * See kf_lua_home_screen.h.
 */

#include "kf_lua_home_screen.h"

#include "kf_creature_presenter.h"
#include "kf_error_banner.h"
#include "kf_home_screen_input.h"
#include "kf_pet_session.h"

#include "kf_lua_port.h"
#include "kf_lua_scene.h"

#include "kf/app.h"
#include "kf/scene.h"

namespace {
kf_scene_id g_error_banner_id = 0;
} // namespace

void kf_lua_home_screen_init(void) {
    /* Belt-and-suspenders, not the primary mechanism: kf_lua_port_init()
     * (sdl_main.cpp/app_main.cpp, called AFTER kf_screen_nav_init(), which
     * is what calls this) already seeds this same flag to true from this
     * build's KF_HOME_SCREEN_LUA compile define. This call sets the same
     * value early -- harmless either way -- so this file's own reason for
     * needing it is documented at its own call site rather than only
     * inside kf_lua_port_init()'s ifdef. */
    kf_lua_port_set_home_screen_active(true);
    g_error_banner_id = kf_error_banner_create();
    /* Matches kf_creature_screen_enter()'s own dt_ms==0 presenter advance
     * exactly, and for the identical reason: kf_creature_presenter_
     * advance()'s "did the resolved sprite name just change" check (kf_
     * creature_presenter.cpp's resolve_and_declare()) fires on the very
     * FIRST call regardless of dt_ms (g_last_requested_name starts empty),
     * and that first call also resets the animation accumulator to 0 as
     * part of "treat this as a fresh pose". Skipping this and letting the
     * script's first on_frame() -- necessarily a REAL, non-zero dt_ms --
     * be that first call would wipe out the very ms it just accumulated,
     * a one-time animation-timing offset the C++ screen never has because
     * its own first call is this same harmless dt_ms==0 one. Found by
     * run_lua_vs_cpp_screen_check() diverging at frame 3 before this fix
     * -- see that check's own header comment. */
    kf_creature_presenter_advance(kf_pet_session_state(), 0);
}

void kf_lua_home_screen_enter(void) {
    /* No re-declaration, unlike kf_creature_screen_enter(): creature.lua's
     * objects were declared once, at script load, and this call's whole
     * job is to make sure they get repainted without disturbing any of
     * them -- see this function's own header comment and kf/scene.h's
     * kf_scene_force_repaint(). Nothing about the creature's own scene
     * object changes here, so -- unlike the cpp path's matching call to
     * kf_creature_presenter_force_anim_restart() on re-entry -- there is
     * no animation cursor to reset: it is the SAME object, mid-cycle
     * exactly where it was, not a freshly recreated one that needs to be
     * told its own history does not apply.
     *
     * Commits IMMEDIATELY, not on the next per-frame kf_lua_home_screen_
     * frame() call: kf_creature_screen_enter() (the cpp path this mirrors)
     * repaints synchronously too, inside kf_screen_nav.cpp's show(), so a
     * caller that inspects the panel right after switching screens --
     * screen_nav_check does exactly this -- sees the repaint on either
     * build. Guarded the same way kf_lua_home_screen_frame() guards its
     * own commit: a script that never declared anything must not force a
     * commit against a scene with nothing in it. */
    kf_scene_force_repaint();
    if (kf_lua_scene_declared_anything()) {
        kf_scene_commit();
    }
}

void kf_lua_home_screen_frame(uint32_t dt_ms) {
    const kf_pet_state *pet = kf_pet_session_state();
    kf_home_screen_handle_care_buttons(pet, kf_app_buttons_pressed());
    /* Advance the shared presenter BEFORE running the script's on_frame(),
     * so creature.x()/y()/sprite()/mirrored()/frame() (sdk/lua/kf_lua_
     * port.cpp) read this frame's fresh result, not last frame's -- the
     * same "advance, then declare" order kf_creature_screen_frame() (the
     * C++ path) has. Skipped entirely once pet->dead, to match that
     * function's own death branch EXACTLY: it returns before ever
     * advancing the presenter once dead, so the wander freezes rather than
     * continuing to walk (and consume kf_rng draws) somewhere neither
     * screen shows. Deliberately lives HERE, not inside kf_lua_port_
     * frame(): that file is generic Lua glue shared by every script this
     * codebase loads (including the proof scripts, which bring up no pet
     * session at all), and it must not assume a pet session exists just
     * because SOME OTHER script (this one) needs one -- found via
     * run_lua_vs_cpp_screen_check() diverging at the revive event before
     * the death gate existed, and via lua_determinism_check/lua_draw_check
     * aborting when this lived in kf_lua_port_frame() unconditionally. */
    if (!pet->dead) {
        kf_creature_presenter_advance(pet, dt_ms);
    }
    kf_lua_port_frame(dt_ms);
    kf_error_banner_update(g_error_banner_id);
    /* Guarded exactly like sdl_main.cpp/app_main.cpp's own pre-Task-5
     * commit call: a script that never declares anything (the narration-
     * only demo, or a script that failed to load at all) must not force a
     * commit against an empty scene -- see kf_lua_scene.h's own comment on
     * kf_lua_scene_declared_anything() for why. The error banner's own
     * kf_scene_add_text() call above already makes this true from the very
     * first frame of a lua-mode build, since creature.lua under KF_HOME_
     * SCREEN=lua always declares a background at its own top level -- but
     * the guard stays because this file must not assume that of every
     * future script. */
    if (kf_lua_scene_declared_anything()) {
        kf_scene_commit();
    }
}

void kf_lua_info_screen_frame(uint32_t dt_ms) {
    /* No buttons, no presenter to advance -- Info has neither.
     * kf_lua_port_info_frame(), NOT kf_lua_port_frame(): see that
     * function's own header comment in kf_lua_port.h for why calling the
     * shared on_frame() from here is the one thing this must NOT do (it
     * mutates Home's own scene objects unconditionally, which un-hides
     * them on top of Info). on_info_frame() is what pushes fresh pet.*
     * values into Info's own kf.screen("info") text objects, declared in
     * creature.lua. Same error banner every screen shares, and the same
     * commit -- see this file's own header comment on why Info needs a
     * registered update at all. */
    kf_lua_port_info_frame(dt_ms);
    kf_error_banner_update(g_error_banner_id);
    if (kf_lua_scene_declared_anything()) {
        kf_scene_commit();
    }
}
