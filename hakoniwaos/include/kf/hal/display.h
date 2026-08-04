/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * HAL: display.
 *
 * What this owns: getting a finished framebuffer onto a physical panel, and
 * telling the core what kind of panel it is.
 *
 * What this does NOT own: the framebuffer itself (core allocates it), any
 * drawing, any clipping, any format conversion visible to core. If a panel
 * wants byte-swapped 565, the backend swaps. Core never knows.
 *
 * Valid C.
 */

#ifndef KF_HAL_DISPLAY_H
#define KF_HAL_DISPLAY_H

#include "kf/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bumped whenever this interface changes shape. Third-party backends can
 * check it. */
#define KF_HAL_DISPLAY_VERSION 1

typedef enum {
    KF_PIXFMT_RGB565 = 0,
    KF_PIXFMT_MONO1 = 1,  /* reserved for future e-ink / mono variants */
    KF_PIXFMT_GRAY4 = 2   /* reserved */
} kf_pixel_format;

/* Runtime capabilities.
 *
 * Core must read dimensions from here rather than hardcoding them, so that a
 * future e-ink or mono variant is a backend rather than a fork. KF_DISPLAY_*
 * in budget.h is the compile-time size of the buffer core allocates; this is
 * what the panel in front of you actually is. On the v1 hardware they match,
 * and app.cpp asserts that they do. */
typedef struct {
    uint16_t width;
    uint16_t height;
    kf_pixel_format format;

    /* True if present() can be given a sub-rectangle and will only push those
     * pixels. False means every present is a full frame regardless of the
     * dirty rect, which is what makes e-ink slow. */
    bool supports_partial_update;

    /* True if the backend has a controllable backlight. */
    bool has_backlight;

    /* Nominal bytes-per-second the panel link can sustain, used by core's
     * frame budget report to estimate transfer cost. Zero means "unknown, do
     * not estimate" (a desktop backend may report the real device figure on
     * purpose so the estimate stays honest). */
    uint32_t link_bytes_per_second;
} kf_display_caps;

/* Bring the panel up. Called once, before any present. */
kf_result kf_display_init(void);

/* Never fails. Valid after init. */
const kf_display_caps *kf_display_get_caps(void);

/* Push a frame.
 *
 * `framebuffer` points at caps.width * caps.height pixels in caps.format,
 * always the full buffer, always native-endian.
 *
 * `dirty` names the sub-rectangle that changed since the last present. The
 * backend may honour it or ignore it; core must supply it honestly either
 * way. Desktop ignores it today. The device will not, and this parameter
 * exists on day one precisely so that adding partial updates later is a
 * backend change rather than an audit of every call site.
 *
 * An empty dirty rect is legal and means "nothing changed", which a backend
 * may use to skip the transfer entirely.
 *
 * Blocks until the frame is accepted. On the device that will mean waiting
 * for a DMA slot, not for the transfer to complete. */
kf_result kf_display_present(const kf_color *framebuffer, kf_rect dirty);

/* 0 = off, 255 = full. Returns KF_ERR_UNAVAILABLE if !caps.has_backlight. */
kf_result kf_display_set_backlight(uint8_t level);

void kf_display_shutdown(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KF_HAL_DISPLAY_H */
