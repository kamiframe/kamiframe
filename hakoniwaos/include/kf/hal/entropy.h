/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * HAL: entropy.
 *
 * Four lines, and it is here rather than in core because esp_random() and the
 * host CSPRNG are genuinely different things.
 *
 * IMPORTANT: this is a SEED SOURCE, not the game's random number generator.
 * Anything the pet or a Lua game observes must come from a deterministic PRNG
 * in core, seeded once from here, so that replays reproduce, tests are stable,
 * and a save file behaves the same way twice. Calling kf_entropy() from game
 * logic is a bug.
 *
 * Valid C.
 */

#ifndef KF_HAL_ENTROPY_H
#define KF_HAL_ENTROPY_H

#include "kf/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KF_HAL_ENTROPY_VERSION 1

/* Fill `out` with `bytes` of entropy. Quality should be good enough to seed a
 * PRNG; it is not required to be cryptographic. Called rarely, at startup. */
kf_result kf_entropy(void *out, size_t bytes);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KF_HAL_ENTROPY_H */
