/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * Ties the Lua port glue together: the arena-backed allocator
 * (kf_lua_alloc.h), a sandboxed lua_State, the kf.* binding surface, and one
 * loaded script. One call to bring Lua up, one call per frame to run the
 * script's on_frame, one call to tear it down.
 *
 * This is simulator-only, on purpose -- the same reasoning ADR 0013 already
 * gave for LVGL: Lua is not wired into hakoniwaos/ (core) or ports/esp32/ in
 * this slice, only into the desktop and headless backends, so nothing here
 * claims ESP32-readiness it has not earned. See ADR 0014.
 */

#ifndef KF_LUA_PORT_H
#define KF_LUA_PORT_H

#include <cstdint>

/* Brings the Lua VM up: kf_lua_alloc_init(), a sandboxed set of standard
 * libraries (see ADR 0014 for exactly which and why), the kf.* bindings,
 * then loads and runs `script_source` as its top-level chunk.
 *
 * `script_source` must be a NUL-terminated Lua chunk and must outlive this
 * call (it is handed to Lua's loader directly, not copied). `chunk_name` is
 * whatever Lua should call it in error messages (Lua's own convention: a
 * leading '@' means "this is a filename", '=' means "show verbatim",
 * anything else is quoted -- see luaL_loadbuffer's documentation via
 * lua_load).
 *
 * Returns false if the chunk failed to compile or its own top-level code
 * raised an error while running; either way the reason is logged through
 * kf_log, never thrown as a C++ exception (core builds with exceptions
 * off). A false return leaves the VM torn down, not half-initialised: call
 * kf_lua_port_shutdown() unconditionally either way if you want to retry
 * with different source, same as every other init/shutdown pair in this
 * codebase. */
bool kf_lua_port_init(const char *script_source, const char *chunk_name);

/* Calls the script's global on_frame(dt_ms) function, if it defined one, in
 * a protected call (lua_pcall) -- a script error here cannot bring down the
 * process, only be logged and reported.
 *
 * `synthetic_frame_delta_ms`: 0 means "use real elapsed time" (tracked
 * internally via kf_time_mono_us(), the same monotonic clock every other
 * frame-timing consumer in this project uses). Non-zero means "advance by
 * exactly this many milliseconds, not real time". Exactly kf_lvgl_port_pump
 * 's parameter, same convention, same reason: see kf_lvgl_tick.h's comment
 * for why real and synthetic time must never be conflated.
 *
 * A script that raises an error here is logged ONCE and on_frame is not
 * called again until the next kf_lua_port_init(): a script erroring every
 * frame should not spam the log 30 times a second. */
void kf_lua_port_frame(uint32_t synthetic_frame_delta_ms);

void kf_lua_port_shutdown();

/* The value most recently passed to kf.report() from Lua, and how many
 * times on_frame ran to completion without raising an error. Used by the
 * headless determinism check (kamiframe-headless --verify-lua) to assert
 * the whole pipeline -- VM, allocator, bindings -- behaves identically run
 * to run, and by the constraint HUD later. */
int64_t kf_lua_port_last_report();
uint32_t kf_lua_port_frame_count();

#endif /* KF_LUA_PORT_H */
