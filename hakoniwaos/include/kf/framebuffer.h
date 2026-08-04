/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * The one framebuffer, and the dirty-rectangle bookkeeping that goes with it.
 *
 * Core owns this, not the display backend. The backend receives a pointer to
 * it once per frame and never allocates one, which is what keeps the drawing
 * code identical on every target.
 *
 * There is exactly one, its dimensions come from kf/budget.h, and its type is
 * sized to those dimensions. That is deliberate: there is no path to a larger
 * surface, because the type for a larger surface does not exist.
 */

#ifndef KF_FRAMEBUFFER_H
#define KF_FRAMEBUFFER_H

#include "kf/budget.h"
#include "kf/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Allocate the framebuffer from KF_ARENA_FRAMEBUFFER and clear it. Call once,
 * after kf_arena_init_all(). */
void kf_fb_init(void);

/* The buffer. KF_DISPLAY_WIDTH * KF_DISPLAY_HEIGHT pixels, row-major, no
 * padding between rows. */
kf_color *kf_fb_pixels(void);

/* Every write to the framebuffer must be inside a region that has been marked
 * dirty, or the device will not send it. Drawing functions in kf/blit.h do
 * this for you; if you write pixels by hand, mark them by hand.
 *
 * The region is tracked as a single bounding box rather than a list. A box is
 * cheap, never wrong, and only ever over-sends. A rect list would send less
 * but can be got wrong in ways that show up as stale pixels on hardware and
 * not on desktop. Revisit only if measurement says the box is costing real
 * transfer time. */
void kf_fb_mark_dirty(kf_rect r);
void kf_fb_mark_all_dirty(void);

/* The accumulated dirty box for this frame. Empty if nothing was drawn. */
kf_rect kf_fb_dirty_rect(void);

/* Called by the frame loop after present. Not by game code. */
void kf_fb_clear_dirty(void);

/* Bytes the device would have to push for the current dirty box, assuming a
 * backend that honours partial updates. This is what makes the transfer-cost
 * estimate track what you actually drew. */
size_t kf_fb_dirty_bytes(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KF_FRAMEBUFFER_H */
