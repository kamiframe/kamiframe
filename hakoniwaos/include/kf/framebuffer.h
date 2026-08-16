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
 * RAISED FROM 8 TO 32 on 2026-08-14, before it bound rather than after,
 * because the way it fails is unusually confusing: everything works, then
 * one more moving object makes the whole screen redraw and the frame gets
 * several times more expensive, with no visible cause. Measured on the
 * desktop model at the old limit: 8 moving sprites cost 2,403us, and 16 cost
 * 12,372us -- a 5x jump for 2x the sprites, entirely because the list
 * collapsed to one bounding box covering 57,763 pixels.
 *
 * 8 was a fair number when the only screen was a pet standing still. It is
 * the wrong number for a game with a dozen moving things, which is what this
 * platform is for.
 *
 * WHAT 32 COSTS. The array is kf_rect (8 bytes) x 32 = 256 bytes of static
 * storage, against 153,600 for the framebuffer itself -- immaterial. On the
 * wire, KF_DISPLAY_RECT_OVERHEAD_BYTES (kf/budget.h) is 11 bytes of
 * addressing per rectangle, so a worst-case 32-rectangle frame spends 352
 * bytes on overhead where 8 spent 88. Both are noise beside a 153,600-byte
 * full frame, and the point of not collapsing is to avoid sending most of
 * those 153,600 bytes at all.
 *
 * THE OLD REASONING WAS NOT WRONG, it was answering a different question.
 * It said that past 8 rectangles the addressing cost eats what the extra
 * rectangles save -- true when the alternative is a slightly larger union,
 * false when the alternative is the ENTIRE SCREEN. The coalescer
 * (hakoniwaos/src/scene.cpp) already merges pairs whenever merging is
 * genuinely cheaper, so a frame only reaches 32 rectangles when 32 is
 * actually the economical answer; this constant sets where "and now give up
 * completely" begins, not where merging starts.
 *
 * Matches KF_SCENE_MAX_DIRTY_CANDIDATES (kf/scene.h), which is also 32 --
 * the scene's coalescer works within that many candidates before handing the
 * result here, so a smaller number on this side would have thrown away work
 * it had already done. They are not required to be equal, but 8 against 32
 * meant the coalescer's careful merging was routinely discarded one step
 * later. See docs/architecture/adr-0011-dirty-rect-list.md for the original
 * decision. */
#define KF_MAX_DIRTY_RECTS 32

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
 * nothing was drawn.
 *
 * NOT a claim that the rectangles never overlap -- they can, and do.
 * kf_fb_mark_dirty() merges a new rectangle into the FIRST existing
 * rectangle it touches or overlaps and returns immediately; it never
 * re-scans to check whether that merged rectangle now touches or
 * overlaps any of the OTHERS. Three marks in the right order (A, then C
 * that doesn't touch A, then B that touches both) leave rect[0] =
 * union(A, B) overlapping rect[1] = C. kf_fb_dirty_bytes() sums each
 * rectangle's area independently, so an overlap like that is double-
 * counted there too -- treat its result as an upper bound on bytes
 * touched, not an exact count. This merging behaviour is deliberate
 * (see kf_fb_mark_dirty()'s own comment) and is not being changed here;
 * this comment only corrects what it guarantees. */
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
