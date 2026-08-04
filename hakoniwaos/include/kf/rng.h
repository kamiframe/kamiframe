/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * The game-visible random number generator.
 *
 * Deliberately deterministic, and deliberately NOT the HAL's entropy source.
 * kf_entropy() seeds this once at startup and is never called again. Anything
 * the pet or a Lua game observes comes from here, so that:
 *
 *   - a save file behaves the same way twice,
 *   - the headless CI backend produces identical frames every run,
 *   - a bug report can include a seed and actually reproduce.
 *
 * xorshift32. Not cryptographic, not trying to be. Small, fast, no state to
 * allocate, and identical on every target, which is the property that matters.
 */

#ifndef KF_RNG_H
#define KF_RNG_H

#include "kf/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A zero seed is silently replaced, since xorshift is stuck at zero. */
void kf_rng_seed(uint32_t seed);

uint32_t kf_rng_next(void);

/* Uniform in [0, bound). Returns 0 if bound is 0. */
uint32_t kf_rng_below(uint32_t bound);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KF_RNG_H */
