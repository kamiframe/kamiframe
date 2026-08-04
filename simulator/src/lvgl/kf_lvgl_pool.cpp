/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 */

#include "kf_lvgl_pool.h"

#include "kf/arena.h"
#include "kf/hal/log.h"

namespace {
constexpr const char *TAG = "lvgl-pool";
}

void *kf_lvgl_mem_pool_alloc(size_t size) {
    /* Called exactly once, at lv_init() time, for LV_MEM_SIZE bytes (see
     * lv_conf.h -- pinned to KF_ARENA_LVGL_BYTES so the two cannot drift).
     * kf_arena_alloc never returns NULL: an undersized arena is a
     * kf/budget.h problem, not something this call site should paper over. */
    KF_LOGI(TAG, "handing LVGL %zu bytes from KF_ARENA_LVGL", size);
    return kf_arena_alloc(KF_ARENA_LVGL, size, 0);
}
