/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * THE one per-frame sequence a running build drives after kf_app_frame()
 * returns, shared by both interactive backends -- simulator/src/sdl/
 * sdl_main.cpp on desktop, ports/esp32/main/app_main.cpp on device -- instead
 * of being maintained twice. See kf_frame_loop.cpp's header comment for why
 * this exists (ADR 0058) and exactly what it does and does not cover.
 *
 * Lives in simulator/src/pet/, alongside kf_pet_session.h and
 * kf_screen_nav.h, which is awkward -- this file's ESP32-side caller is a
 * device port, not a "simulator" in any sense -- but it follows the
 * established pattern exactly rather than inventing a new one:
 * kf_pet_session.cpp and kf_screen_nav.cpp are already compiled straight
 * into ports/esp32/main by relative path (../main/CMakeLists.txt SRCS) from
 * this same directory, "one canonical copy, not a fork". This file's own
 * dependencies (kf_pet_session.h, kf_screen_nav.h, sdk/lua's kf_lua_port.h/
 * kf_lua_scene.h) are the exact set kf_screen_nav.cpp already has, so it
 * belongs where they live.
 */

#ifndef KF_FRAME_LOOP_H
#define KF_FRAME_LOOP_H

#include <cstdint>

/* Optional per-backend extension point. Pass nullptr (or a zeroed struct)
 * for "no hook" -- ports/esp32/main/app_main.cpp does exactly that; the
 * device has nothing to plug in here (see kf_frame_loop.cpp's own comment
 * on why the debug window is desktop-only). */
struct kf_frame_loop_hooks {
    /* Run once, after kf_pet_session_frame() and before
     * kf_screen_nav_frame() -- the exact slot sdl_main.cpp's
     * kf_sdl_debug_window_frame() call already occupied before this
     * extraction. Nullable. */
    void (*after_pet_session)(void);
};

/* Runs one frame's worth of the shared sequence: computes this frame's real
 * elapsed time, drags the wall clock along with `multiplier` (see
 * kf_frame_loop.cpp for the full reasoning -- this is the exact mechanism
 * whose desktop/device fork produced the 2026-08-12/13 offline-fast-forward
 * bug, d1ab8a7), ticks the pet session and the active screen, pumps LVGL
 * when built in, runs one Lua frame, and commits the scene if Lua declared
 * anything this session.
 *
 * `multiplier`: this frame's debug time-speed multiplier, already read by
 * the caller from whichever accessor its own debug bridge exposes
 * (kf_sdl_debug_window_time_multiplier() on desktop,
 * kf_dbg_time_multiplier() on device). The two bridges are genuinely
 * different mechanisms -- a second SDL window vs a serial protocol -- so
 * reading the value stays the caller's job; this function only ever
 * consumes the result. Pass 1 for "no multiplier".
 *
 * `hooks`: may be nullptr, equivalent to a kf_frame_loop_hooks with
 * after_pet_session == nullptr.
 *
 * Returns this frame's real (un-multiplied) elapsed milliseconds. Neither
 * current caller needs the return value for its own bookkeeping (device
 * times the whole call from outside instead, for kf_app_post_frame_us() --
 * see app_main.cpp's own comment), but it costs nothing to expose and
 * avoids a future caller needing a second, redundant kf_time_mono_us()
 * scheme of its own.
 *
 * Call once per iteration of the port's own frame loop, after
 * kf_app_frame() has returned true for this frame. Does NOT call
 * kf_app_frame() itself, and does NOT run the debug bridge
 * (kf_dbg_bridge_frame()) -- that has to run BEFORE kf_app_frame() so a
 * queued button press affects the input poll kf_app_frame() does first
 * thing, which is earlier than this function is ever reached; see
 * app_main.cpp's own comment on that ordering. */
uint32_t kf_frame_loop_run(uint32_t multiplier, const kf_frame_loop_hooks *hooks);

#endif /* KF_FRAME_LOOP_H */
