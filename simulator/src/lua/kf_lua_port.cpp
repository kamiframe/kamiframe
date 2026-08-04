/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 */

#include "kf_lua_port.h"

#include "kf_lua_alloc.h"

#include "kf/hal/entropy.h"
#include "kf/hal/log.h"
#include "kf/hal/time.h"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include <cstring>

namespace {

constexpr const char *TAG = "lua";

struct State {
    lua_State *L = nullptr;
    bool ready = false;
    bool disabled_after_error = false;
    uint64_t last_call_us = 0;
    int64_t last_report = 0;
    uint32_t frame_count = 0;
};
State g;

/* kf.log(msg) -- a script's only way to reach the log, deliberately: no
 * io library means no print() destination anyway (Lua's stock print()
 * lives in the base library and writes to C's stdio, which is exactly the
 * kind of direct-to-host-console escape hatch a sandboxed cartridge should
 * not have on a device with no console attached). Routing through kf_log
 * means a Lua script's log lines go through the same tag/level/backend
 * machinery as everything else in the OS. */
int lua_kf_log(lua_State *L) {
    const char *msg = luaL_checkstring(L, 1);
    KF_LOGI(TAG, "%s", msg);
    return 0;
}

/* kf.report(n) -- lets a script hand a single integer back to C without
 * needing a real save/telemetry API yet. This slice's only consumer is the
 * headless determinism check (kamiframe-headless --verify-lua), which
 * calls kf_lua_port_last_report() after N frames and asserts it against a
 * value computed independently of Lua -- proof the VM, the allocator and
 * this binding all behaved identically run to run, not just that nothing
 * crashed. */
int lua_kf_report(lua_State *L) {
    g.last_report = static_cast<int64_t>(luaL_checkinteger(L, 1));
    return 0;
}

const luaL_Reg kKfFuncs[] = {
    {"log", lua_kf_log},
    {"report", lua_kf_report},
    {nullptr, nullptr},
};

void register_bindings(lua_State *L) {
    luaL_newlib(L, kKfFuncs);
    lua_setglobal(L, "kf");
}

} // namespace

