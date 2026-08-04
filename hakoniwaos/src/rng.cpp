/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 */

#include "kf/rng.h"

#include <cstdint>

#include "kf/poison.h"

namespace {
uint32_t g_state = 0x2545F491u;
}

void kf_rng_seed(uint32_t seed) {
    g_state = (seed == 0u) ? 0x2545F491u : seed;
}

uint32_t kf_rng_next(void) {
    uint32_t x = g_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_state = x;
    return x;
}

uint32_t kf_rng_below(uint32_t bound) {
    if (bound == 0u) {
        return 0u;
    }
    /* Rejection sampling, so small bounds are not biased toward low values.
     * Costs an occasional extra draw and removes a class of "why does the pet
     * always evolve into the first one" bug. */
    const uint32_t limit = 0xFFFFFFFFu - (0xFFFFFFFFu % bound);
    uint32_t value;
    do {
        value = kf_rng_next();
    } while (value >= limit);
    return value % bound;
}
