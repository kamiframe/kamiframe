/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 */

#include "kf/arena.h"

#include "kf/hal/log.h"
#include "kf/hal/memory.h"

#include <cstdint>
#include <cstring>

/* Must be last. See kf/poison.h. */
#include "kf/poison.h"

namespace {

constexpr const char *TAG = "arena";

struct ArenaDesc {
    const char *name;
    kf_mem_pool pool;
    size_t capacity;
    bool resettable;
};

/* Order must match kf_arena_id exactly. */
constexpr ArenaDesc kDescs[KF_ARENA_COUNT] = {
    {"framebuffer", KF_POOL_INTERNAL_DMA, KF_ARENA_FRAMEBUFFER_BYTES, false},
    {"scratch", KF_POOL_INTERNAL_DMA, KF_ARENA_SCRATCH_BYTES, true},
    {"lua", KF_POOL_EXTERNAL, KF_ARENA_LUA_BYTES, false},
    {"assets", KF_POOL_EXTERNAL, KF_ARENA_ASSETS_BYTES, false},
};

struct ArenaState {
    uint8_t *base = nullptr;
    kf_arena_stats stats{};
};

ArenaState g_arenas[KF_ARENA_COUNT];
bool g_initialised = false;

size_t align_up(size_t value, size_t align) {
    return (value + (align - 1u)) & ~(align - 1u);
}

bool is_power_of_two(size_t v) { return v != 0u && (v & (v - 1u)) == 0u; }

} // namespace

void kf_arena_init_all(void) {
    KF_ASSERT(!g_initialised, "kf_arena_init_all called twice");

    /* Check the budget fits the hardware before carving anything up, so the
     * failure names the whole problem rather than whichever arena happened to
     * be unlucky. */
    for (int p = 0; p < KF_POOL_COUNT; ++p) {
        const kf_mem_pool pool = static_cast<kf_mem_pool>(p);
        size_t wanted = 0;
        for (int a = 0; a < KF_ARENA_COUNT; ++a) {
            if (kDescs[a].pool == pool) {
                wanted += align_up(kDescs[a].capacity, 32u);
            }
        }
        const size_t have = kf_mem_pool_capacity(pool);
        KF_ASSERT(wanted <= have,
                  "Pool '%s' is too small for the budget: arenas want %zu "
                  "bytes, pool has %zu. This budget does not fit the "
                  "hardware. Fix the numbers in kf/budget.h.",
                  kf_mem_pool_name(pool), wanted, have);
        KF_LOGI(TAG, "pool %-13s %8zu / %8zu bytes committed",
                kf_mem_pool_name(pool), wanted, have);
    }

    for (int a = 0; a < KF_ARENA_COUNT; ++a) {
        const ArenaDesc &d = kDescs[a];
        void *block = kf_mem_pool_acquire(d.pool, d.capacity);
        KF_ASSERT(block != nullptr,
                  "Could not acquire %zu bytes for arena '%s' from pool '%s'. "
                  "See kf/budget.h.",
                  d.capacity, d.name, kf_mem_pool_name(d.pool));

        g_arenas[a].base = static_cast<uint8_t *>(block);
        g_arenas[a].stats.name = d.name;
        g_arenas[a].stats.pool = d.pool;
        g_arenas[a].stats.capacity_bytes = d.capacity;
        g_arenas[a].stats.used_bytes = 0;
        g_arenas[a].stats.high_water_bytes = 0;
        g_arenas[a].stats.alloc_count = 0;
    }

    g_initialised = true;
    KF_LOGI(TAG, "%d arenas ready", KF_ARENA_COUNT);
}

void *kf_arena_alloc(kf_arena_id arena, size_t bytes, size_t align) {
    KF_ASSERT(g_initialised, "kf_arena_alloc before kf_arena_init_all");
    KF_ASSERT(arena >= 0 && arena < KF_ARENA_COUNT, "bad arena id %d",
              static_cast<int>(arena));

    if (align == 0u) {
        align = sizeof(void *);
    }
    KF_ASSERT(is_power_of_two(align), "alignment %zu is not a power of two",
              align);

    ArenaState &s = g_arenas[arena];
    const size_t offset = align_up(s.stats.used_bytes, align);
    const size_t end = offset + bytes;

    /* The hard stop. No graceful degradation on purpose: a caller that can
     * handle failure is a caller that will quietly do less on the device than
     * it does on desktop, and then the two stop matching. */
    KF_ASSERT(end <= s.stats.capacity_bytes,
              "Arena '%s' exhausted.\n"
              "  requested   : %zu bytes (aligned to %zu)\n"
              "  in use      : %zu bytes\n"
              "  capacity    : %zu bytes\n"
              "  short by    : %zu bytes\n"
              "This is the device's real limit, not a simulator artefact. "
              "Either use less, or change the budget in kf/budget.h in a "
              "commit of its own.",
              s.stats.name, bytes, align, s.stats.used_bytes,
              s.stats.capacity_bytes, end - s.stats.capacity_bytes);

    uint8_t *ptr = s.base + offset;
    s.stats.used_bytes = end;
    s.stats.alloc_count++;
    if (end > s.stats.high_water_bytes) {
        s.stats.high_water_bytes = end;
    }

    memset(ptr, 0, bytes);
    return ptr;
}

void kf_arena_reset(kf_arena_id arena) {
    KF_ASSERT(g_initialised, "kf_arena_reset before kf_arena_init_all");
    KF_ASSERT(arena >= 0 && arena < KF_ARENA_COUNT, "bad arena id %d",
              static_cast<int>(arena));
    KF_ASSERT(kDescs[arena].resettable,
              "Arena '%s' is permanent and cannot be reset. Resetting it "
              "would leave live pointers dangling. Only '%s' is resettable.",
              kDescs[arena].name, kDescs[KF_ARENA_SCRATCH].name);

    g_arenas[arena].stats.used_bytes = 0;
    g_arenas[arena].stats.alloc_count = 0;
}

const kf_arena_stats *kf_arena_get_stats(kf_arena_id arena) {
    KF_ASSERT(arena >= 0 && arena < KF_ARENA_COUNT, "bad arena id %d",
              static_cast<int>(arena));
    return &g_arenas[arena].stats;
}

void kf_arena_log_all(void) {
    for (int a = 0; a < KF_ARENA_COUNT; ++a) {
        const kf_arena_stats &s = g_arenas[a].stats;
        const unsigned pct =
            s.capacity_bytes == 0u
                ? 0u
                : static_cast<unsigned>((s.high_water_bytes * 100u) /
                                        s.capacity_bytes);
        KF_LOGI(TAG, "  %-12s %7zu peak / %7zu cap  (%3u%%)  pool=%s",
                s.name, s.high_water_bytes, s.capacity_bytes, pct,
                kf_mem_pool_name(s.pool));
    }
}
