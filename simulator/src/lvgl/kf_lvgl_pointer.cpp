/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 */

#include "kf_lvgl_pointer.h"

/* Implemented once per desktop backend, exactly the way kf/hal/input.h's
 * kf_input_poll() is -- see this file's own header comment. Coordinates
 * are already in the logical 240x320 framebuffer space kf_pet_screen.cpp's
 * widgets are laid out in, not raw window pixels; each backend that can
 * report a real position is responsible for its own scale conversion
 * (sdl_input.cpp divides by the SDL window's integer scale factor). */
void kf_sim_pointer_poll(int32_t *x, int32_t *y, bool *pressed);

namespace {

void read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
    (void)indev;
    int32_t x = 0;
    int32_t y = 0;
    bool pressed = false;
    kf_sim_pointer_poll(&x, &y, &pressed);
    data->point.x = x;
    data->point.y = y;
    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

} // namespace

void kf_lvgl_pointer_init(lv_group_t *group) {
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, read_cb);
    if (group != nullptr) {
        lv_indev_set_group(indev, group);
    }
}
