/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 */

#include "kf_lvgl_input.h"

#include "kf/app.h"
#include "kf/types.h"

namespace {

lv_group_t *g_group = nullptr;
uint32_t g_last_key = 0;

/* One button, one key, checked in a fixed priority order. A physical keypad
 * only ever has one thing "focused" at a time, and the buttons this project
 * has (kf/types.h's kf_button) map onto LVGL's keypad keys directly enough
 * that nothing fancier -- chords, multi-key -- is worth building for a
 * five-widget menu layer. */
uint32_t button_to_key(uint32_t buttons) {
    if (buttons & KF_BTN_UP) {
        return LV_KEY_UP;
    }
    if (buttons & KF_BTN_DOWN) {
        return LV_KEY_DOWN;
    }
    if (buttons & KF_BTN_LEFT) {
        return LV_KEY_LEFT;
    }
    if (buttons & KF_BTN_RIGHT) {
        return LV_KEY_RIGHT;
    }
    if (buttons & KF_BTN_A) {
        return LV_KEY_ENTER;
    }
    if (buttons & KF_BTN_B) {
        return LV_KEY_ESC;
    }
    if (buttons & KF_BTN_MENU) {
        /* Cycles focus to the next widget in the group -- the closest
         * keypad-native equivalent to a MENU button on hardware with no
         * pointer device. Core's own HUD toggle on KF_BTN_MENU (kf/app.h)
         * is unrelated and unaffected: this indev only ever moves LVGL's
         * own focus. */
        return LV_KEY_NEXT;
    }
    return 0u;
}

void read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
    (void)indev;
    const uint32_t held = kf_app_buttons_held();
    const uint32_t key = button_to_key(held);

    if (key != 0u) {
        g_last_key = key;
        data->key = key;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->key = g_last_key;
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

} // namespace

lv_group_t *kf_lvgl_input_init(void) {
    g_group = lv_group_create();
    lv_group_set_default(g_group);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(indev, read_cb);
    lv_indev_set_group(indev, g_group);

    return g_group;
}
