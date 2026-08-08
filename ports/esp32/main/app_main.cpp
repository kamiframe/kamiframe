/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * ESP-IDF entry point, and the first version of this file that is a whole
 * device rather than a proof of one.
 *
 * The path here: ADR 0020 got the HAL backends running for real, ADR 0025
 * got a live kf_pet_state ticking, ADR 0026 put a real DS3231 behind the
 * wall clock, and three slices landed together -- ADR 0027 (LVGL, so the pet
 * has a SCREEN), ADR 0028 (Lua, so the demo creature SCRIPT drives it), and
 * ADR 0029 (panel profiles, so the driver underneath is not welded to one
 * display module).
 *
 * This file calls the real thing, and nothing here is a stub:
 * kf_app_init()/kf_app_frame() (kf/app.h), kf_pet_session_init()/_frame()
 * (simulator/src/pet/), kf_lvgl_port_init()/_pump() and kf_screen_nav_init()/
 * _frame() (simulator/src/lvgl/), and kf_lua_port_init()/_frame()
 * (simulator/src/lua/). Those are the same entry points sdl_main.cpp drives
 * on desktop, in the same order, for the reasons that file's own comments
 * give: the pet session needs the storage/power/time HAL up first, LVGL's
 * memory pool needs kf_app_init()'s arenas up first, and the per-frame order
 * (pet session, then screen nav, then LVGL's pump) is what stops a screen
 * showing last frame's numbers.
 *
 * KF_DEMO_NONE, not KF_DEMO_SPRITE: now that LVGL owns the framebuffer, the
 * demo sprite would fight it for the same pixels with no coordination
 * between them -- see kf/demo.h's comment on KF_DEMO_NONE, and ADR 0013's
 * "Found after delivery" section, which diagnosed exactly this on desktop
 * before it could happen here. sdl_main.cpp made the identical switch for
 * the identical reason.
 *
 * ============================================================================
 *  WHAT THIS DOES NOT DO, AND WHY -- read before assuming more than this
 *  slice claims.
 *
 *  NONE OF THIS HAS RUN ON HARDWARE. Every slice above is a clean
 *  cross-compile and link against ESP-IDF v6.0.2, nothing more. The pet
 *  screen has never been seen on glass, the demo creature script has never
 *  been observed driving anything on-device, and the panel profile this
 *  build defaults to is correct according to the bring-up diagnostic rather
 *  than according to this firmware. Treat the first flash as a test, not a
 *  formality.
 *
 *  The pet screen needs no Lua: kf_pet_screen.cpp only reads
 *  kf_pet_session_state() and calls kf_pet_session_feed()/play()/rest(),
 *  the same C++ API this file already drove before either slice. So a Lua
 *  fault should degrade the creature's behaviour, not blank the screen --
 *  which is worth knowing when reading a first-flash failure.
 *
 *  kf_time_wall() IS backed by a real DS3231 (ADR 0026), and unlike the rest
 *  of this list that part is hardware-verified: the bring-up diagnostic
 *  confirmed the clock advancing across a genuine power cut on coin-cell
 *  power. So kf_pet_session_init()'s offline fast-forward has something real
 *  to fast-forward across, on a board with the cell fitted.
 *
 *  Still not reached: no partition table beyond ESP-IDF's default
 *  single-app layout, so there is nowhere for assets to live yet.
 * ============================================================================
 */

#include "kf/app.h"
#include "kf/hal/log.h"
#include "kf/hal/time.h"

#include "kf_dbg_bridge.h"
#include "kf_lua_demo_creature_script.h"
#include "kf_lua_port.h"
#include "kf_lvgl_port.h"
#include "kf_pet_session.h"
#include "kf_screen_nav.h"

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_system.h"

#include <inttypes.h>
#include <stdio.h>

namespace {
constexpr const char *TAG = "app_main";

/* How often to print the pet's state to the serial log, on top of the
 * screen this slice adds. Kept, not replaced: a screen needs a working
 * device in hand and good lighting, and the log has been the fastest way
 * to confirm the pet is actually ticking since ADR 0025, still true now
 * that there is somewhere else to look too. 10s is frequent enough to
 * watch hunger/happiness/energy move against the default decay rates (the
 * fastest, hunger, is 1042 mp/hour -- about 3 millipercent every 10s,
 * visible over a few minutes) without flooding the monitor. */
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
     * exactly what "real" does and does not mean yet. KF_DEMO_NONE, not
     * KF_DEMO_SPRITE: LVGL owns the framebuffer now -- see this file's
     * header comment. */
    KF_LOGI(TAG, "starting kf_app_init (real ESP32 HAL backends)");
    kf_app_init(KF_DEMO_NONE);

    /* Same ordering sdl_main.cpp uses, and the same reason: this only
     * needs the storage/power/time HAL, already up as of kf_app_init()
     * above. First real on-device call -- a fresh pet if NVS has no save
     * yet, or a loaded one fast-forwarded by however long kf_time_wall()
     * says has passed (see this file's header comment on ADR 0026). */
    KF_LOGI(TAG, "starting kf_pet_session_init (real NVS-backed pet)");
    kf_pet_session_init();
    log_pet_state();

    /* LVGL next, then every screen this build has -- same ordering and same
     * reason as sdl_main.cpp: LVGL's memory pool comes from KF_ARENA_LVGL
     * (carved out by kf_app_init()'s kf_arena_init_all() above), its display
     * bridge writes into the framebuffer kf_app_init()'s kf_fb_init() already
     * created, and both screens' init functions read kf_pet_session_state()
     * the moment they run (kf_screen_nav_init() calls each screen's own init,
     * which calls its own *_update() once for real values from the first
     * frame) -- so kf_pet_session_init() above has to have already run. */
    KF_LOGI(TAG, "starting kf_lvgl_port_init (pet screen on the real panel)");
    kf_lvgl_port_init();
    kf_screen_nav_init();

    /* Lua comes up last -- same ordering and same reason sdl_main.cpp uses
     * (see its own comment on this call): its allocator's one block comes
     * from KF_ARENA_LUA, already carved out above, and its pet.* binding
     * (ADR 0016) reads kf_pet_session_state(), which must already exist.
     *
     * Last on purpose rather than by accident: a script that fails to load
     * is not fatal to the rest of the firmware. It runs with no Lua this
     * session, logged loudly by kf_lua_port_init() itself, and everything
     * already initialised above -- including the screen -- keeps working.
     * That is the failure mode you want on a first flash. */
    KF_LOGI(TAG, "starting kf_lua_port_init (real demo creature script)");
    kf_lua_port_init(kKfLuaDemoCreatureScriptSource,
                      kKfLuaDemoCreatureScriptChunkName);

    /* Last of all the *_init() calls, and deliberately so: STATE's reply
     * reads kf_pet_session_state(), SHOT reads kf_fb_pixels() (up since
     * kf_app_init() above), and PING reads KF_PANEL_PROFILE -- every one of
     * those needs to already be real by the time a command could possibly
     * arrive, which is only ever inside the loop below. See ADR 0030 for
     * the full protocol and docs/architecture/adr-0030-serial-debug-bridge.md's
     * "Shipping a build with this off" section for the one-flag way to
     * remove this from a non-developer build. */
    KF_LOGI(TAG, "starting kf_dbg_bridge_init (serial debug bridge)");
    kf_dbg_bridge_init();

    uint64_t next_pet_log_us = kf_time_mono_us() + kPetLogIntervalUs;

    KF_LOGI(TAG, "running (kf_app_frame loop; no quit condition on device)");
    for (;;) {
        /* kf_dbg_bridge_frame() runs BEFORE kf_app_frame() specifically so
         * a KFDBG BTN/BTNHOLD command already sitting in the queue affects
         * THIS iteration's kf_input_poll() (called from inside
         * kf_app_frame(), first thing) -- "for the next frame," as the
         * protocol spec puts it, means the very next one, not the one
         * after. Non-blocking either way: see kf_dbg_bridge.h's own "WHY A
         * BACKGROUND TASK" comment for why this never stalls the loop
         * regardless of where it sits in it. */
        kf_dbg_bridge_frame();

        if (!kf_app_frame()) {
            break;
        }

        /* kf_app_frame() paces itself against KF_FRAME_BUDGET_US via
         * kf_time_delay_us() (see hakoniwaos/src/app.cpp) -- no extra
         * vTaskDelay needed here, matching sdl_main.cpp's own bare
         * while-loop shape on desktop.
         *
         * The 0 argument means "use your own real-elapsed-time tracking" for
         * every call below that takes one (see kf_pet_session.h,
         * kf_lvgl_port.h, kf_lua_port.h) -- there is no debug time multiplier
         * on this backend to fold in, unlike sdl_main.cpp's own calls, so
         * there is nothing this file needs to compute itself.
         *
         * Ordering matches sdl_main.cpp's frame-ordering comment exactly,
         * for the same two reasons. kf_pet_session_frame() and
         * kf_screen_nav_frame() both run BEFORE kf_lvgl_port_pump(), so the
         * active screen has this frame's numbers pushed into its widgets
         * before pump's lv_timer_handler() redraws and flushes -- otherwise
         * the screen is permanently one frame behind. And the pet session
         * runs before kf_lua_port_frame(), so the script's pet.hunger() and
         * friends see this frame's elapsed time already applied. */
        kf_pet_session_frame(0);
        kf_screen_nav_frame();
        kf_lvgl_port_pump(0);

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
     * g.running in app.cpp never flips false on this backend -- `for (;;)`
     * with an explicit `if (!kf_app_frame()) break;` above still only ever
     * exits through that same, never-taken path. Kept anyway, the same way
     * sdl_main.cpp keeps its own shutdown call after a loop that can exit --
     * correct shape if that ever changes, dead code if it doesn't, and
     * either way cheaper than leaving it out and being wrong later.
     * Shutdown is the exact reverse of the init order above -- the debug
     * bridge, then Lua, then LVGL, then the pet session, then the app --
     * which for everything below kf_dbg_bridge_shutdown() is also the order
     * sdl_main.cpp uses. Lua and LVGL only touch their own arena blocks, but
     * kf_pet_session_shutdown() still needs the storage HAL that
     * kf_app_shutdown() is about to tear down. */
    kf_dbg_bridge_shutdown();
    kf_lua_port_shutdown();
    kf_lvgl_port_shutdown();
    kf_pet_session_shutdown();
    kf_app_shutdown();
}
