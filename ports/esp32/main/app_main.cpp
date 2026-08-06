/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * ESP-IDF entry point. Phase 1b (ADR 0020) got the HAL backends running
 * for real; this slice (ADR 0025) is the first one where a live
 * kf_pet_state is actually ticking on the device, not just proven to
 * compile against it.
 *
 * This file calls the real thing: kf_app_init()/kf_app_frame() (kf/app.h),
 * the exact same core entry points sdl_main.cpp drives on desktop, now
 * running against the ESP32 HAL backends in ../hal/ instead of SDL3 --
 * plus kf_pet_session_init()/kf_pet_session_frame() (simulator/src/pet/),
 * the same pet-session orchestration sdl_main.cpp drives, wired up here
 * the same way.
 *
 * ============================================================================
 *  WHAT THIS DOES NOT DO, AND WHY -- read before assuming more than this
 *  slice claims.
 *
 *  kf_app_frame() still drives kf/demo.h's placeholder content
 *  (KF_DEMO_SPRITE below), same as ADR 0020 -- see that macro's comment
 *  just below for why it stays. The pet session runs entirely alongside
 *  it: it never touches the framebuffer, only its own state and NVS, so
 *  there is no coordination needed between the two, the same "no
 *  coordination, because nothing shares a surface" reasoning KF_DEMO_NONE's
 *  own comment gives for the desktop build once LVGL is in the picture.
 *
 *  What still is NOT reachable: no ADR 0017 pet screen and no ADR 0018 demo
 *  creature. LVGL and Lua are not in this component tree -- this port's
 *  CMakeLists.txt (see ../CMakeLists.txt) still only points
 *  EXTRA_COMPONENT_DIRS at hakoniwaos, and hakoniwaos/sources.cmake itself
 *  contains no LVGL or Lua files. So the pet is genuinely alive and being
 *  saved/loaded through real NVS, but the only way to see it right now is
 *  the periodic KF_LOGI summary below, not a screen. Also unchanged from
 *  ADR 0020: kf_time_wall() has no DS3231 behind it yet (see
 *  ../hal/esp_time.cpp's own header comment), so kf_pet_session_init()'s
 *  offline fast-forward has nothing to fast-forward across on a genuine
 *  power-off -- a pet that ages while the device is unplugged is still
 *  future work, not this slice's.
 * ============================================================================
 */

#include "kf/app.h"
#include "kf/hal/log.h"
#include "kf/hal/time.h"

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
     * says has passed (nothing, today -- see this file's header comment). */
    KF_LOGI(TAG, "starting kf_pet_session_init (real NVS-backed pet)");
    kf_pet_session_init();
    log_pet_state();

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
         * own call, so there is nothing this file needs to compute itself. */
        kf_pet_session_frame(0);

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
     * later. Session shutdown before app shutdown, same order sdl_main.cpp
     * uses -- kf_pet_session_shutdown() still needs the storage HAL
     * kf_app_shutdown() is about to tear down. */
    kf_pet_session_shutdown();
    kf_app_shutdown();
}
