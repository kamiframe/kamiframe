/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 */

#include "kf_lvgl_proof_screen.h"

#include <lvgl.h>

void kf_lvgl_proof_screen_init(void) {
    lv_obj_t *screen = lv_screen_active();

    lv_obj_t *label = lv_label_create(screen);
    lv_label_set_text(label, "LVGL: menus live here");
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 24);

    lv_obj_t *bar = lv_bar_create(screen);
    lv_obj_set_size(bar, 160, 16);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 65, LV_ANIM_OFF);
    lv_obj_align(bar, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *button = lv_button_create(screen);
    lv_obj_align(button, LV_ALIGN_BOTTOM_MID, 0, -24);
    lv_obj_t *button_label = lv_label_create(button);
    lv_label_set_text(button_label, "OK");
    lv_obj_center(button_label);

    /* The one thing this screen exists to prove the keypad bridge can
     * reach: MENU cycles focus (kf_lvgl_input.h), A/ENTER activates. */
    lv_group_t *group = lv_group_get_default();
    if (group != nullptr) {
        lv_group_add_obj(group, button);
    }
}
