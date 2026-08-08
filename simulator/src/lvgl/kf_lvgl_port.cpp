/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 */

#include "kf_lvgl_port.h"

#include "kf_lvgl_display.h"
#include "kf_lvgl_input.h"
#include "kf_lvgl_tick.h"
/* kf_lvgl_pointer.h's own header comment is explicit: "It has no ESP32
 * counterpart and never will" -- a mouse cursor is a desktop development
 * convenience, and kf_sim_pointer_poll() (the function it calls) is only
 * ever implemented by a desktop backend (sdl_input.cpp, headless_input.cpp).
 * ESP_PLATFORM is the same macro hakoniwaos/CMakeLists.txt already branches
 * on, so this stays the ONE copy of kf_lvgl_port.cpp -- not a fork -- for
 * the same reason ADR 0027 gives for every other file it ports unchanged. */
#ifndef ESP_PLATFORM
#include "kf_lvgl_pointer.h"
#endif

#include "kf/hal/log.h"

namespace {

constexpr const char *TAG = "lvgl";

void log_bridge(lv_log_level_t level, const char *buf) {
    /* LVGL's own level enum happens to run the same direction as kf's
     * (0 = most severe), but they are not the same enum, so map explicitly
     * rather than casting one onto the other. */
    kf_log_level kf_level = KF_LOG_INFO;
    switch (level) {
    case LV_LOG_LEVEL_ERROR:
        kf_level = KF_LOG_ERROR;
        break;
    case LV_LOG_LEVEL_WARN:
        kf_level = KF_LOG_WARN;
        break;
    case LV_LOG_LEVEL_INFO:
        kf_level = KF_LOG_INFO;
        break;
    default:
        kf_level = KF_LOG_DEBUG;
        break;
    }
    kf_log(kf_level, TAG, "%s", buf);
}

} // namespace

lv_group_t *kf_lvgl_port_init(void) {
    lv_init();
    lv_log_register_print_cb(log_bridge);

    kf_lvgl_display_init();
    lv_group_t *group = kf_lvgl_input_init();
#ifndef ESP_PLATFORM
    /* Desktop only -- see the #include guard above. `group` is still
     * returned below either way: ESP32 skips the mouse cursor, not the
     * keypad group itself. */
    kf_lvgl_pointer_init(group);
#endif
    kf_lvgl_tick_init();

    KF_LOGI(TAG, "LVGL %d.%d.%d ready (lv_obj, lv_label, lv_image, "
                 "lv_button, lv_bar)",
            LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);
    return group;
}

void kf_lvgl_port_pump(uint32_t synthetic_frame_delta_ms) {
    if (synthetic_frame_delta_ms == 0u) {
        kf_lvgl_tick_advance_real();
    } else {
        kf_lvgl_tick_advance_synthetic(synthetic_frame_delta_ms);
    }
    lv_timer_handler();
}

void kf_lvgl_port_shutdown(void) { lv_deinit(); }
