/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * Fixed-size arena allocation. This is the spine of constraint enforcement.
 *
 * There is no malloc in core. Every allocation comes from one of the named
 * arenas below, each of which is a fixed block whose size lives in
 * kf/budget.h. Running out is fatal, immediately, on desktop as well as on
 * the device, because a limit you can exceed on desktop is not a limit.
 *
 * There is no free. Two lifetimes exist and neither needs one:
 *
 *   PERMANENT arenas (framebuffer, lua, assets) are allocated at startup and
 *   live until power off. Handing memory back would only invite
 *   fragmentation on a device meant to run for months.
 *
 *   The SCRATCH arena is reset to empty at the top of every frame. Allocation
 *   is a pointer bump, freeing is free, and a leak is impossible because
 *   nothing survives the frame. Anything needed next frame does not belong
 *   here.
 *
 * Valid C, so a C backend or a binding can use it.
 */

#ifndef KF_ARENA_H
#define KF_ARENA_H

#include "kf/budget.h"
#include "kf/hal/memory.h"
#include "kf/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    /* The one display buffer. Internal SRAM, DMA-capable. Permanent. */
    KF_ARENA_FRAMEBUFFER = 0,

    /* Per-frame temporaries. Internal SRAM. RESET EVERY FRAME. */
    KF_ARENA_SCRATCH = 1,

    /* Lua's heap, handed to lua_newstate as an allocator. PSRAM. Permanent.
     * Unused until the Lua slice; it is declared now so the budget arithmetic
     * in budget.h is honest about what has to fit. */
    KF_ARENA_LUA = 2,

    /* Decoded sprites and game data. PSRAM. Permanent. */
    KF_ARENA_ASSETS = 3,

    /* LVGL's object/style pool, handed to it via LV_MEM_POOL_ALLOC. PSRAM.
     * Permanent. Unused until the menu slice; see ADR 0013 and
     * KF_ARENA_LVGL_BYTES in budget.h. */
    KF_ARENA_LVGL = 4,

    KF_ARENA_COUNT = 5
} kf_arena_id;

typedef struct {
    const char *name;
    kf_mem_pool pool;
    size_t capacity_bytes;
    size_t used_bytes;
    size_t high_water_bytes; /* survives reset, so scratch use is visible */
    uint32_t alloc_count;
} kf_arena_stats;

/* Acquire every arena's backing block from the HAL. Call once, early, before
 * anything else in core. Panics if the budget does not fit the pools, because
 * that means kf/budget.h describes a device that does not exist. */
void kf_arena_init_all(void);

/* Bump-allocate. `align` must be a power of two; pass 0 for the default
 * (sizeof(void*)).
 *
 * NEVER RETURNS NULL. Exhaustion calls KF_PANIC with the arena name, the
 * requested size, the space remaining and a pointer at kf/budget.h. A caller
 * that has to check for failure is a caller that will be tempted to degrade
 * gracefully, and silently degrading is how a constraint stops being one. */
void *kf_arena_alloc(kf_arena_id arena, size_t bytes, size_t align);

/* Empty an arena. Only legal on KF_ARENA_SCRATCH; panics on the rest, since
 * resetting a permanent arena would invalidate live pointers. Called by the
 * frame loop, not by game code. */
void kf_arena_reset(kf_arena_id arena);

const kf_arena_stats *kf_arena_get_stats(kf_arena_id arena);

/* One line per arena to the log. Called by the periodic budget report. */
void kf_arena_log_all(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KF_ARENA_H */
