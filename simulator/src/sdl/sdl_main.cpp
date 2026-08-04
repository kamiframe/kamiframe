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
#include "../lvgl/kf_lvgl_proof_screen.h"
#include "../lua/kf_lua_port.h"
#include "../lua/kf_lua_proof_script.h"
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

} // namespace

int main(int argc, char *argv[]) {
    int scale = 3;
    long max_frames = 0; /* 0 = run until quit */
    kf_demo_mode mode = KF_DEMO_SPRITE;

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

    /* LVGL comes up after core (its memory pool comes from KF_ARENA_LVGL,
     * carved out by kf_app_init()'s kf_arena_init_all(); its display bridge
     * writes into the framebuffer kf_app_init()'s kf_fb_init() creates).
     * See ADR 0013 -- this is a proof screen, not a real menu; nothing to
     * put in one yet. */
    kf_lvgl_port_init();
    kf_lvgl_proof_screen_init();

    /* The pet session comes up next: it only needs the storage/power/time
     * HAL, already brought up by kf_app_init() above, not LVGL or Lua. It
     * has to be ready before Lua, though -- the pet.* binding
     * (kf_lua_port.cpp, ADR 0016) reads it starting the moment a script
     * calls one of those functions, which could be as early as the
     * script's own top-level code. This is the FIRST time this mechanism
     * runs against the real host clock and a real save directory, rather
     * than only inside a synthetic headless test -- see ADR 0015's
     * offline-fast-forward proof and ADR 0016's Lua binding proof for the
     * automated version of what this is now doing for real. */
    kf_pet_session_init();

    /* Lua comes up after core, LVGL and the pet session, same "after
     * everything it depends on" ordering: its allocator's one big block
     * comes from KF_ARENA_LUA, carved out by kf_app_init()'s
     * kf_arena_init_all(), and its pet.* binding needs the pet session
     * ready. See ADR 0014 -- this is a proof script, not a real cartridge,
     * same "nothing to put in one yet" as LVGL's proof screen; it does not
     * touch pet.* itself, so nothing about this session's decay is visible
     * yet, only running for real underneath. A script that fails to load
     * is not fatal to the rest of the simulator; it just runs with no Lua
     * this session, logged loudly by kf_lua_port_init(). */
    kf_lua_port_init(kKfLuaProofScriptSource, kKfLuaProofScriptChunkName);

    KF_LOGI(TAG, "running (close the window or press Ctrl-C to stop)");

    long frames = 0;
    while (kf_app_frame()) {
        /* 0 => real elapsed time, not a synthetic step: this is the
         * interactive build, actually watching the clock. See
         * kf_lvgl_tick.h. */
        kf_lvgl_port_pump(0);
        kf_pet_session_frame(0);
        kf_lua_port_frame(0);
        update_title(static_cast<uint64_t>(frames));
        frames++;
        if (max_frames > 0 && frames >= max_frames) {
            break;
        }
    }

    kf_lua_port_shutdown();
    kf_pet_session_shutdown();
    kf_lvgl_port_shutdown();
    kf_app_shutdown();
    return 0;
}
