/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * HAL: memory pools.
 *
 * The narrow thing the HAL owns: handing core one large block per pool, once,
 * at startup, from memory with the right physical properties.
 *
 * Everything above that (arenas, bump allocation, high-water tracking, hard
 * failure on exhaustion) is core, in kf/arena.h, so it behaves identically on
 * both targets.
 *
 * Why pools exist at all: the ESP32-S3 has ~512KB of fast internal SRAM that
 * can drive DMA, and 8MB of slower external PSRAM that mostly cannot. Your
 * desktop has one flat heap and will therefore never tell you that you have
 * exhausted the small fast pool, which is the limit you will actually hit.
 * Making core ask for a specific pool is what makes that failure visible on
 * desktop, months before there is any hardware to discover it on.
 *
 * Valid C.
 */

#ifndef KF_HAL_MEMORY_H
#define KF_HAL_MEMORY_H

#include "kf/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KF_HAL_MEMORY_VERSION 1

typedef enum {
    /* Fast, DMA-capable. Internal SRAM on the device. Scarce. */
    KF_POOL_INTERNAL_DMA = 0,

    /* Large, slower, generally not DMA-capable. PSRAM on the device. */
    KF_POOL_EXTERNAL = 1,

    KF_POOL_COUNT = 2
} kf_mem_pool;

/* Acquire a block of `bytes` from `pool`, aligned to at least 32 bytes (the
 * S3's cache line, and enough for any DMA descriptor).
 *
 * Called a handful of times at startup and never again. There is deliberately
 * no free: nothing in Kamiframe returns memory to the pool, because a device
 * that runs for months must not fragment.
 *
 * Returns NULL if the pool cannot satisfy the request. Callers treat that as
 * fatal, because it means the budget in kf/budget.h does not fit the hardware
 * and no amount of runtime handling will change that. */
void *kf_mem_pool_acquire(kf_mem_pool pool, size_t bytes);

/* Total size of `pool` as this backend sees it. Used for reporting and for
 * the startup check that the budget actually fits. */
size_t kf_mem_pool_capacity(kf_mem_pool pool);

/* Human-readable name, for log messages. Never NULL. */
const char *kf_mem_pool_name(kf_mem_pool pool);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KF_HAL_MEMORY_H */
