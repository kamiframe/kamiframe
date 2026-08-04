/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * Bridges LVGL's display driver to Kamiframe's existing framebuffer and
 * dirty-rectangle mechanism, rather than giving LVGL a screen of its own.
 * See ADR 0013.
 */

#ifndef KF_LVGL_DISPLAY_H
#define KF_LVGL_DISPLAY_H

#include <lvgl.h>

/* Creates the lv_display_t, sized to KF_DISPLAY_WIDTH x KF_DISPLAY_HEIGHT,
 * and points its flush callback at kf_fb_pixels()/kf_fb_mark_dirty(). Call
 * once, after kf_fb_init() (the framebuffer this writes into must already
 * exist) and after lv_init(). */
lv_display_t *kf_lvgl_display_init(void);

#endif /* KF_LVGL_DISPLAY_H */
