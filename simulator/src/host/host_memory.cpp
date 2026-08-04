/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * HAL backend: memory pools, host implementation.
 *
 * The pools are STATIC ARRAYS, not malloc. That is the whole point: on the
 * device these are physically distinct chips with fixed sizes, and the only
 * way desktop can tell you that you have outgrown one is to give you exactly
 * as much as the device has and no more.
 *
 * If this file used malloc, every constraint above it would be decorative.
 */

#include "kf/hal/memory.h"

#include "kf/budget.h"
#include "kf/hal/log.h"

#include <cstddef>
#include <cstdint>

namespace {

constexpr const char *TAG = "mem";

/* alignas(32) matches the S3's cache line and is enough for any DMA
 * descriptor, so alignment behaviour is the same on both targets. */
alignas(32) uint8_t g_internal[KF_POOL_INTERNAL_BYTES];
alignas(32) uint8_t g_external[KF_POOL_PSRAM_BYTES];

struct PoolState {
    uint8_t *base;
    size_t capacity;
    size_t used;
    const char *name;
};

PoolState g_pools[KF_POOL_COUNT] = {
    {g_internal, KF_POOL_INTERNAL_BYTES, 0, "internal-dma"},
    {g_external, KF_POOL_PSRAM_BYTES, 0, "external-psram"},
};

size_t align_up(size_t v, size_t a) { return (v + (a - 1u)) & ~(a - 1u); }

} // namespace

void *kf_mem_pool_acquire(kf_mem_pool pool, size_t bytes) {
    if (pool < 0 || pool >= KF_POOL_COUNT) {
        return nullptr;
    }
    PoolState &p = g_pools[pool];
    const size_t offset = align_up(p.used, 32u);
    if (offset + bytes > p.capacity) {
        KF_LOGE(TAG, "pool '%s' cannot supply %zu bytes (%zu of %zu used)",
                p.name, bytes, p.used, p.capacity);
        return nullptr;
    }
    p.used = offset + bytes;
    return p.base + offset;
}

size_t kf_mem_pool_capacity(kf_mem_pool pool) {
    if (pool < 0 || pool >= KF_POOL_COUNT) {
        return 0u;
    }
    return g_pools[pool].capacity;
}

const char *kf_mem_pool_name(kf_mem_pool pool) {
    if (pool < 0 || pool >= KF_POOL_COUNT) {
        return "invalid";
    }
    return g_pools[pool].name;
}
