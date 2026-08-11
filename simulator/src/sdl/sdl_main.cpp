/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * The desktop simulator's entry point.
 *
 * THIS is where the loop lives. Core has no loop: kf_app_frame() runs one
 * frame and returns, and each backend drives it however its platform needs.
 * Under Emscripten this same file would hand kf_app_frame to
 * emscripten_set_main_loop instead of using while(), and nothing above would
 * change. On the ESP32 a FreeRTOS task will do the same.
 *
 * Usage:
 *     kamiframe-sim [--scale N] [--frames N] [--pack PATH]
 */

#include "kf/app.h"
#include "kf/hal/log.h"
#include "kf/hal/time.h"
#include "kf/scene.h"
#include "host_assets.h"
#include "sdl_debug_window.h"
#include "sdl_shared.h"

#ifdef KF_ENABLE_LVGL
#include "../lvgl/kf_lvgl_port.h"
#endif
#include "../../../sdk/lua/generated/kf_lua_demo_creature_script.h"
#include "../../../sdk/lua/kf_lua_port.h"
#include "../../../sdk/lua/kf_lua_scene.h"
#include "../pet/kf_pet_session.h"
#include "../pet/kf_screen_nav.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr const char *TAG = "sim";

void update_title(uint64_t frame) {
    /* Once every 15 frames: retitling a window is not free and this is a
     * development affordance, not something the device does. */
    if (frame % 15u != 0u) {
        return;
    }
    KfSdlState &s = kf_sdl_state();
    if (s.window == nullptr) {
        return;
    }
    const kf_frame_summary sum = kf_app_frame_summary();
    const kf_frame_stats *last = kf_app_last_frame();

    char title[192];
    std::snprintf(title, sizeof(title),
                  "Kamiframe  |  %.1f ms (%.1f cpu + %.1f xfer)  avg %.1f  "
                  "p99 %.1f  |  dirty %u%%%s",
                  last->total_us / 1000.0, last->cpu_us / 1000.0,
                  last->transfer_us / 1000.0, sum.mean_us / 1000.0,
                  sum.p99_us / 1000.0, last->dirty_percent,
                  last->over_budget ? "  OVER" : "");
    SDL_SetWindowTitle(s.window, title);
}

} // namespace

