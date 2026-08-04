/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * HAL backend: entropy, host implementation.
 *
 * Seeds the core PRNG once at startup and is never called again. The device
 * version is esp_random(). Neither is used for game logic; see kf/rng.h.
 */

#include "kf/hal/entropy.h"

#include <cstdint>
#include <cstring>
#include <random>

namespace {
bool g_fixed = false;
uint32_t g_fixed_value = 0;
} // namespace

/* Simulator-private: pin entropy so a headless run is byte-for-byte
 * reproducible. Declared here rather than in a header because only the
 * headless main calls it. */
extern "C" void kf_host_entropy_pin(uint32_t value) {
    g_fixed = true;
    g_fixed_value = value;
}

kf_result kf_entropy(void *out, size_t bytes) {
    if (out == nullptr || bytes == 0u) {
        return KF_ERR_INVALID;
    }

    uint8_t *dst = static_cast<uint8_t *>(out);

    if (g_fixed) {
        for (size_t i = 0; i < bytes; ++i) {
            dst[i] = static_cast<uint8_t>((g_fixed_value >> ((i % 4u) * 8u)) &
                                          0xFFu);
        }
        return KF_OK;
    }

    std::random_device rd;
    size_t written = 0;
    while (written < bytes) {
        const uint32_t value = rd();
        const size_t chunk = (bytes - written) < 4u ? (bytes - written) : 4u;
        std::memcpy(dst + written, &value, chunk);
        written += chunk;
    }
    return KF_OK;
}
