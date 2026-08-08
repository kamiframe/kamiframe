/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * ESP-IDF entry point. Phase 1b (ADR 0020) got the HAL backends running
 * for real; ADR 0025 got a live kf_pet_state ticking on the device; this
 * slice (ADR 0030) is the first one where that pet is actually driven by a
 * Lua script, not just C++.
 *
 * This file calls the real thing: kf_app_init()/kf_app_frame() (kf/app.h),
 * the exact same core entry points sdl_main.cpp drives on desktop, now
 * running against the ESP32 HAL backends in ../hal/ instead of SDL3 --
 * plus kf_pet_session_init()/kf_pet_session_frame() (simulator/src/pet/)
 * and kf_lua_port_init()/kf_lua_port_frame() (simulator/src/lua/), the
 * same pet-session and Lua orchestration sdl_main.cpp drives, wired up
 * here the same way and in the same relative order (pet session before
 * Lua -- see ADR 0030 for why, and this file's own call sites below).
 *
 * ============================================================================
 *  WHAT THIS DOES NOT DO, AND WHY -- read before assuming more than this
 *  slice claims.
 *
 *  kf_app_frame() still drives kf/demo.h's placeholder content
 *  (KF_DEMO_SPRITE below), same as ADR 0020 -- see that macro's comment
 *  just below for why it stays. The pet session and Lua both run entirely
 *  alongside it: neither touches the framebuffer, only their own state (and
 *  NVS, for the pet session), so there is no coordination needed with the
 *  placeholder sprite, the same "no coordination, because nothing shares a
 *  surface" reasoning KF_DEMO_NONE's own comment gives for the desktop
 *  build once LVGL is in the picture.
 *
 *  What still is NOT reachable: no ADR 0017 pet screen, no ADR 0018 demo
 *  creature SCREEN -- the demo creature SCRIPT (kf_lua_demo_creature_script.h)
 *  is loaded and running as of this slice (ADR 0030), it just has nowhere to
 *  draw yet, so kf.log() lines are still this build's only visible output
 *  from it, same as the pet session's own KF_LOGI summary below. LVGL is
 *  still not in this component tree -- that is a separate, still-open
 *  EXTRA_COMPONENT_DIRS entry this slice does not add. Stale note this
 *  slice found and is correcting in passing, since it already had this
 *  paragraph open: kf_time_wall() IS now backed by a real DS3231 (ADR
 *  0026, landed after ADR 0025's version of this comment was written), so
 *  kf_pet_session_init()'s offline fast-forward has something real to
 *  fast-forward across on a genuine power-off, on hardware carrying a
 *  battery-backed DS3231 -- see ../hal/esp_time.cpp's own header comment
 *  for exactly what is and is not proven about that path yet.
 * ============================================================================
 */

#include "kf/app.h"
#include "kf/hal/log.h"
#include "kf/hal/time.h"

#include "kf_lua_demo_creature_script.h"
#include "kf_lua_port.h"
#include "kf_pet_session.h"

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_system.h"

#include <inttypes.h>
#include <stdio.h>

namespace {
constexpr const char *TAG = "app_main";

/* How often to print the pet's state to the serial log. Nothing renders
 * it yet (see this file's header comment), so this IS the visibility
 * this slice has -- 10s is frequent enough to watch hunger/happiness/
 * energy move against the default decay rates (the fastest, hunger, is
 * 1042 mp/hour -- about 3 millipercent every 10s, visible over a few
 * minutes) without flooding the monitor. */
constexpr uint64_t kPetLogIntervalUs = 10ull * 1000ull * 1000ull;

void log_pet_state() {
    const kf_pet_state *pet = kf_pet_session_state();
    KF_LOGI(TAG,
            "pet: stage=%d hunger=%lu.%02lu%% happy=%lu.%02lu%% "
            "energy=%lu.%02lu%% base_trait=%u",
            static_cast<int>(pet->stage),
            static_cast<unsigned long>(pet->hunger_mp / 1000u),
            static_cast<unsigned long>((pet->hunger_mp / 10u) % 100u),
            static_cast<unsigned long>(pet->happiness_mp / 1000u),
            static_cast<unsigned long>((pet->happiness_mp / 10u) % 100u),
            static_cast<unsigned long>(pet->energy_mp / 1000u),
            static_cast<unsigned long>((pet->energy_mp / 10u) % 100u),
            static_cast<unsigned>(pet->base_trait));
}

} // namespace

extern "C" void app_main(void) {
    /* Kept from ADR 0019's hello-world verbatim: this banner is what Chris's
     * real Wokwi run already proved boots correctly, and there is no reason
     * to touch working, verified output while adding unrelated code below
     * it. */
    printf("kamiframe: hello from ESP-IDF\n");

    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    printf("kamiframe: chip=%s cores=%d revision=%d\n", CONFIG_IDF_TARGET,
           chip_info.cores, chip_info.revision);

    uint32_t flash_size = 0;
    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK) {
        printf("kamiframe: flash=%" PRIu32 "MB\n", flash_size / (1024u * 1024u));
    }

    printf("kamiframe: free heap=%" PRIu32 " bytes\n", esp_get_free_heap_size());

    /* Real HAL backends from here on -- see this file's header comment for
     * exactly what "real" does and does not mean yet. KF_DEMO_SPRITE, not
     * KF_DEMO_NONE: there is still no LVGL screen to coordinate with on
     * this build, so the placeholder content is the only thing worth
     * having on screen, and KF_DEMO_SPRITE is the cheap case that best
     * exercises the dirty-rect path esp_display.cpp's kf_display_present()
     * still ignores today. */
    KF_LOGI(TAG, "starting kf_app_init (real ESP32 HAL backends)");
    kf_app_init(KF_DEMO_SPRITE);

    /* Same ordering sdl_main.cpp uses, and the same reason: this only
     * needs the storage/power/time HAL, already up as of kf_app_init()
     * above. First real on-device call -- a fresh pet if NVS has no save
     * yet, or a loaded one fast-forwarded by however long kf_time_wall()
     * says has passed (see this file's header comment on ADR 0026). */
    KF_LOGI(TAG, "starting kf_pet_session_init (real NVS-backed pet)");
    kf_pet_session_init();
    log_pet_state();

    /* Lua comes up last, after the pet session -- same ordering and same
     * reason sdl_main.cpp uses (see its own comment on this call): its
     * allocator's one block comes from KF_ARENA_LUA, already carved out by
     * kf_app_init()'s kf_arena_init_all() above, and its pet.* binding
     * (ADR 0016) reads kf_pet_session_state(), which must already exist.
     * No LVGL step in between on this build -- unlike sdl_main.cpp, there
     * is no screen for LVGL to own here yet. A script that fails to load
     * is not fatal to the rest of the firmware; it just runs with no Lua
     * this session, logged loudly by kf_lua_port_init() itself. */
    KF_LOGI(TAG, "starting kf_lua_port_init (real demo creature script)");
    kf_lua_port_init(kKfLuaDemoCreatureScriptSource,
                      kKfLuaDemoCreatureScriptChunkName);

    uint64_t next_pet_log_us = kf_time_mono_us() + kPetLogIntervalUs;

    KF_LOGI(TAG, "running (kf_app_frame loop; no quit condition on device)");
    while (kf_app_frame()) {
        /* kf_app_frame() paces itself against KF_FRAME_BUDGET_US via
         * kf_time_delay_us() (see hakoniwaos/src/app.cpp) -- no extra
         * vTaskDelay needed here, matching sdl_main.cpp's own bare
         * while-loop shape on desktop.
         *
         * kf_pet_session_frame(0): 0 means "use your own real-elapsed-time
         * tracking" (see kf_pet_session.h) -- there is no debug time
         * multiplier on this backend to fold in, unlike sdl_main.cpp's
         * own call, so there is nothing this file needs to compute itself.
         * Runs before kf_lua_port_frame() below, same ordering sdl_main.cpp
         * uses: the script's pet.hunger()/etc. calls this frame should see
         * this frame's elapsed time already applied, not last frame's. */
        kf_pet_session_frame(0);

        /* kf_lua_port_frame(0): same "0 means real elapsed time" convention
         * as kf_pet_session_frame() above, tracked internally the same way
         * kf_lvgl_port_pump()'s would be on the desktop build -- there is no
         * debug time multiplier on this backend to fold in either. */
        kf_lua_port_frame(0);

        const uint64_t now_us = kf_time_mono_us();
        if (now_us >= next_pet_log_us) {
            log_pet_state();
            next_pet_log_us = now_us + kPetLogIntervalUs;
        }
    }

    /* Unreachable in practice: esp_input.cpp's kf_input_poll() hardcodes
     * quit_requested = false (there is no hardware "quit" concept), so
     * g.running in app.cpp never flips false on this backend. Kept anyway,
     * the same way sdl_main.cpp keeps its own shutdown call after a loop
     * that can exit -- correct shape if that ever changes, dead code if it
     * doesn't, and either way cheaper than leaving it out and being wrong
     * later. Shutdown order is the exact reverse of the init order above
     * (Lua, then session, then app), same order sdl_main.cpp uses --
     * kf_lua_port_shutdown() only touches its own arena block, but
     * kf_pet_session_shutdown() still needs the storage HAL
     * kf_app_shutdown() is about to tear down. */
    kf_lua_port_shutdown();
    kf_pet_session_shutdown();
    kf_app_shutdown();
}
