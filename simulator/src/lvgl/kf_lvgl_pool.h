/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * LVGL's memory pool, handed off to kf_arena_alloc(KF_ARENA_LVGL, ...).
 *
 * This is the only bridge between LVGL and Kamiframe's own memory model.
 * lv_conf.h wires it in via LV_MEM_POOL_INCLUDE/LV_MEM_POOL_ALLOC, and
 * LVGL's builtin allocator (src/stdlib/builtin/lv_mem_core_builtin.c) calls
 * it exactly once at lv_init() time, for exactly LV_MEM_SIZE bytes, then
 * carves it up itself (a TLSF pool) for every lv_malloc() after that. See
 * ADR 0013: LVGL never touches the system heap, so this does not violate
 * ADR 0008.
 *
 * Included from LVGL's own C source, so this must stay valid C.
 */

#ifndef KF_LVGL_POOL_H
#define KF_LVGL_POOL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void *kf_lvgl_mem_pool_alloc(size_t size);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KF_LVGL_POOL_H */
