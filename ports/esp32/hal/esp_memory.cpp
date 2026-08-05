/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * HAL backend: memory pools, ESP32 implementation.
 *
 * This is the one file in this directory that actually tests the claim
 * kf/hal/memory.h's own header comment makes: that the S3 has two pools
 * with genuinely different properties, and that only the device can tell
 * you if you've exhausted the small fast one. heap_caps_malloc() below
 * either gets real internal-DMA-capable SRAM and real octal PSRAM from the
 * chip, or it doesn't -- there is no simulating this the way host_memory.cpp
 * simulates it with two static arrays.
 *
 * Every acquire is once, at startup, never freed -- same contract as the
 * desktop backend, and the same reason: nothing in Kamiframe returns pool
 * memory, because a device meant to run for months must not fragment.
 */

#include "kf/hal/memory.h"

#include "kf/hal/log.h"

#include "esp_heap_caps.h"

namespace {

constexpr const char *TAG = "mem";

struct PoolCaps {
    uint32_t caps;
    const char *name;
};

/* MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA: internal SRAM, DMA-capable -- the
 * exact combination README.md's own "What still has to be written" section
 * named for this pool. MALLOC_CAP_SPIRAM: octal PSRAM, per sdkconfig.defaults'
 * CONFIG_SPIRAM_MODE_OCT. If PSRAM isn't actually wired up correctly (or
 * sdkconfig.defaults' PSRAM support isn't compiled in for a given build),
 * heap_caps_malloc() for that pool simply returns NULL here -- which
 * kf_mem_pool_acquire()'s own documented contract already treats as fatal,
 * not something this file needs to special-case. */
constexpr PoolCaps kPoolCaps[KF_POOL_COUNT] = {
    {MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA, "internal-dma"},
    {MALLOC_CAP_SPIRAM, "external-psram"},
};

} // namespace

void *kf_mem_pool_acquire(kf_mem_pool pool, size_t bytes) {
    if (pool < 0 || pool >= KF_POOL_COUNT) {
        return nullptr;
    }
    const PoolCaps &p = kPoolCaps[pool];
    /* heap_caps_malloc() itself already returns an allocation aligned to
     * the cache line on this target -- no manual alignment bookkeeping
     * needed the way host_memory.cpp's static-array pools require. */
    void *ptr = heap_caps_malloc(bytes, p.caps);
    if (ptr == nullptr) {
        KF_LOGE(TAG, "pool '%s' could not supply %zu bytes (%zu total on this "
                     "chip)",
                p.name, bytes, heap_caps_get_total_size(p.caps));
    }
    return ptr;
}

size_t kf_mem_pool_capacity(kf_mem_pool pool) {
    if (pool < 0 || pool >= KF_POOL_COUNT) {
        return 0u;
    }
    /* The REAL figure this chip reports, not the KF_POOL_*_BYTES budget
     * figure in kf/budget.h -- those are the pessimistic working numbers
     * the budget arithmetic is checked against; this is what bring-up is
     * for measuring against them. */
    return heap_caps_get_total_size(kPoolCaps[pool].caps);
}

const char *kf_mem_pool_name(kf_mem_pool pool) {
    if (pool < 0 || pool >= KF_POOL_COUNT) {
        return "invalid";
    }
    return kPoolCaps[pool].name;
}
