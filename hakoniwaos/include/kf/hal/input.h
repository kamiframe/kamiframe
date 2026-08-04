/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * HAL: input.
 *
 * What this owns: reading the physical state of the buttons, right now.
 *
 * What this does NOT own: debounce, auto-repeat, edge detection, chords,
 * long-press, or anything else that constitutes "feel". All of that is core,
 * so that the simulator and the device behave identically. A backend that
 * debounces in hardware is welcome to, but core will debounce again and core's
 * timings are the ones that define the platform.
 *
 * Valid C.
 */

#ifndef KF_HAL_INPUT_H
#define KF_HAL_INPUT_H

#include "kf/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KF_HAL_INPUT_VERSION 1

typedef struct {
    /* Bitmask of kf_button. A set bit means the button is physically down
     * at the instant of sampling. Raw. Not debounced. */
    uint32_t buttons;

    /* Monotonic microseconds at which this sample was taken, from the same
     * clock as kf_time_mono_us(). Core needs this rather than "now" because
     * a backend may have sampled in an interrupt some time ago. */
    uint64_t sampled_at_us;

    /* True if the user asked the host to close the simulator window, or the
     * device's power button was held. Core should shut down cleanly.
     * Always false on backends with no such concept. */
    bool quit_requested;
} kf_input_raw;

kf_result kf_input_init(void);

/* Sample the buttons. Call once per frame. Cheap; must not block. */
kf_result kf_input_poll(kf_input_raw *out);

void kf_input_shutdown(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KF_HAL_INPUT_H */
