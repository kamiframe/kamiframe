/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * A keypad lv_indev_t driven by the SAME debounced buttons the game reads
 * (kf_app_buttons_held()/kf_app_buttons_pressed()), not a touchscreen: this
 * hardware has none. See ADR 0013 and kf/hal/input.h's "backends report raw
 * state, core debounces once" rule -- this is a second CONSUMER of that one
 * debounce, not a second debouncer.
 */

#ifndef KF_LVGL_INPUT_H
#define KF_LVGL_INPUT_H

#include <lvgl.h>

/* Creates the keypad indev and a group for it to drive. Call once, after
 * lv_init(). Widgets that should be reachable with the buttons need adding
 * to the returned group with lv_group_add_obj(); nothing is added
 * automatically. */
lv_group_t *kf_lvgl_input_init(void);

#endif /* KF_LVGL_INPUT_H */
