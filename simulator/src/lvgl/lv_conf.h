/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * Kamiframe's curated LVGL configuration. See docs/architecture/adr-0013.
 *
 * This is NOT a copy of lv_conf_template.h with things commented out.
 * lv_conf_internal.h already gives every setting a sane default when this
 * file doesn't define it, so what follows is only the settings that
 * genuinely need to differ from LVGL's own defaults: which memory this
 * library is allowed to touch, and which widgets a contributor is meant to
 * see. Anything not mentioned here is stock LVGL.
 *
 * Widgets: only lv_obj (always built in), lv_label, lv_image, lv_button and
 * lv_bar are enabled -- menus, stat displays and settings, per ADR 0013.
 * Everything else in LVGL's 30+ widget catalog is switched off below, not
 * because it doesn't work, but because a casual contributor should see five
 * things to learn, not thirty. Turning one back on is a one-line change.
 */

#ifndef LV_CONF_H
#define LV_CONF_H

/* Some of LVGL's own sources are assembled directly (SIMD blend routines,
 * *.S, preprocessed but never compiled as C) and pick up this same
 * lv_conf.h along the way. kf/budget.h's KF_STATIC_ASSERT lines are not
 * valid assembly, and none of those routines care about LV_MEM_SIZE, so this
 * whole block -- pool size included -- is C/C++-only. */
#ifndef __ASSEMBLER__

/* Single source of truth for the pool size below, so it cannot drift from
 * what kf/arena.h actually hands out. Valid C, safe to include here. */
#include "kf/budget.h"

/*====================
 * COLOR SETTINGS
 *====================*/

/* RGB565, matching kf_color exactly (kf/types.h) -- this is LVGL's own
 * default, spelled out because ADR 0013 leans on it being exact, not
 * coincidental. */
#define LV_COLOR_DEPTH 16

/*=========================
 * STDLIB WRAPPER SETTINGS
 *=========================*/

#define LV_USE_STDLIB_MALLOC    LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_STRING    LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_SPRINTF   LV_STDLIB_BUILTIN

/* The whole point of the exercise: one fixed block from KF_ARENA_LVGL, never
 * the system heap. LV_MEM_SIZE is what LVGL asks kf_lvgl_mem_pool_alloc()
 * for at lv_init() time, and it must equal the arena's own capacity -- see
 * kf_lvgl_pool.cpp and the static assert in kf/budget.h that keeps this
 * inside the PSRAM pool's real size. */
#define LV_MEM_SIZE              KF_ARENA_LVGL_BYTES
#define LV_MEM_POOL_EXPAND_SIZE  0
#define LV_MEM_ADR               0 /* 0: use LV_MEM_POOL_ALLOC below, not a fixed address */
#define LV_MEM_POOL_INCLUDE      "kf_lvgl_pool.h"
#define LV_MEM_POOL_ALLOC        kf_lvgl_mem_pool_alloc

#endif /* !__ASSEMBLER__ */

/*=================
 * OPERATING SYSTEM
 *=================*/

/* No RTOS on desktop, and none assumed on the device yet either -- see
 * kf/hal/time.h's two clocks. LVGL's own internal timing runs off
 * lv_tick_inc(), driven by the port glue, not this. */
#define LV_USE_OS LV_OS_NONE

/*====================
 * LOGGING
 *====================*/

/* Routed through kf_log (kf/hal/log.h), the same place every other subsystem
 * in this project logs to, rather than LVGL's own printf default. See
 * kf_lvgl_port.cpp for the registered callback. */
#define LV_USE_LOG            1
#define LV_LOG_LEVEL          LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF         0

/*==================
 * WIDGETS
 *
 * lv_obj, lv_label, lv_image, lv_button and lv_bar are LVGL's own defaults
 * and stay that way. Everything else below is explicitly switched off.
 *================*/

#define LV_USE_ANIMIMG      0
#define LV_USE_ARC          0
#define LV_USE_BUTTONMATRIX 0
#define LV_USE_CALENDAR     0
#define LV_USE_CANVAS       0
#define LV_USE_CHART        0
#define LV_USE_CHECKBOX     0
#define LV_USE_DROPDOWN     0
#define LV_USE_IMAGEBUTTON  0
#define LV_USE_KEYBOARD     0
#define LV_USE_LED          0
#define LV_USE_LINE         0
#define LV_USE_LIST         0
#define LV_USE_LOTTIE       0
#define LV_USE_MENU         0
#define LV_USE_MSGBOX       0
#define LV_USE_ROLLER       0
#define LV_USE_SCALE        0
#define LV_USE_SLIDER       0
#define LV_USE_SPAN         0
#define LV_USE_SPINBOX      0
#define LV_USE_SPINNER      0
#define LV_USE_SWITCH       0
#define LV_USE_TEXTAREA     0
#define LV_USE_TABLE        0
#define LV_USE_TABVIEW      0
#define LV_USE_TILEVIEW     0
#define LV_USE_WIN          0

/*==================
 * THEMES
 *
 * Simple only -- "a very simple theme that is a good starting point for a
 * custom theme" (LVGL's own description), not the full default theme with
 * its animations and rounded-corner styling. Found necessary empirically,
 * not assumed: with no theme at all, lv_obj/lv_label have no base styling,
 * which on this project's black demo background meant black-on-black --
 * LVGL was genuinely drawing (the headless checksum test proved that) but a
 * human looking at the simulator window could not see it. A "proof screen"
 * that only proves something to a checksum defeats the point.
 *==================*/

#define LV_USE_THEME_DEFAULT 0
#define LV_USE_THEME_SIMPLE  1
#define LV_USE_THEME_MONO    0

/*==================
 * LAYOUTS
 *
 * Off for the same reason as themes: the proof screen positions its one
 * object directly. Flex/grid are a real, likely need for actual menu
 * screens later -- ADR 0013's "Later" section -- not this slice.
 *==================*/

#define LV_USE_FLEX 0
#define LV_USE_GRID 0

#endif /* LV_CONF_H */
