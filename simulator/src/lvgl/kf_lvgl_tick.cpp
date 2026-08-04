/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 */

#include "kf_lvgl_tick.h"

#include "kf/hal/time.h"

#include <lvgl.h>

namespace {
uint64_t g_last_mono_us = 0;
bool g_started = false;
} // namespace

void kf_lvgl_tick_init(void) {
    g_last_mono_us = kf_time_mono_us();
    g_started = true;
}

void kf_lvgl_tick_advance_real(void) {
    if (!g_started) {
        kf_lvgl_tick_init();
        return;
    }
    const uint64_t now_us = kf_time_mono_us();
    const uint64_t delta_us =
        now_us >= g_last_mono_us ? now_us - g_last_mono_us : 0u;
    g_last_mono_us = now_us;
    lv_tick_inc(static_cast<uint32_t>(delta_us / 1000u));
}

void kf_lvgl_tick_advance_synthetic(uint32_t delta_ms) {
    lv_tick_inc(delta_ms);
}
