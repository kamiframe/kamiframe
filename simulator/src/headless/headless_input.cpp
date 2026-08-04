/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * HAL backend: input, headless.
 *
 * Replays a fixed script so a CI run exercises the button paths and still
 * produces identical output every time. Held long enough to survive core's
 * 8ms debounce, because a script that presses a button for one frame would
 * be correctly ignored and would test nothing.
 */

#include "kf/hal/input.h"

#include "kf/hal/time.h"
#include "headless_probe.h"

namespace {
uint64_t g_frame = 0;
}

uint32_t kf_headless_script(uint64_t frame) {
    /* At 30fps each frame is ~33ms, comfortably past the debounce window. */
    if (frame >= 60 && frame < 70) {
        return KF_BTN_RIGHT;
    }
    if (frame >= 90 && frame < 100) {
        return KF_BTN_A; /* changes the background, forces a full repaint */
    }
    if (frame >= 120 && frame < 130) {
        return KF_BTN_DOWN | KF_BTN_LEFT;
    }
    if (frame >= 150 && frame < 160) {
        return KF_BTN_B; /* reverses direction */
    }
    return 0u;
}

kf_result kf_input_init(void) { return KF_OK; }

kf_result kf_input_poll(kf_input_raw *out) {
    if (out == nullptr) {
        return KF_ERR_INVALID;
    }
    out->buttons = kf_headless_script(g_frame);
    out->sampled_at_us = kf_time_mono_us();
    out->quit_requested = false;
    g_frame++;
    return KF_OK;
}

void kf_input_shutdown(void) {}
