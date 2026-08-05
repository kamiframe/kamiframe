/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * ESP-IDF entry point, Phase 1b (ADR 0020). Supersedes main.c, which was
 * deliberately a plain hello-world that never called into hakoniwaos's own
 * app loop -- see main.c's retired header comment, kept in git history, for
 * why that was the right scope for ADR 0019.
 *
 * This file calls the real thing: kf_app_init()/kf_app_frame() (kf/app.h),
 * the exact same core entry points sdl_main.cpp drives on desktop, now
 * running against the ESP32 HAL backends in ../hal/ instead of SDL3.
 *
 * ============================================================================
 *  WHAT THIS DOES NOT DO, AND WHY -- read before assuming more than this
 *  slice claims.
 *
 *  kf_app_frame() drives kf/demo.h's placeholder content (KF_DEMO_SPRITE
 *  below), NOT the pet. kf_pet_session.{h,cpp} -- the code that owns the
 *  live kf_pet_state, offline fast-forward, and per-frame decay -- lives in
 *  simulator/src/pet/, which simulator/CMakeLists.txt documents as living
 *  outside hakoniwaos "for the same reason kamiframe_lvgl_port and
 *  kamiframe_lua_port do: this is simulator-only orchestration around a
 *  Core mechanism, not a claim that the ESP32 build has it wired up." This
 *  port's CMakeLists.txt (see ../CMakeLists.txt) only points
 *  EXTRA_COMPONENT_DIRS at hakoniwaos -- simulator/src/pet, simulator/src/lvgl,
 *  and simulator/src/lua are not in the ESP32 component tree at all, and
 *  hakoniwaos/sources.cmake itself contains no LVGL or Lua files either.
 *
 *  So: no ADR 0017 pet screen (LVGL isn't reachable), no ADR 0018 demo
 *  creature (Lua isn't reachable), and no live pet state (kf_pet_session
 *  isn't reachable). What DOES run, for real, on real ESP-IDF, through the
 *  real ST7789/GPIO/NVS/deep-sleep HAL backends written for this slice: the
 *  same bouncing-sprite placeholder kf/demo.h's own header comment calls
 *  "the cheap case" -- proof the HAL backends actually work, not proof the
 *  game does. Porting the pet session (and, later, LVGL/Lua) onto ESP-IDF is
 *  real, separate, future work -- most likely each becoming its own
 *  EXTRA_COMPONENT_DIRS entry once there's real hardware to debug them
 *  against, not something to guess at compile-only.
 * ============================================================================
 */

#include "kf/app.h"
#include "kf/hal/log.h"

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_system.h"

#include <inttypes.h>
#include <stdio.h>

namespace {
constexpr const char *TAG = "app_main";
}

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
     * KF_DEMO_NONE: there is no LVGL screen to coordinate with on this
     * build (see header comment), so the placeholder content is the only
     * thing worth having on screen, and KF_DEMO_SPRITE is the cheap case
     * that best exercises the dirty-rect path esp_display.cpp's
     * kf_display_present() still ignores today. */
    KF_LOGI(TAG, "starting kf_app_init (real ESP32 HAL backends)");
    kf_app_init(KF_DEMO_SPRITE);

    KF_LOGI(TAG, "running (kf_app_frame loop; no quit condition on device)");
    while (kf_app_frame()) {
        /* kf_app_frame() paces itself against KF_FRAME_BUDGET_US via
         * kf_time_delay_us() (see hakoniwaos/src/app.cpp) -- no extra
         * vTaskDelay needed here, matching sdl_main.cpp's own bare
         * while-loop shape on desktop. */
    }

    /* Unreachable in practice: esp_input.cpp's kf_input_poll() hardcodes
     * quit_requested = false (there is no hardware "quit" concept), so
     * g.running in app.cpp never flips false on this backend. Kept anyway,
     * the same way sdl_main.cpp keeps its own shutdown call after a loop
     * that can exit -- correct shape if that ever changes, dead code if it
     * doesn't, and either way cheaper than leaving it out and being wrong
     * later. */
    kf_app_shutdown();
}
