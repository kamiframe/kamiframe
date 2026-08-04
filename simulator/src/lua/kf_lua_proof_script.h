/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * PLACEHOLDER CONTENT, same spirit as kf/demo.h and
 * kf_lvgl_proof_screen.h: this is "Lua runs, the allocator survives real
 * churn, and both binding directions work," not a pet or a real cartridge.
 * There is no cartridge format yet -- see ADR 0014. Do not build on top of
 * this; delete it once one exists.
 *
 * What it proves, and why each part is here:
 *   - loads and runs at all, sandboxed (kf_lua_port.cpp's
 *     luaL_openselectedlibs call)
 *   - kf.log() and kf.report() both work (Lua calling into C)
 *   - on_frame(dt_ms) works (C calling into Lua, with an argument)
 *   - the allocator's free/realloc path survives real pressure: 32 fresh
 *     string concatenations built and then abandoned EVERY frame, plus a
 *     forced full collection every 30th frame. A bump-only allocator (see
 *     kf_lua_alloc.h's opening comment for why kf_arena_alloc() alone is
 *     not enough) would exhaust KF_ARENA_LUA_BYTES within a few hundred
 *     frames of this; a correct one runs indefinitely.
 *   - the whole pipeline is deterministic: `total` after N calls to
 *     on_frame is always exactly 32*N, independent of dt_ms and of real
 *     time, which is what kamiframe-headless --verify-lua asserts against.
 */

#ifndef KF_LUA_PROOF_SCRIPT_H
#define KF_LUA_PROOF_SCRIPT_H

inline constexpr const char *kKfLuaProofScriptSource = R"lua(
local total = 0
local frame = 0

function on_frame(dt_ms)
    frame = frame + 1

    -- Allocate and discard every frame -- the allocator's realloc/free
    -- path under real pressure, not just its alloc path. See the header
    -- comment on this file for what a broken allocator would do here.
    local scratch = {}
    for i = 1, 32 do
        scratch[i] = "kf-" .. tostring(i) .. "-" .. tostring(dt_ms)
    end
    total = total + #scratch

    if frame % 30 == 0 then
        kf.log("frame " .. frame .. ", total " .. total)
        collectgarbage("collect")
    end

    kf.report(total)
end

kf.log("proof script loaded")
)lua";

/* Leading '=' is Lua's own convention for "show this name verbatim in
 * error messages" (as opposed to a leading '@', which means "this is a
 * filename" -- see lua_load's documentation). Neither is true of a
 * compiled-in string, so verbatim is the honest choice. */
inline constexpr const char *kKfLuaProofScriptChunkName = "=proof_script";

#endif /* KF_LUA_PROOF_SCRIPT_H */
