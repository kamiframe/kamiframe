/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * Set a widget's value ONLY when it would actually change.
 *
 * ============================================================================
 *  WHY THIS EXISTS, AND WHY EVERY SCREEN MUST USE IT
 *
 *  LVGL does not diff for you. lv_label_set_text() and lv_bar_set_value()
 *  invalidate the widget's area whether or not the value differs, so a screen
 *  that refreshes its widgets every frame -- the obvious way to write an
 *  update function -- marks itself permanently dirty even when nothing about
 *  it has changed.
 *
 *  On a desktop that is invisible. On the real panel it is the difference
 *  between a still image and a visibly rippling one. The glass is scanned out
 *  of the panel's own memory on its own clock, and nothing on this board
 *  wires up a tearing-effect signal to say when writing is safe, so a
 *  permanent stream of writes leaves a permanently moving boundary between
 *  old and new pixels. Measured on hardware: an idle pet screen reported
 *  "dirty 7% (4 rects)" every frame and burned 2446us of wire time per frame
 *  producing pixels identical to the ones already on the glass.
 *
 *  So this is not a performance helper. A screen that updates unconditionally
 *  is a visibly broken screen, and the bug does not reproduce on desktop.
 *  Route every per-frame widget update through these.
 * ============================================================================
 *
 * The comparison is against the widget's own current value rather than a
 * cached copy kept beside it. That matters: a cache is a second source of
 * truth that can drift out of step with the widget after any code path that
 * sets it directly, and it has to be reset whenever the screen is rebuilt.
 * Asking the widget costs a pointer dereference and a short strcmp.
 */

#ifndef KF_LVGL_IDEMPOTENT_H
#define KF_LVGL_IDEMPOTENT_H

#include <lvgl.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

/* Longest label this project draws is well under this; the formatted result
 * is truncated rather than heap-allocated, matching the fixed-size, no-heap
 * discipline the rest of the codebase keeps. */
#define KF_LVGL_LABEL_MAX 64

inline void kf_lvgl_set_label(lv_obj_t *label, const char *text) {
    if (label == nullptr || text == nullptr) {
        return;
    }
    const char *current = lv_label_get_text(label);
    if (current != nullptr && std::strcmp(current, text) == 0) {
        return;
    }
    lv_label_set_text(label, text);
}

inline void kf_lvgl_set_label_fmt(lv_obj_t *label, const char *fmt, ...) {
    if (label == nullptr || fmt == nullptr) {
        return;
    }
    char buf[KF_LVGL_LABEL_MAX];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    kf_lvgl_set_label(label, buf);
}

inline void kf_lvgl_set_bar(lv_obj_t *bar, int32_t value) {
    if (bar == nullptr) {
        return;
    }
    if (lv_bar_get_value(bar) == value) {
        return;
    }
    lv_bar_set_value(bar, value, LV_ANIM_OFF);
}

#endif /* KF_LVGL_IDEMPOTENT_H */
