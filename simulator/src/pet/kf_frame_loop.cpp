/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * WHY THIS FILE EXISTS. simulator/src/sdl/sdl_main.cpp and
 * ports/esp32/main/app_main.cpp each used to implement this same ~40-line,
 * order-sensitive sequence by hand. On 2026-08-12 a fix to the desktop copy
 * (the debug time multiplier has to drag the wall clock along with it, not
 * just the pet's delta) did not make it into the device copy, whose own
 * comment claimed the two mirrored each other while they had quietly
 * diverged. The bug was invisible on desktop and surfaced one power cycle
 * later, on hardware, as an offline fast-forward that under-credited
 * elapsed time by the exact amount the multiplier had run ahead of the RTC
 * -- fixed in d1ab8a7. See ADR 0058 for the full incident and for why this
 * extraction, not just a fix, is the actual remedy: two hand-maintained
 * copies of ordering-sensitive logic will drift again, given enough time
 * and a change to only one of them. This file is that logic, once.
 *
 * WHERE THE LINE IS DRAWN. Everything below is genuinely identical between
 * the two backends today -- not just similar, checked line by line against
 * both files as they stood at 14d350c before this extraction. What is NOT
 * here, because it is genuinely different, stays in each port:
 *   - kf_dbg_bridge_frame() (device only, and runs BEFORE kf_app_frame(),
 *     i.e. before this function is ever reached at all -- see
 *     app_main.cpp's own comment on why that ordering matters).
 *   - The debug window's own click/drag handling (desktop only) -- this
 *     file exposes exactly one slot for it, kf_frame_loop_hooks::
 *     after_pet_session, in the exact position sdl_main.cpp called it from.
 *   - Timing the post-kf_app_frame() segment for kf_app_post_frame_us()
 *     (device only) -- app_main.cpp does this by timing its own call to
 *     kf_frame_loop_run(), from outside; nothing in here needs to know
 *     that measurement exists.
 *   - --frames N, the window title, and shutdown ordering (desktop only,
 *     nothing to do with the per-frame sequence itself).
 *
 * WHAT CHANGED IN THE PROCESS, DELIBERATELY. The wall-clock-set below now
 * checks `wall.valid` before calling kf_time_set_wall(), which is what
 * app_main.cpp already did and sdl_main.cpp did not. This was harmless on
 * desktop -- simulator/src/host/host_time.cpp's kf_time_init() always
 * leaves the simulated wall clock valid before the frame loop can run a
 * single iteration -- but it was still a latent gap: had that ever not been
 * true, sdl_main.cpp would have written a wall-clock value computed from a
 * garbage 0 epoch_seconds. Standardising on the device's more defensive
 * check costs desktop nothing and removes the gap everywhere at once.
 */

#include "kf_frame_loop.h"

#include "kf/hal/time.h"
#include "kf/scene.h"

#include "kf_lua_port.h"
#include "kf_lua_scene.h"
#include "kf_pet_session.h"
#include "kf_screen_nav.h"

#ifdef KF_ENABLE_LVGL
/* Relative path, not a flat include relying on either build's
 * INCLUDE_DIRS: this file is compiled from two different build systems
 * (simulator/CMakeLists.txt's kamiframe_screen_port library, and
 * ports/esp32/main/CMakeLists.txt's SRCS by relative path), and a path
 * resolved against this file's own real location on disk works identically
 * either way with nothing to wire up. simulator/src/lvgl/ is a sibling of
 * this file's own simulator/src/pet/, one level up then back down, on both
 * builds -- sdl_main.cpp already relies on the identical relative depth for
 * the identical reason. */
#include "../lvgl/kf_lvgl_port.h"
#endif

namespace {

/* Persists across calls, the same way sdl_main.cpp's local `last_frame_us`
 * and app_main.cpp's identical local used to -- there is exactly one frame
 * loop running per process, so one set of statics here is the right amount
 * of state, not a struct threaded through every caller for no benefit. 0
 * means "no previous frame yet", the same sentinel both original files
 * used to mean "this is the first call, report zero elapsed time". */
uint64_t g_last_frame_us = 0;

/* Sub-second remainder of (real_dt_ms * (multiplier - 1)) not yet folded
 * into a whole second handed to kf_time_set_wall() -- see the call site
 * below for why this has to be carried rather than truncated every frame. */
uint64_t g_extra_ms_carry = 0;

} // namespace

uint32_t kf_frame_loop_run(uint32_t multiplier, const kf_frame_loop_hooks *hooks) {
    const uint64_t now_us = kf_time_mono_us();
    const uint32_t real_dt_ms =
        g_last_frame_us == 0u
            ? 0u
            : static_cast<uint32_t>((now_us - g_last_frame_us) / 1000u);
    g_last_frame_us = now_us;

    /* The wall clock is dragged along with the multiplier, and this is NOT
     * the same thing as the pet delta below.
     *
     * Core evaluates the 22:00-07:00 night window against
     * kf_pet_state::last_advanced, which kf_pet_advance() carries forward
     * by the MULTIPLIED delta a few lines down. Lua's kf.hour() -- the
     * Settings clock, the drowsy cue, the on-screen clock -- reads
     * kf_time_wall() instead. Left alone, the wall clock ticks at 1x while
     * Core's races at up to 256x, so within seconds the creature is asleep
     * against a displayed clock that still says mid-afternoon.
     *
     * This deliberately does NOT make the multiplier affect animation:
     * kf_screen_nav_frame() and kf_lua_port_frame() below still get real
     * time, and neither reads the wall clock for timing. Only what
     * kf_time_wall() REPORTS changes.
     *
     * Whole seconds only, with the remainder carried in g_extra_ms_carry --
     * kf_time_set_wall() takes seconds, so accumulating the sub-second part
     * here is what stops 33ms frames at 2x from rounding away to nothing.
     *
     * wall.valid is checked before writing -- see this file's header
     * comment on why that check is here even though desktop's simulated
     * clock is always valid by the time this can run at all. */
    if (multiplier > 1u) {
        g_extra_ms_carry += static_cast<uint64_t>(real_dt_ms) * (multiplier - 1u);
        const uint64_t whole_seconds = g_extra_ms_carry / 1000ull;
        if (whole_seconds > 0u) {
            g_extra_ms_carry -= whole_seconds * 1000ull;
            const kf_wall_time wall = kf_time_wall();
            if (wall.valid) {
                kf_time_set_wall(wall.epoch_seconds +
                                  static_cast<int64_t>(whole_seconds));
            }
        }
    }

    /* kf_pet_session_frame() and kf_screen_nav_frame() both run before
     * kf_lvgl_port_pump(): the session needs to have applied this frame's
     * elapsed time before the active screen reads it. Under
     * -DKF_ENABLE_LVGL=ON this also keeps a still-LVGL screen from showing
     * last frame's numbers one frame behind -- ADR 0045 removed the guard
     * that used to make the pump call conditional on which screen was
     * active, since every screen either build can show is a kf.screen()
     * group over the retained scene now, so LVGL, when built in at all, has
     * nothing left to pump for; the call below is unconditional on which
     * screen is active, only on KF_ENABLE_LVGL itself.
     *
     * kf_pet_session_frame() gets the MULTIPLIED delta -- the pet's decay
     * is the thing the multiplier exists to speed up. kf_screen_nav_frame()
     * gets the real, un-multiplied elapsed time: the creature's wander is
     * presentation, not pet decay, and stays real-time the same way LVGL's
     * own tick and Lua's frame delta do. */
    kf_pet_session_frame(real_dt_ms * multiplier);

    if (hooks != nullptr && hooks->after_pet_session != nullptr) {
        hooks->after_pet_session();
    }

    kf_screen_nav_frame(real_dt_ms);

#ifdef KF_ENABLE_LVGL
    kf_lvgl_port_pump(0);
#endif

    /* kf_lua_port_frame(0): same "0 means real elapsed time" convention as
     * kf_pet_session_frame() would use without a multiplier, tracked
     * internally the same way kf_lvgl_port_pump()'s is -- Lua's frame delta
     * deliberately does not get the multiplier folded in, per the pet-
     * session comment above. */
    kf_lua_port_frame(0);

    /* kf_scene_commit() belongs to the frame loop, not the Lua binding --
     * present happens at the top of the NEXT kf_app_frame(), so a scene
     * committed here reaches the panel on the following frame.
     *
     * Guarded on kf_lua_scene_declared_anything(): hakoniwaos/src/scene.cpp's
     * own g_force_full_redraw starts true and stays true until the
     * process's first kf_scene_commit() ever runs, by design, so that first
     * commit repaints correctly with no reset needed. Calling this
     * unconditionally before any script has declared a single object would
     * paint one solid KF_BLACK frame over whatever the creature screen or
     * LVGL just drew. The demo creature script declares its entire Home
     * screen through kf.screen("home") and IS the render path under
     * KF_HOME_SCREEN=lua (the default), so this guard is live, not a
     * no-op. */
    if (kf_lua_scene_declared_anything()) {
        kf_scene_commit();
    }

    return real_dt_ms;
}