int main(int argc, char *argv[]) {
    int scale = 3;
    long max_frames = 0; /* 0 = run until quit */
    /* KF_DEMO_NONE, not KF_DEMO_SPRITE: the interactive build's Home screen
     * draws its own creature straight into the framebuffer now (Task 4 of
     * the pet-screen plan, kf_creature_screen.cpp) -- not LVGL, but the
     * exact same coordination hazard KF_DEMO_NONE was already added to
     * prevent for LVGL's Home screen still applies here unchanged: whatever
     * KF_DEMO_SPRITE's bouncing placeholder sprite drew would fight the
     * creature screen for the same pixels with no coordination between the
     * two -- see kf/demo.h's own comment on KF_DEMO_NONE for what that
     * looked like in practice when it was LVGL on the other side instead.
     * --stress below still opts into KF_DEMO_FULLSCREEN deliberately; it is
     * a stress tool for the custom engine, not the everyday interactive
     * experience, and accepts the same lack of coordination as a known cost
     * of asking for it explicitly. */
    kf_demo_mode mode = KF_DEMO_NONE;
    /* nullptr means "no override" -- kf_host_assets_set_pack_path(nullptr)
     * below is a no-op in that case, and kf_hal_assets_mount() falls
     * through to the compiled-in default exactly as it does today. Points
     * into argv, which outlives this whole function, so no copy is needed
     * before the set-pack-path call below reads it. */
    const char *pack_path = nullptr;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
            scale = std::atoi(argv[++i]);
            if (scale < 1) {
                scale = 1;
            }
        } else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            max_frames = std::atol(argv[++i]);
        } else if (std::strcmp(argv[i], "--stress") == 0) {
            mode = KF_DEMO_FULLSCREEN;
        } else if (std::strcmp(argv[i], "--pack") == 0 && i + 1 < argc) {
            /* Loads a different .kfpack at runtime -- e.g. a creature
             * roster built by tools/kf_ingest_sprites.py -- without
             * touching KF_ASSET_PACK (the CMake cache variable
             * examples/hello_sprite/assets.kfpack is baked in from,
             * simulator/CMakeLists.txt) or recompiling. This flag only
             * exists on this binary's argv, so it cannot itself affect
             * kamiframe-headless or any ctest target -- the one other
             * caller of kf_host_assets_set_pack_path(),
             * run_creature_screen_sprite_check() in headless_main.cpp,
             * reaches it through its own compiled-in demo-pack path
             * instead, and restores the override to null before
             * returning, which is what keeps every other test's mount
             * unaffected -- see host_assets.h's own comment on why the
             * override has to be a desktop-only, opt-in call rather than
             * something Core (or the default path) can reach. */
            pack_path = argv[++i];
        } else if (std::strcmp(argv[i], "--help") == 0) {
            std::printf(
                "kamiframe-sim [--scale N] [--frames N] [--stress] "
                "[--pack PATH]\n"
                "  --stress     scrolling tilemap + 12 sprites, every "
                "pixel redrawn every frame\n"
                "  --pack PATH  load this .kfpack instead of the compiled-"
                "in default\n");
            return 0;
        }
    }

    kf_sdl_state().scale = scale;
    kf_host_assets_set_pack_path(pack_path);

    kf_app_init(mode);

    /* The pet session comes up first among these four, ahead of LVGL and
     * Lua: it only needs the storage/power/time HAL, already brought up by
     * kf_app_init() above, but BOTH of the other two now depend on it
     * being ready before they finish their own init -- the pet screen's
     * buttons (kf_pet_screen.cpp, ADR 0017) and the pet.* Lua binding
     * (kf_lua_port.cpp, ADR 0016) both read it starting the moment
     * something calls one of their functions, which for the screen is as
     * early as kf_pet_screen_init() itself (it calls
     * kf_pet_screen_update() once to show real values from its first
     * frame). This is the FIRST time this mechanism runs against the real
     * host clock and a real save directory, rather than only inside a
     * synthetic headless test -- see ADR 0015's offline-fast-forward proof
     * and ADR 0016/0017's own proofs for the automated version of what
     * this is now doing for real. */
    kf_pet_session_init();

    /* LVGL, only under -DKF_ENABLE_LVGL=ON (ADR 0045 -- Info moved to a
     * kf.screen() group over the retained scene, so nothing in a running
     * build needs LVGL any more; the option exists to keep the code and
     * its 256 KB arena available for the LVGL-vs-custom-engine evaluation
     * CLAUDE.md names as deliberately deferred, not decided). Then every
     * screen this build has (kf_screen_nav.cpp, ADR 0022/0044) -- Home's
     * init function needs the pet session ready, same as always. */
#ifdef KF_ENABLE_LVGL
    kf_lvgl_port_init();
