/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * HAL backend: entropy, ESP32 implementation.
 *
 * esp_random() is the real thing kf/hal/entropy.h's header comment already
 * named as "the device version" -- a hardware RNG, not a PRNG seeded from
 * uptime. Called once at startup to seed core's own deterministic PRNG
 * (kf/rng.h); never called from game logic. See host_entropy.cpp for the
 * desktop equivalent this file matches the shape of.
 */

#include "kf/hal/entropy.h"

#include "esp_random.h"

#include <cstring>

kf_result kf_entropy(void *out, size_t bytes) {
    if (out == nullptr || bytes == 0u) {
        return KF_ERR_INVALID;
    }

    uint8_t *dst = static_cast<uint8_t *>(out);
    size_t written = 0;
    while (written < bytes) {
        const uint32_t value = esp_random();
        const size_t chunk = (bytes - written) < 4u ? (bytes - written) : 4u;
        std::memcpy(dst + written, &value, chunk);
        written += chunk;
    }
    return KF_OK;
}
