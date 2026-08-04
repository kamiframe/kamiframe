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
 * Three modes now, not two -- see KF_DEMO_NONE below for why a third one
 * became necessary rather than optional, the same way ADR 0013 found a
 * theme necessary rather than optional once there was something real on
 * screen to look at.
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
    KF_DEMO_FULLSCREEN = 1,

    /* Draws and moves nothing at all -- kf_demo_update()/kf_demo_draw()
     * become no-ops. For a real LVGL screen (ADR 0017's pet screen and
     * whatever follows it): KF_DEMO_SPRITE's bouncing sprite continuously
     * erases the small patch it moved off of, and LVGL's partial-render
     * mode only re-flushes pixels where ITS OWN object tree changed, so
     * every erased patch that overlapped a static widget stayed erased
     * permanently -- visible as a black trail slowly consuming the whole
     * screen. See docs/architecture/adr-0017-pet-screen.md's "Found after
     * delivery" section for the diagnosis. Golden-checksum tests
     * (headless_determinism, headless_dirty_area, headless_fullscreen)
     * keep using KF_DEMO_SPRITE/KF_DEMO_FULLSCREEN unchanged -- they are
     * about the demo mechanism itself, not about anything LVGL owns. */
    KF_DEMO_NONE = 2
} kf_demo_mode;

void kf_demo_init(uint32_t seed, kf_demo_mode mode);

/* `held` is the debounced button mask, `pressed` the buttons that went down
 * this frame. */
void kf_demo_update(uint32_t held, uint32_t pressed);

void kf_demo_draw(void);

void kf_demo_shutdown(void);

/* Force the next kf_demo_draw() to repaint the whole background instead of
 * just the sprite's dirty patch. KF_DEMO_SPRITE only has any effect;
 * KF_DEMO_FULLSCREEN already repaints everything every frame.
 *
 * Exists for kf/app.h: something drawn OUTSIDE the demo (the budget HUD)
 * can leave stale pixels behind when it stops drawing, and the demo is the
 * only thing that knows its own background colour well enough to clear
 * them. This is the one crack the demo's placeholder status is allowed to
 * show through kf/app.h; it goes away with kf/demo.h itself. */
void kf_demo_request_full_repaint(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KF_DEMO_H */
