/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * PLACEHOLDER CONTENT. This is the "something is on screen" of slice one and
 * nothing more.
 *
 * It lives inside hakoniwaos/ only because there is not yet any mechanism for
 * loading an application. Once the Lua runtime and the app loader exist, this
 * file is deleted and kf_app_frame() drives a loaded application instead. Do
 * not build anything on top of it.
 *
 * Two modes, and the second one exists to answer a specific question.
 */

#ifndef KF_DEMO_H
#define KF_DEMO_H

#include "kf/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    /* One sprite bouncing on a flat background, repainting only the patch it
     * moved off. The cheap case: about 2% of the screen changes per frame. */
    KF_DEMO_SPRITE = 0,

    /* A scrolling tilemap under several moving sprites: every pixel on screen
     * is redrawn, every frame, exactly as a 2D tile game with a scrolling
     * background would.
     *
     * This exists to answer "can the device animate the whole screen?"
     * empirically rather than by argument. Run it and read the budget report:
     * the drawing is cheap and the transfer is not, which is the shape of
     * this hardware. See docs/frame-budget.md. */
    KF_DEMO_FULLSCREEN = 1
} kf_demo_mode;

void kf_demo_init(uint32_t seed, kf_demo_mode mode);

/* `held` is the debounced button mask, `pressed` the buttons that went down
 * this frame. */
void kf_demo_update(uint32_t held, uint32_t pressed);

void kf_demo_draw(void);

void kf_demo_shutdown(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KF_DEMO_H */
