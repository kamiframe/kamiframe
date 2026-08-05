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
 *     kamiframe-sim [--scale N] [--frames N]
 */

#include "kf/app.h"
#include "kf/hal/log.h"
#include "sdl_shared.h"

#include "../lvgl/kf_lvgl_port.h"
#include "../lvgl/kf_pet_screen.h"
#include "../lua/kf_lua_demo_creature_script.h"
#include "../lua/kf_lua_port.h"
#include "../pet/kf_pet_session.h"

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

/* Duplicated here rather than shared with kf_pet_screen.cpp's identical
 * mapping: that one lives in an anonymous namespace private to a
 * different translation unit, and this is four lines of log-message
 * formatting, not a mechanism worth a shared header over. */
const char *stage_name(kf_pet_stage stage) {
    switch (stage) {
    case KF_PET_STAGE_EGG:
        return "egg";
    case KF_PET_STAGE_BABY:
        return "baby";
    case KF_PET_STAGE_CHILD:
        return "child";
    case KF_PET_STAGE_TEEN:
        return "teen";
    case KF_PET_STAGE_ADULT:
    default:
        return "adult";
    }
}

/* DEBUG ONLY -- see kf_pet_session.h's own "DEBUG ONLY below this line"
 * section for why these exist and why they are safe to call directly:
 * kf_pet_advance()'s bounded-loop design (ADR 0021) makes an arbitrary
 * time jump cheap, and this is the exact same function offline
 * fast-forward already relies on, not a separate, less-tested path.
 *
 * Number keys, not letters: they read naturally as "how far" (1/2/3 ==
 * hour/day/week) and do not collide with the WASD/arrows/Z/X/J/K/Enter/
 * Escape bindings sdl_input.cpp already uses for the real d-pad/A/B/menu
 * buttons (see that file's header comment) -- these are deliberately NOT
 * added to kf_button/kf_input_raw, since a time-skip is not one of the
 * real device's physical buttons and has no business being modelled as
 * one.
 *
 * Polled with simple press-edge tracking (a static `previous` array) so
 * holding a key down skips once, not once per frame at 30fps. */
void poll_debug_time_skip() {
    struct DebugSkip {
        SDL_Scancode scancode;
        uint32_t seconds;
        const char *label;
    };
    constexpr DebugSkip kSkips[] = {
        {SDL_SCANCODE_1, 3600u, "1 hour"},
        {SDL_SCANCODE_2, 24u * 3600u, "1 day"},
        {SDL_SCANCODE_3, 7u * 24u * 3600u, "1 week"},
    };
    static bool previous[3] = {false, false, false};
    static bool previous_reset = false;

    const bool *keys = SDL_GetKeyboardState(nullptr);
    if (keys == nullptr) {
        return;
    }

    for (size_t i = 0; i < 3; ++i) {
        const bool now = keys[kSkips[i].scancode];
        if (now && !previous[i]) {
            kf_pet_session_debug_advance(kSkips[i].seconds);
            const kf_pet_state *s = kf_pet_session_state();
            KF_LOGI(TAG,
                    "[debug] skipped %s -- now %s, hunger %u.%u%% happy "
                    "%u.%u%% energy %u.%u%%",
                    kSkips[i].label, stage_name(s->stage),
                    s->hunger_mp / 1000u, (s->hunger_mp / 100u) % 10u,
                    s->happiness_mp / 1000u, (s->happiness_mp / 100u) % 10u,
                    s->energy_mp / 1000u, (s->energy_mp / 100u) % 10u);
        }
        previous[i] = now;
    }

    const bool reset_now = keys[SDL_SCANCODE_0];
    if (reset_now && !previous_reset) {
        kf_pet_session_debug_reset();
        KF_LOGI(TAG, "[debug] reset to a fresh egg");
    }
    previous_reset = reset_now;
}

} // namespace

int main(int argc, char *argv[]) {
    int scale = 3;
    long max_frames = 0; /* 0 = run until quit */
    /* KF_DEMO_NONE, not KF_DEMO_SPRITE: the interactive build's default is
     * the real pet screen now (ADR 0017), and KF_DEMO_SPRITE's bouncing
     * sprite draws into the exact same framebuffer LVGL owns with no
     * coordination between the two -- see kf/demo.h's own comment on
     * KF_DEMO_NONE for what that looked like in practice. --stress below
     * still opts into KF_DEMO_FULLSCREEN deliberately; it is a stress tool
     * for the custom engine, not the everyday interactive experience, and
     * accepts the same lack of coordination as a known cost of asking for
     * it explicitly. */
    kf_demo_mode mode = KF_DEMO_NONE;

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
        } else if (std::strcmp(argv[i], "--help") == 0) {
            std::printf("kamiframe-sim [--scale N] [--frames N] [--stress]\n"
                        "  --stress  scrolling tilemap + 12 sprites, every "
                        "pixel redrawn every frame\n");
            return 0;
        }
    }

    kf_sdl_state().scale = scale;

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

    /* LVGL comes up next (its memory pool comes from KF_ARENA_LVGL, carved
     * out by kf_app_init()'s kf_arena_init_all(); its display bridge
     * writes into the framebuffer kf_app_init()'s kf_fb_init() creates),
     * then the pet screen -- the first real menu screen kf_lvgl_proof_
     * screen.h's own header comment named as the reason to delete it
     * (still not deleted; see ADR 0017's "Decision" for why the proof
     * screen itself stays, even though nothing here calls
     * kf_lvgl_proof_screen_init() any more). */
    kf_lvgl_port_init();
    kf_pet_screen_init();

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

    KF_LOGI(TAG, "debug (this build only): 1/2/3 = skip 1 hour/day/week, "
                 "0 = reset to a fresh egg -- see kf_pet_session.h");
    KF_LOGI(TAG, "running (close the window or press Ctrl-C to stop)");

    long frames = 0;
    while (kf_app_frame()) {
        /* 0 => real elapsed time, not a synthetic step: this is the
         * interactive build, actually watching the clock. See
         * kf_lvgl_tick.h.
         *
         * kf_pet_session_frame() and kf_pet_screen_update() both run
         * BEFORE kf_lvgl_port_pump(): the session needs to have applied
         * this frame's elapsed time before the screen reads it, and the
         * screen needs to have pushed that into its widgets before pump's
         * lv_timer_handler() call redraws and flushes -- otherwise the
         * screen would always be showing last frame's numbers, one frame
         * behind. A button press this frame is handled INSIDE pump (LVGL
         * processes input during lv_timer_handler()), so its effect shows
         * up starting next frame's update -- one frame of input lag,
         * imperceptible at this frame rate. */
        kf_pet_session_frame(0);
        poll_debug_time_skip();
        kf_pet_screen_update();
        kf_lvgl_port_pump(0);
        kf_lua_port_frame(0);
        update_title(static_cast<uint64_t>(frames));
        frames++;
        if (max_frames > 0 && frames >= max_frames) {
            break;
        }
    }

    kf_lua_port_shutdown();
    kf_lvgl_port_shutdown();
    kf_pet_session_shutdown();
    kf_app_shutdown();
    return 0;
}