bool kf_lua_port_init(const char *script_source, const char *chunk_name) {
    KF_ASSERT(!g.ready, "kf_lua_port_init called twice without an "
                        "intervening kf_lua_port_shutdown()");

    kf_lua_alloc_init();

    /* Lua 5.5's lua_newstate takes a hash seed (new: earlier versions
     * generated one internally). Deliberately NOT luaL_makeseed(), the
     * "convenient" default lauxlib.c itself uses -- that one mixes in
     * time(NULL) and a stack address, which makes string-keyed table
     * iteration order non-deterministic run to run. Everything else this
     * project seeds (kf/rng.h, via kf_app_init()'s own kf_entropy() call)
     * goes through the entropy HAL specifically so it can be pinned for
     * headless/CI determinism (see kf_host_entropy_pin() in the headless
     * backend) and left real for interactive play; Lua's hash seed gets
     * the identical treatment for the identical reason. See ADR 0014. */
    uint32_t seed = 0;
    KF_ASSERT(kf_entropy(&seed, sizeof(seed)) == KF_OK,
              "entropy HAL failed to start (kf_lua_port_init)");

    g.L = lua_newstate(kf_lua_alloc, nullptr, seed);
    KF_ASSERT(g.L != nullptr,
              "lua_newstate returned NULL -- the Lua state object itself "
              "did not fit in KF_ARENA_LUA_BYTES. That is a kf/budget.h "
              "sizing problem (the arena is too small to hold even an "
              "empty VM), not a script problem, so this panics rather "
              "than degrading -- see ADR 0014.");

    /* Sandboxed: base, coroutine, math, string, table, utf8. Deliberately
     * NOT loaded, and why:
     *   io      unrestricted file access. A cartridge has no filesystem of
     *           its own to see -- save state goes through kf/hal/storage.h
     *           when that binding exists, never raw files.
     *   os      os.execute, os.remove, os.getenv, and a wall clock that
     *           bypasses kf/hal/time.h's two-clock split (see ADR 0004) --
     *           every one of those is either a sandbox hole or a way to
     *           silently disagree with the HAL about what time it is.
     *   package (LUA_LOADLIBK) require() and dynamic module loading. There
     *           is no module system yet, and when there is one it will be
     *           the SDK's cartridge format, not the host filesystem.
     *   debug   can inspect and rewrite arbitrary stack frames and
     *           upvalues from other functions, which is precisely what a
     *           sandbox exists to prevent. Revisit only if in-cartridge
     *           tooling ever needs it, deliberately, later.
     * luaL_openselectedlibs is new in Lua 5.5 (see ADR 0014) -- exactly
     * this case: select libraries by bitmask instead of hand-rolling the
     * equivalent loop over luaL_requiref(). */
    luaL_openselectedlibs(g.L,
                          LUA_GLIBK | LUA_COLIBK | LUA_MATHLIBK |
                              LUA_STRLIBK | LUA_TABLIBK | LUA_UTF8LIBK,
                          /*preload=*/0);

    register_bindings(g.L);

    g.disabled_after_error = false;
    g.last_call_us = 0;
    g.last_report = 0;
    g.frame_count = 0;

    if (luaL_loadbuffer(g.L, script_source, std::strlen(script_source),
                         chunk_name) != LUA_OK) {
        KF_LOGE(TAG, "script failed to load: %s", lua_tostring(g.L, -1));
        lua_close(g.L);
        g.L = nullptr;
        return false;
    }

    if (lua_pcall(g.L, 0, 0, 0) != LUA_OK) {
        KF_LOGE(TAG, "script's top-level code raised an error: %s",
                lua_tostring(g.L, -1));
        lua_close(g.L);
        g.L = nullptr;
        return false;
    }

    g.ready = true;
    KF_LOGI(TAG, "Lua " LUA_VERSION_MAJOR "." LUA_VERSION_MINOR "."
                 LUA_VERSION_RELEASE " ready, script '%s' loaded",
            chunk_name);
    return true;
}

void kf_lua_port_frame(uint32_t synthetic_frame_delta_ms) {
    if (!g.ready || g.disabled_after_error) {
        return;
    }

    /* Same convention as kf_lvgl_port_pump: 0 means "use real elapsed
     * time", tracked here via kf_time_mono_us() -- see kf_lvgl_tick.h's
     * comment for why real and synthetic time must never be conflated.
     * The headless build always passes a fixed non-zero period; SDL always
     * passes literal 0. */
    uint32_t dt_ms = synthetic_frame_delta_ms;
    if (dt_ms == 0u) {
        const uint64_t now_us = kf_time_mono_us();
        dt_ms = g.last_call_us == 0u
                    ? 0u
                    : static_cast<uint32_t>(
                          (now_us >= g.last_call_us
                               ? now_us - g.last_call_us
                               : 0u) /
                          1000u);
        g.last_call_us = now_us;
    }

    lua_getglobal(g.L, "on_frame");
    if (!lua_isfunction(g.L, -1)) {
        lua_pop(g.L, 1);
        return; /* a script is not required to define on_frame */
    }
    lua_pushinteger(g.L, static_cast<lua_Integer>(dt_ms));
    if (lua_pcall(g.L, 1, 0, 0) != LUA_OK) {
        KF_LOGE(TAG,
                "on_frame raised an error, disabling further calls until "
                "the next kf_lua_port_init(): %s",
                lua_tostring(g.L, -1));
        lua_pop(g.L, 1);
        g.disabled_after_error = true;
        return;
    }
    g.frame_count++;
}

void kf_lua_port_shutdown() {
    if (!g.ready) {
        return;
    }
    KF_LOGI(TAG, "shutting down after %u frame(s)", g.frame_count);
    lua_close(g.L);
    g.L = nullptr;
    g.ready = false;
}

int64_t kf_lua_port_last_report() { return g.last_report; }
uint32_t kf_lua_port_frame_count() { return g.frame_count; }
