/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 */

#include "kf_lvgl_port.h"

#include "kf_lvgl_display.h"
#include "kf_lvgl_input.h"
#include "kf_lvgl_pointer.h"
#include "kf_lvgl_tick.h"

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
    kf_lvgl_pointer_init(group);
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
