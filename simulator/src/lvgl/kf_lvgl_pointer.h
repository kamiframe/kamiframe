/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * A pointer lv_indev_t for the desktop simulator's mouse -- a development
 * convenience, not a claim about real hardware. kf_lvgl_input.h's own
 * header comment already draws the important line: the keypad indev it
 * creates is "not a touchscreen: this hardware has none," and that is
 * still true. This is a SEPARATE, additional indev, on top of the keypad
 * one, not a replacement for it -- the keypad indev still models exactly
 * what the real 5-7 physical buttons drive, unchanged. This one exists
 * purely because clicking a button on screen is a faster, more natural way
 * to test the pet screen while developing than reaching for the keyboard,
 * the same reasoning as every other simulator-only affordance in this
 * codebase (e.g. sdl_main.cpp's window title stats). It has no ESP32
 * counterpart and never will, the same way `--stress` mode does not model
 * anything a real device does either.
 */

#ifndef KF_LVGL_POINTER_H
#define KF_LVGL_POINTER_H

#include <lvgl.h>

/* Creates the pointer indev and, if `group` is non-NULL, assigns it the
 * same group the keypad indev (kf_lvgl_input_init()) uses -- so clicking a
 * button also moves keypad focus onto it, keeping the two input methods
 * visually in sync rather than clicking silently leaving the keyboard's
 * last-focused widget highlighted. Call once, after kf_lvgl_input_init().
 *
 * Backed by kf_sim_pointer_poll(), implemented once per desktop backend
 * (sdl_input.cpp reports the real mouse; headless_input.cpp reports
 * "never pressed, never moved" so this has zero effect on any
 * golden-checksum test) -- the same per-backend-implementation pattern
 * kf/hal/input.h's kf_input_poll() already uses. */
void kf_lvgl_pointer_init(lv_group_t *group);

#endif /* KF_LVGL_POINTER_H */
