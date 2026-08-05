/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * HAL backend: input, headless.
 *
 * Replays a fixed script so a CI run exercises the button paths and still
 * produces identical output every time. Held long enough to survive core's
 * 8ms debounce, because a script that presses a button for one frame would
 * be correctly ignored and would test nothing.
 *
 * sampled_at_us is a SIMULATED per-frame clock, one KF_FRAME_BUDGET_US tick
 * per poll -- deliberately not kf_time_mono_us(). The headless runner turns
 * off real-time pacing (see kf_host_time_set_realtime in headless_main.cpp)
 * so 300 frames run in milliseconds rather than ten seconds, which means the
 * real host clock between polls reflects nothing but scheduler noise: how
 * many microseconds a slower CI runner, or a machine under load, happened to
 * take on that particular frame. Core's debounce compares that gap against
 * a fixed 8ms window, so feeding it real elapsed time made whether a scripted
 * button-hold cleared the debounce depend on host speed rather than frame
 * count -- the same seed could produce a different checksum from one run to
 * the next. A synthetic clock ticking one nominal frame per poll makes the
 * debounce see exactly what a correctly-paced 30fps run would, independent
 * of how fast this particular machine executes the loop. */

#include "kf/hal/input.h"

#include "kf/budget.h"
#include "headless_probe.h"

namespace {
uint64_t g_frame = 0;
uint64_t g_sim_us = 0;
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
    out->sampled_at_us = g_sim_us;
    out->quit_requested = false;
    g_frame++;
    g_sim_us += KF_FRAME_BUDGET_US;
    return KF_OK;
}

void kf_input_shutdown(void) {}

/* kf_lvgl_pointer.cpp's other half for this backend -- see sdl_input.cpp's
 * real implementation and kf_lvgl_pointer.h's header comment. Headless has
 * no mouse and no window; reporting "never pressed" here means the
 * pointer indev this links against is created (kf_lvgl_port_init() calls
 * kf_lvgl_pointer_init() unconditionally, both backends) but never
 * actually generates an event, so it has zero effect on any golden-
 * checksum test -- deliberately, not by omission. */
void kf_sim_pointer_poll(int32_t *x, int32_t *y, bool *pressed) {
    *x = 0;
    *y = 0;
    *pressed = false;
}
