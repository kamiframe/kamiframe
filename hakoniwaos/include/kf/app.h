/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * The application entry points, and the frame budget report.
 *
 * ============================================================================
 *  CORE DOES NOT OWN THE LOOP. THE BACKEND DOES.
 *
 *  There is no while(running) anywhere in hakoniwaos/. Core exposes "run one
 *  frame" and each backend drives it in whatever way its platform requires:
 *
 *      desktop     while (!done) { kf_app_frame(); }
 *      WASM        emscripten_set_main_loop(kf_app_frame, 0, 1);
 *      ESP32       a FreeRTOS task calling kf_app_frame()
 *      CI          for (i = 0; i < n; i++) { kf_app_frame(); }
 *
 *  The browser is the reason. Emscripten cannot run a blocking loop without
 *  ASYNCIFY, which bloats and slows the build. Getting this shape right costs
 *  nothing today; retrofitting it later means unpicking a loop that by then
 *  owns state.
 * ============================================================================
 */

#ifndef KF_APP_H
#define KF_APP_H

#include "kf/demo.h"
#include "kf/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up arenas, framebuffer and every HAL module, then the demo content.
 * Panics rather than returning an error: there is no useful degraded mode
 * this early.
 *
 * The mode argument is temporary scaffolding and goes away with kf/demo.h
 * once there is a real application loader. */
void kf_app_init(kf_demo_mode mode);

/* Run exactly one frame: sample input, advance state, draw, present, account.
 * Returns false when the app should stop (the user closed the window, or the
 * device is powering down). */
bool kf_app_frame(void);

void kf_app_shutdown(void);

/* --------------------------------------------------------------------------
 * Frame budget accounting
 *
 * Measured every frame in every build configuration. Not a debug feature: a
 * budget you only measure when you remember to is a budget you will blow.
 * -------------------------------------------------------------------------- */

typedef struct {
    uint64_t frame_index;

    /* Wall time this frame's work actually took ON THIS MACHINE. Useful for
     * spotting simulator problems, and almost useless as a prediction: your
     * PC draws pixels roughly a hundred times faster than a 240MHz
     * microcontroller. Use draw_us instead when you want to know whether
     * something will fit on the device. */
    uint32_t cpu_us;

    /* ESTIMATED microseconds the device would spend DRAWING this frame,
     * computed from the number of pixels written and the rates in budget.h.
     * Host-independent: identical on a laptop and a workstation. */
    uint32_t draw_us;

    uint32_t opaque_pixels;
    uint32_t keyed_pixels;

    /* ESTIMATED microseconds the device's panel link would need for the
     * pixels this frame dirtied, from KF_DISPLAY_SPI_HZ in budget.h.
     *
     * This is the number desktop would otherwise report as zero, and it is
     * usually the largest single item in the budget: a full 240x320 RGB565
     * frame is 153,600 bytes, which at 40MHz is about 30ms of wire time.
     * Without it you will happily design a 60fps game that the hardware
     * cannot display. */
    uint32_t transfer_us;

    /* The two ways the device could spend a frame.
     *
     * serial_us     = draw_us + transfer_us. What it costs today: draw the
     *                 whole frame, then wait for all of it to go out.
     * overlapped_us = max(draw_us, transfer_us). What it costs with DMA and
     *                 a second buffer, because the previous frame goes out
     *                 while the CPU draws the next.
     *
     * Both are always reported, so the headroom double buffering would buy is
     * visible now rather than discovered later. On this hardware they are
     * usually close, because transfer dominates drawing by an order of
     * magnitude: you are limited by the wire, not the processor. */
    uint32_t serial_us;
    uint32_t overlapped_us;

    /* Whichever of the two applies given KF_DISPLAY_DOUBLE_BUFFERED. */
    uint32_t total_us;
    bool over_budget;

    /* Fraction of the screen this frame redrew, in percent. Watch this: the
     * gap between 100% and something small is the gap between 30fps and 60.
     * Computed from the sum of this frame's dirty rectangles, so it reflects
     * what actually gets sent, not the span between the furthest-apart two
     * things that changed. */
    uint8_t dirty_percent;

    /* How many separate rectangles this frame's dirty area was tracked as,
     * from kf/framebuffer.h's KF_MAX_DIRTY_RECTS list. Watch this alongside
     * dirty_percent: a low percent with a rect count pinned at
     * KF_MAX_DIRTY_RECTS means the list just fell back to one box again, and
     * dirty_percent is about to jump. */
    uint8_t dirty_rect_count;
} kf_frame_stats;

