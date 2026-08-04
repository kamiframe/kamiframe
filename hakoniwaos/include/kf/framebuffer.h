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

/* How many independent dirty rectangles core tracks per frame before it
 * gives up and falls back to a single bounding box -- the behaviour the
 * very first version of this file always used.
 *
 * 8 is enough for a fixed UI element (a HUD, a name tag) plus a handful of
 * independently-moving things without ever falling back in the cases that
 * exist today. Past that, KF_DISPLAY_RECT_OVERHEAD_BYTES in kf/budget.h
 * means the per-rectangle addressing cost is already eating into whatever
 * the extra rectangles would have saved, so falling back stops being a
 * compromise and starts being the right answer anyway. See
 * docs/architecture/adr-0011-dirty-rect-list.md. */
#define KF_MAX_DIRTY_RECTS 8

typedef struct {
    kf_rect rects[KF_MAX_DIRTY_RECTS];
    int count;
} kf_dirty_rects;

/* Every write to the framebuffer must be inside a region that has been marked
 * dirty, or the device will not send it. Drawing functions in kf/blit.h do
 * this for you; if you write pixels by hand, mark them by hand.
 *
 * Tracked as up to KF_MAX_DIRTY_RECTS independent rectangles, not one
 * bounding box. A single box is cheap and never wrong, but it is only ever
 * as small as the smallest rectangle containing EVERYTHING that changed --
 * a fixed HUD in one corner and a sprite roaming the rest of the screen
 * combine into a box spanning nearly the whole panel, even though almost
 * none of it actually changed. Measured: adr-0010 found this pushed a
 * simple two-thing scene to 97% dirty. A rectangle LIST keeps unrelated
 * regions separate.
 *
 * Anything that touches or overlaps an existing tracked rectangle still
 * merges into it -- this only helps when regions AREN'T touching. Past
 * KF_MAX_DIRTY_RECTS, everything collapses into one box for the rest of the
 * frame, which is the exact same guarantee the original design always gave:
 * never wrong, only ever over-sends. */
void kf_fb_mark_dirty(kf_rect r);
void kf_fb_mark_all_dirty(void);

/* The tracked dirty rectangles for this frame. Empty (count == 0) if
 * nothing was drawn. Rectangles never overlap by construction. */
kf_dirty_rects kf_fb_dirty_rects(void);

/* Called by the frame loop after present. Not by game code. */
void kf_fb_clear_dirty(void);

/* Total PIXEL bytes across the current dirty rectangles, assuming a backend
 * that honours partial updates. This is what makes the transfer-cost
 * estimate track what you actually drew.
 *
 * Deliberately does NOT include KF_DISPLAY_RECT_OVERHEAD_BYTES -- that is a
 * per-rectangle protocol cost, not a pixel cost, and kf_app_log_budget_report
 * adds it in separately using kf_fb_dirty_rects().count, so the report can
 * show the two apart. */
size_t kf_fb_dirty_bytes(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KF_FRAMEBUFFER_H */
