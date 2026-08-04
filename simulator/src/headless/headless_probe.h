/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * Simulator-private inspection hooks for the headless backend. Not HAL, not
 * visible to core.
 */

#ifndef KF_HEADLESS_PROBE_H
#define KF_HEADLESS_PROBE_H

#include <cstdint>

/* FNV-1a over every framebuffer presented so far. Deterministic for a given
 * seed and frame count, which is what makes it usable as a CI assertion. */
uint64_t kf_headless_checksum(void);

uint64_t kf_headless_frames(void);

/* Total pixels reported as dirty across the run. Guards against a change that
 * quietly starts redrawing the whole screen every frame: that would still
 * look correct, and would still be a regression, because on the device it is
 * the difference between 30fps and 60. */
uint64_t kf_headless_dirty_pixels(void);

/* Scripted button input, so CI can drive the app deterministically. Returns
 * the mask for the given frame. */
uint32_t kf_headless_script(uint64_t frame);

#endif /* KF_HEADLESS_PROBE_H */