typedef struct {
    uint64_t frames;
    uint64_t over_budget_frames;
    uint32_t last_us;
    uint32_t mean_us;
    uint32_t p99_us;
    uint32_t worst_us;
} kf_frame_summary;

/* The debounced button state as of the most recently completed frame --
 * exactly what kf_demo_update() was called with. core's debounce is the only
 * one that exists (see kf/hal/input.h: backends report raw state, never
 * smoothed state), so anything else that wants "is this button down" reads
 * it from here rather than re-deriving it from raw HAL polls. Currently
 * used by the LVGL input port (simulator/src/lvgl) to drive a keypad-style
 * lv_indev_t off the same buttons the game sees, not a second, independently
 * debounced set. */
uint32_t kf_app_buttons_held(void);
uint32_t kf_app_buttons_pressed(void);

/* DEBUG/TEST ONLY -- same convention as kf_screen_nav_debug_advance()
 * (simulator/src/pet/kf_screen_nav.h): "same effect as a real press, just
 * callable without one," not a second input path. Sets the two fields
 * kf_app_buttons_held()/_pressed() read directly, bypassing kf_app_frame()'s
 * own HAL poll and 8ms debounce entirely.
 *
 * Exists because a headless check that never calls kf_app_frame() -- every
 * check built around kf_screen_nav_frame() and the per-screen update
 * functions it calls, which read these two exactly like a real button press
 * would -- has no other way to drive one. Task 4 of docs/superpowers/plans/
 * 2026-08-13-screens-clock-sleep.md is the first caller: the Settings
 * screen's four-field editor (simulator/src/pet/kf_lua_settings_screen.cpp)
 * needs individual LEFT/RIGHT/UP/DOWN/A presses to move its cursor and
 * change a value, one frame at a time, which kf_screen_nav_debug_advance()
 *'s fixed MENU/B edges cannot express.
 *
 * `pressed_edge` should be a subset of `held` -- a button cannot be newly
 * pressed without also being currently down -- but this is not enforced:
 * it is a test-only escape hatch, not part of the debounce contract itself,
 * and a caller that gets it wrong only confuses its own test. */
void kf_app_debug_set_buttons(uint32_t held, uint32_t pressed_edge);

const kf_frame_stats *kf_app_last_frame(void);

/* The on-screen constraint HUD (ADR 0006). Off by default, and deliberately
 * NOT bound to any button: MENU used to toggle it, which both stole a button
 * the player needs for screen navigation (ADR 0022) and put a
 * redrawn-every-frame overlay on the panel, which is a visible ripple on a
 * display with no tearing-effect signal.
 *
 * The same numbers are available without drawing anything: over serial once a
 * second via kf_app_log_budget_report(), on demand as JSON through the KFDBG
 * bridge (ADR 0030), and live in the desktop debug window. Reach for this only
 * when the numbers genuinely have to be burned into the framebuffer -- a
 * photograph of the panel, or a backend with no console. */
void kf_app_set_hud_visible(bool visible);
bool kf_app_hud_visible(void);

/* Rolling summary over the most recent window of frames (256). */
kf_frame_summary kf_app_frame_summary(void);

/* One multi-line report: timing, arenas, dirty area. The frame loop calls
 * this about once a second; backends may call it whenever. */
void kf_app_log_budget_report(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KF_APP_H */