#endif
    kf_screen_nav_init();

    /* Lua comes up last of the four, same "after everything it depends
     * on" ordering: its allocator's one big block comes from KF_ARENA_LUA,
     * carved out by kf_app_init()'s kf_arena_init_all(), and its pet.*
     * binding needs the pet session ready. See ADR 0014. This is the demo
     * creature (ADR 0018), not the ADR 0014 proof script any earlier
     * version of this file loaded -- that script still exists and is still
     * exactly right for what it proves (kamiframe-headless --verify-lua,
     * an exact arithmetic invariant unrelated to any pet), it is just not
     * what a person running kamiframe-sim should be looking at. This is a
     * real cartridge: it reads pet.hunger()/happiness()/energy() and logs
     * in-character lines when a need crosses a band, entirely from Lua, no
     * C required. A script that fails to load is not fatal to the rest of
     * the simulator; it just runs with no Lua this session, logged loudly
     * by kf_lua_port_init(). */
    kf_lua_port_init(kKfLuaDemoCreatureScriptSource,
                      kKfLuaDemoCreatureScriptChunkName);

    /* The debug window (sdl_debug_window.h) -- after the pet window (up
     * since kf_app_init()) and the pet session (up since kf_pet_session_
     * init() above), its two dependencies. Everything that used to be
     * keyboard shortcuts + KF_LOGI lines on this window now lives there
     * instead, clickable, per Chris's call that he doesn't want debug
     * text sitting on the game screen or the terminal. */
    kf_sdl_debug_window_init();

    KF_LOGI(TAG, "running (close the window or press Ctrl-C to stop)");

    /* Tracked here, not left to kf_pet_session_frame()'s own internal
     * real-time tracking, because the debug window's time multiplier
     * (kf_sdl_debug_window_time_multiplier()) has to scale ONLY the delta
     * fed to the pet session -- not LVGL's tick (kf_lvgl_port_pump()) or
     * Lua's frame delta (kf_lua_port_frame()), both of which stay
     * real-time so animation and script frame-rate semantics are
     * unaffected. Passing a non-zero synthetic value every frame (instead
     * of 0 = "use your own real-time tracking") also avoids a correctness
     * trap: kf_pet_session_frame() only updates ITS internal last-call
     * timestamp on the `dt_ms == 0` path, so interleaving 0 and non-zero
     * calls across frames (e.g. multiplier flips between 1x and 2x) would
     * leave that internal timestamp stale and cause a double-counted
     * jump the next time 0 was passed. Always computing and passing the
     * delta ourselves sidesteps that entirely. */
    uint64_t last_frame_us = 0;
    long frames = 0;
    while (kf_app_frame()) {
        const uint64_t now_us = kf_time_mono_us();
        const uint32_t real_dt_ms =
            last_frame_us == 0u
                ? 0u
                : static_cast<uint32_t>((now_us - last_frame_us) / 1000u);
        last_frame_us = now_us;
        const uint32_t multiplier = kf_sdl_debug_window_time_multiplier();

        /* kf_pet_session_frame() and kf_screen_nav_frame() both run before
         * kf_lvgl_port_pump(): the session needs to have applied this
         * frame's elapsed time before the active screen reads it. Under
         * -DKF_ENABLE_LVGL=ON this also keeps a still-LVGL screen from
         * showing last frame's numbers one frame behind, the reason the
         * pump call used to be conditional on which screen was active
         * (kf_screen_nav_wants_lvgl(), removed with ADR 0045 -- every
         * screen this build can show is a kf.screen() group now, so LVGL,
         * when built in at all, has nothing left to pump for). A button
         * press this frame is handled INSIDE pump (LVGL processes input
         * during lv_timer_handler()), so its effect shows up starting next
         * frame's update -- one frame of input lag, imperceptible at this
         * frame rate. kf_screen_nav_frame() itself reads MENU/B straight
         * from kf_app_buttons_pressed() (ADR 0022), which kf_app_frame()
         * -- the while-condition above -- has already refreshed for this
         * frame by the time we get here, so a screen switch this frame is
         * NOT subject to that same one-frame lag.
         *
         * kf_screen_nav_frame() gets the real, un-multiplied elapsed time
         * -- the creature's wander is presentation, not pet decay, so it
         * stays real-time exactly the way LVGL's own tick and Lua's frame
         * delta already do (see this function's header comment on why the
         * multiplier applies only to the pet session). */
        kf_pet_session_frame(real_dt_ms * multiplier);
        kf_sdl_debug_window_frame();
        kf_screen_nav_frame(real_dt_ms);
#ifdef KF_ENABLE_LVGL
        kf_lvgl_port_pump(0);
#endif
        kf_lua_port_frame(0);
        /* kf_scene_commit() belongs to the frame loop, not the Lua binding
         * (Task 3 of docs/superpowers/plans/2026-08-12-lua-game-layer.md)
         * -- present happens at the top of the NEXT kf_app_frame(), so a
         * scene committed here reaches the panel on the following frame,
         * the same as the creature screen's own drawing.
         *
         * Guarded on kf_lua_scene_declared_anything(): kf_scene_reset() is
         * never called this task (Task 4's job) or ever by this file, so
         * hakoniwaos/src/scene.cpp's own g_force_full_redraw starts true
         * and stays true until the process's first kf_scene_commit() ever
         * runs -- by design, so that first commit repaints correctly with
         * no reset needed. Calling it unconditionally here, before any
         * script has declared a single object, would paint one solid
         * KF_BLACK frame over whatever the creature screen or LVGL just
         * drew. The demo creature script (examples/creature_demo/
         * creature.lua) still only logs, so this stays a no-op for the
         * whole of this task -- see kf_lua_scene.h's own comment on this
         * predicate for the full reasoning. */
        if (kf_lua_scene_declared_anything()) {
            kf_scene_commit();
        }
        update_title(static_cast<uint64_t>(frames));
        frames++;
        if (max_frames > 0 && frames >= max_frames) {
            break;
        }
    }

    kf_sdl_debug_window_shutdown();
    kf_lua_port_shutdown();
#ifdef KF_ENABLE_LVGL
    kf_lvgl_port_shutdown();
#endif
    kf_pet_session_shutdown();
    kf_app_shutdown();
    return 0;
}
