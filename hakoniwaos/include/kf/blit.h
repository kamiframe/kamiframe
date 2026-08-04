/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * Minimal drawing. Deliberately minimal.
 *
 * This is NOT the sprite engine. The sprite engine question (LVGL versus
 * something custom) is a separate evaluation and nothing here prejudges it:
 * this is a rectangle fill and a rectangular blit, enough to prove the
 * framebuffer and the HAL work end to end. Whatever wins that evaluation
 * sits above kf/framebuffer.h exactly as this does.
 *
 * All coordinates are in framebuffer space. All functions clip. All functions
 * mark the region they touched as dirty.
 */

#ifndef KF_BLIT_H
#define KF_BLIT_H

#include "kf/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Fill the whole framebuffer. Marks everything dirty. */
void kf_fill(kf_color color);

/* Fill a rectangle, clipped to the framebuffer. */
void kf_fill_rect(kf_rect r, kf_color color);

/* Draw `sprite` with its top-left at (x, y), clipped.
 *
 * If sprite->has_color_key, pixels equal to sprite->color_key are skipped.
 * There is no alpha blending: RGB565 has no alpha channel, and per-pixel
 * blending on a 240MHz core with no GPU is expensive enough that it should be
 * a deliberate decision later rather than an accident now. */
void kf_blit(const kf_sprite *sprite, int16_t x, int16_t y);

/* Intersection of two rects. Result may be empty. */
kf_rect kf_rect_intersect(kf_rect a, kf_rect b);

/* Smallest rect containing both. An empty operand returns the other. */
kf_rect kf_rect_union(kf_rect a, kf_rect b);

bool kf_rect_is_empty(kf_rect r);

/* Width * height. Zero if empty. */
uint32_t kf_rect_area(kf_rect r);

/* --------------------------------------------------------------------------
 * Drawing work counters
 *
 * Every drawing call counts the pixels it writes, split by how expensive each
 * kind is on the device. The frame loop turns the counts into an estimated
 * device draw time using KF_DRAW_*_PX_PER_US from kf/budget.h.
 *
 * Counting rather than timing is the point: host wall-clock time tells you
 * about your PC, and the pixel count tells you about the device. The count is
 * identical on every machine that runs this code.
 * -------------------------------------------------------------------------- */
typedef struct {
    uint32_t opaque_pixels; /* fills and un-keyed sprite rows: memcpy-shaped */
    uint32_t keyed_pixels;  /* colour-keyed blits: per-pixel test, slower */
} kf_draw_counters;

void kf_draw_counters_reset(void);
kf_draw_counters kf_draw_counters_get(void);

/* Let a drawing module that lives outside kf/blit.cpp -- today only
 * kf/font.h -- contribute to the same per-frame counters, so nothing drawn
 * on screen goes missing from the budget report. `keyed` picks the bucket
 * by cost shape, not by whether an actual colour key is involved: rows
 * copied whole are opaque, anything plotted pixel by pixel is keyed. */
void kf_draw_count_pixels(bool keyed, uint32_t count);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KF_BLIT_H */
