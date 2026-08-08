/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 */

#include "kf_lvgl_display.h"

#include "kf/budget.h"
#include "kf/framebuffer.h"
#include "kf/hal/log.h"
#include "kf/types.h"

#include <cstring>

namespace {

constexpr const char *TAG = "lvgl-display";

/* LVGL's own compose buffer: the "small buffer holding the pixels actively
 * being sent to the panel" ADR 0013 calls out as belonging in fast internal
 * SRAM, same as the main framebuffer, not the PSRAM-backed KF_ARENA_LVGL
 * (that arena is LVGL's object/style heap, a different thing entirely). At
 * 24 rows this is 24 * 240 * 2 = 11,520 bytes -- small enough that, like the
 * SDL and headless backends' own file-static state, it does not need a named
 * entry in kf/budget.h to stay honest; it is fixed, permanent for the
 * process lifetime, and never grows. A single buffer, not double: flushing
 * here is a synchronous memcpy into kf_fb_pixels(), so there is nothing for
 * a second buffer to overlap with. */
constexpr int32_t kDrawBufRows = 24;
constexpr size_t kDrawBufBytes =
    static_cast<size_t>(KF_DISPLAY_WIDTH) * kDrawBufRows * sizeof(kf_color);
uint8_t g_draw_buf[kDrawBufBytes];

void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    kf_color *fb = kf_fb_pixels();
    const kf_color *src = reinterpret_cast<const kf_color *>(px_map);
    const int32_t width = area->x2 - area->x1 + 1;

    for (int32_t y = area->y1; y <= area->y2; ++y) {
        kf_color *dst_row = fb + (static_cast<size_t>(y) * KF_DISPLAY_WIDTH) +
                             area->x1;
        std::memcpy(dst_row, src, static_cast<size_t>(width) * sizeof(kf_color));
        src += width;
    }

    /* LVGL's own invalidated-area tracking feeds straight into core's
     * dirty-rectangle list -- see ADR 0013 -- so this area shows up in the
     * same budget report every other dirty rectangle does. */
    const kf_rect r{
        static_cast<int16_t>(area->x1), static_cast<int16_t>(area->y1),
        static_cast<int16_t>(area->x2 + 1), static_cast<int16_t>(area->y2 + 1)};
    kf_fb_mark_dirty(r);

    lv_display_flush_ready(disp);
}

} // namespace

lv_display_t *kf_lvgl_display_init(void) {
    lv_display_t *disp =
        lv_display_create(KF_DISPLAY_WIDTH, KF_DISPLAY_HEIGHT);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_set_buffers(disp, g_draw_buf, nullptr, sizeof(g_draw_buf),
                            LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_default(disp);

    /* LV_USE_THEME_SIMPLE only -- see lv_conf.h for why a theme turned out
     * to be necessary, not optional, despite the curated widget set having
     * nothing to do with it. */
    lv_display_set_theme(disp, lv_theme_simple_init(disp));

    /* static_cast<int>(kDrawBufRows), not kDrawBufRows bare: found on the
     * ESP32 target, not desktop -- xtensa-esp32s3-elf's <cstdint> typedefs
     * int32_t to `long`, a distinct type from `int` even though both are
     * 32 bits there, so %d (which only ever promises `int`) genuinely
     * mismatches kDrawBufRows's declared type on that one target. Desktop's
     * int32_t is plain `int`, so this was never visible there. Casting
     * makes the actual printf argument type match %d's contract on every
     * target, not just the one that happened to warn first. */
    KF_LOGI(TAG, "%dx%d RGB565, %d-row partial draw buffer (%zu bytes)",
            KF_DISPLAY_WIDTH, KF_DISPLAY_HEIGHT,
            static_cast<int>(kDrawBufRows), kDrawBufBytes);
    return disp;
}
