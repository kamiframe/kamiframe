/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 */

#include "kf_lua_port.h"

#include "kf_lua_alloc.h"
#include "kf_lua_scene.h"

#include "../pet/kf_creature_presenter.h"
#include "../pet/kf_pet_session.h"

#include "kf/hal/entropy.h"
#include "kf/hal/log.h"
#include "kf/hal/time.h"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include <cstdio>
#include <cstring>

namespace {

constexpr const char *TAG = "lua";

struct State {
    lua_State *L = nullptr;
    bool ready = false;
    bool disabled_after_error = false;
    uint64_t last_call_us = 0;
    /* kf_lua_port_info_frame()'s OWN real-elapsed-time tracker -- see that
     * function's own comment for why it cannot share last_call_us above.
     * Reset alongside it in kf_lua_port_init(), for the identical reason:
     * a stale timestamp surviving an init/shutdown/init cycle would
     * compute one bogus, oversized dt_ms on the first post-reinit call. */
    uint64_t last_info_call_us = 0;
    int64_t last_report = 0;
    uint32_t frame_count = 0;

    /* Task 5 of the Lua game-layer plan: see kf_lua_port.h's own comment on
     * kf_lua_port_set_home_screen_active(). The compile-time default lives
     * HERE, in the member initializer (evaluated once, at process start,
     * before main() even runs) rather than being re-applied inside kf_lua_
     * port_init() every time -- kf_lua_port_init() runs the script's
     * top-level chunk as part of the SAME call, and that chunk needs to see
     * whatever value the LAST kf_lua_port_set_home_screen_active() call
     * left (or this default, if none ever ran) at the moment IT runs, not
     * a value kf_lua_port_init() has just reset out from under a caller
     * that set it moments earlier. kamiframe-headless --verify-screen-
     * parity is exactly that caller: one binary, built with one KF_HOME_
     * SCREEN default, overriding this before EACH kf_lua_port_init() call
     * so the SAME script's top-level `if kf.home_screen_active() then`
     * gate goes the right way for both halves of the comparison. */
#ifdef KF_HOME_SCREEN_LUA
    bool home_screen_active = true;
#else
    bool home_screen_active = false;
#endif
    /* Copy of the error on_frame most recently raised, if disabled_after_
     * error is true -- kept in addition to the KF_LOGE this project already
     * emits, so a screen's error banner can show the same words rather than
     * "something went wrong, check the log a device in someone's pocket has
     * no way to show". Truncated, never unterminated. */
    char last_error[160] = {};
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

/* kf.home_screen_active() -- Task 5 of the Lua game-layer plan. True when
 * THIS build's KF_HOME_SCREEN chose "lua" (or the headless parity check has
 * overridden the flag for this half of its comparison), false under
 * KF_HOME_SCREEN=cpp. examples/creature_demo/creature.lua reads this ONCE,
 * at the top level, to decide whether to declare a screen at all -- the
 * same single script file runs under either build, and only one
 * implementation may ever declare into the shared scene at a time (kf/
 * scene.h has no notion of "two games sharing a screen"). See kf_lua_port.h
 * 's own comment on why this is a runtime flag rather than a second
 * compiled script. */
int lua_kf_home_screen_active(lua_State *L) {
    lua_pushboolean(L, g.home_screen_active ? 1 : 0);
    return 1;
}

const luaL_Reg kKfFuncs[] = {
    {"log", lua_kf_log},
    {"report", lua_kf_report},
    {"home_screen_active", lua_kf_home_screen_active},
    {nullptr, nullptr},
};

void register_bindings(lua_State *L) {
    luaL_newlib(L, kKfFuncs);
    lua_setglobal(L, "kf");
}

/* pet.* -- read the live pet (kf_pet_session.h, ADR 0016) and act on it.
 * Registered unconditionally, the same as kf.* above, whether or not a
 * given script uses it: the cost of an unused binding is nothing, and it
 * keeps this file the single place every global a script can see gets
 * wired up. Requires kf_pet_session_init() to already have run before any
 * script calls one of these -- a host wiring order this codebase treats
 * the same as "arenas before LVGL" or "arenas before Lua": a caller
 * mistake, not a script mistake, so it asserts rather than degrading (see
 * kf_pet_session.cpp). */
int lua_pet_hunger(lua_State *L) {
    lua_pushinteger(
        L, static_cast<lua_Integer>(kf_pet_session_state()->hunger_mp));
    return 1;
}

int lua_pet_happiness(lua_State *L) {
    lua_pushinteger(
        L, static_cast<lua_Integer>(kf_pet_session_state()->happiness_mp));
    return 1;
}

int lua_pet_energy(lua_State *L) {
    lua_pushinteger(
        L, static_cast<lua_Integer>(kf_pet_session_state()->energy_mp));
    return 1;
}

/* pet.feed(variation)/play(variation)/rest(variation)/clean(variation) --
 * `variation` is optional and defaults to 0 (luaL_optinteger, not
 * luaL_checkinteger): a script that does not care about variations, like
 * the demo creature's pet.feed(), keeps working unchanged rather than
 * erroring for omitting an argument it has no opinion about. */
int lua_pet_feed(lua_State *L) {
    const uint8_t variation =
        static_cast<uint8_t>(luaL_optinteger(L, 1, 0));
    kf_pet_session_feed(variation);
    return 0;
}

int lua_pet_play(lua_State *L) {
    const uint8_t variation =
        static_cast<uint8_t>(luaL_optinteger(L, 1, 0));
    kf_pet_session_play(variation);
    return 0;
}

int lua_pet_rest(lua_State *L) {
    const uint8_t variation =
        static_cast<uint8_t>(luaL_optinteger(L, 1, 0));
    kf_pet_session_rest(variation);
    return 0;
}

int lua_pet_bath(lua_State *L) {
    const uint8_t variation =
        static_cast<uint8_t>(luaL_optinteger(L, 1, 0));
    kf_pet_session_bath(variation);
    return 0;
}

/* pet.flush() takes no argument, unlike the four care actions -- there is
 * one way to clear up poops and the creature has no view on it. */
int lua_pet_flush(lua_State *L) {
    (void)L;
    kf_pet_session_flush();
    return 0;
}

/* Mess, readable from a script. Poops are a plain count and dirtiness is
 * millipercent like the three needs, so a creature script can react to
 * "there are three poops down" or "we are past the flies threshold" the
 * same way it already reacts to hunger. */
int lua_pet_poops(lua_State *L) {
    lua_pushinteger(
        L, static_cast<lua_Integer>(kf_pet_session_state()->poop_count));
    return 1;
}

int lua_pet_dirtiness(lua_State *L) {
    lua_pushinteger(
        L, static_cast<lua_Integer>(kf_pet_session_state()->dirtiness_mp));
    return 1;
}

/* Illness, readable from a script. neglect_seconds is exposed raw rather
 * than as a "distress level" enum because where the thresholds sit is
 * config: a script that wants three bands can compute them, and one that
 * wants five is not blocked by a choice made here. */
int lua_pet_sick(lua_State *L) {
    lua_pushboolean(L, kf_pet_session_state()->sick ? 1 : 0);
    return 1;
}

int lua_pet_dead(lua_State *L) {
    lua_pushboolean(L, kf_pet_session_state()->dead ? 1 : 0);
    return 1;
}

int lua_pet_neglect_seconds(lua_State *L) {
    lua_pushinteger(
        L, static_cast<lua_Integer>(kf_pet_session_state()->neglect_seconds));
    return 1;
}

int lua_pet_save(lua_State *L) {
    (void)L;
    kf_pet_session_save();
    return 0;
}

/* pet.stage() -- the life-cycle position (ADR 0021), as a lowercase string:
 * "egg", "baby", "child", "teen", or "adult". A string, not the raw
 * kf_pet_stage integer, deliberately: kf/pet.h's own header comment draws
 * the line that WHAT a stage means is not Core's business, and the demo
 * creature script (kf_lua_demo_creature_script.h) already reacts to its
 * needs via band-crossing string-style thresholds, not raw numbers -- this
 * matches that existing convention rather than leaking Core's enum layout
 * into the cartridge layer. */
int lua_pet_stage(lua_State *L) {
    const char *name = "egg";
    switch (kf_pet_session_state()->stage) {
    case KF_PET_STAGE_EGG:
        name = "egg";
        break;
    case KF_PET_STAGE_BABY:
        name = "baby";
        break;
    case KF_PET_STAGE_CHILD:
        name = "child";
        break;
    case KF_PET_STAGE_TEEN:
        name = "teen";
        break;
    case KF_PET_STAGE_ADULT:
        name = "adult";
        break;
    }
    lua_pushstring(L, name);
    return 1;
}

/* pet.stage_seconds() -- how long the pet has been in ITS CURRENT stage,
 * added for the Info screen (Task 2, docs/superpowers/plans/2026-08-13-
 * screens-clock-sleep.md): stage_elapsed_seconds is the one field Info's
 * old LVGL implementation read that this binding had not exposed yet.
 * uint64_t straight to lua_Integer, no clamping, same as neglect_seconds()
 * above -- a Tamagotchi-scale lifetime never comes close to overflowing
 * either lua_Integer width this project builds with (64-bit on desktop,
 * 32-bit under LUA_32BITS on the ESP32 build; see kf_lua_scene.cpp's own
 * comment on that split). */
int lua_pet_stage_seconds(lua_State *L) {
    lua_pushinteger(L, static_cast<lua_Integer>(
                            kf_pet_session_state()->stage_elapsed_seconds));
    return 1;
}

/* pet.teen_form() / pet.adult_branch() -- WHICH branch was taken, as plain
 * 0-based indices (0..KF_PET_TEEN_FORM_COUNT-1, 0..kf_pet_adults_in_family
 * (teen_form)-1 -- the adult count is per-family, not one shared constant,
 * see kf/pet.h). Raw integers, not names, on purpose: unlike the stage itself,
 * naming these branches is real creative content that does not exist yet
 * (Chris: "simplistic blobs for now to get the systems working... I will
 * work with my friend who is a designer on creating actual characters,
 * along with names/backstories"). A future cartridge script maps these
 * indices to real names/behaviour itself once that content exists; Core
 * and this binding only ever hand over which slot was picked. Meaningless
 * (always 0) before the branch point that sets it -- see kf/pet.h's
 * kf_pet_state comment -- so a script must check pet.stage() first if it
 * needs to know whether the value is meaningful yet. */
int lua_pet_teen_form(lua_State *L) {
    lua_pushinteger(
        L, static_cast<lua_Integer>(kf_pet_session_state()->teen_form));
    return 1;
}

int lua_pet_adult_branch(lua_State *L) {
    lua_pushinteger(
        L, static_cast<lua_Integer>(kf_pet_session_state()->adult_branch));
    return 1;
}

/* pet.base_trait() / pet.dominant_care_trait() -- ADR 0023's personality
 * traits, the same opaque-0-based-index convention as teen_form/
 * adult_branch above and for the identical reason: the actual trait
 * names (Chatty/Quiet/Curious/... for base traits, Foodie/Playful/Chill
 * for the care-derived ones -- see 16-personality-traits-concrete-plan.md,
 * still placeholders as of this slice) are real creative content that
 * belongs in the Lua cartridge layer, not here. base_trait() is
 * meaningful from the moment a pet exists (rolled once at kf_pet_init(),
 * see kf/pet.h); dominant_care_trait() defaults to 0 (hunger-leaning)
 * before any care has actually accumulated, e.g. still an egg -- see
 * kf_pet_dominant_care_trait()'s own header comment in kf/pet.h for the
 * tie-break rule. */
int lua_pet_base_trait(lua_State *L) {
    lua_pushinteger(
        L, static_cast<lua_Integer>(kf_pet_session_state()->base_trait));
    return 1;
}

int lua_pet_dominant_care_trait(lua_State *L) {
    lua_pushinteger(L, static_cast<lua_Integer>(
                            kf_pet_dominant_care_trait(kf_pet_session_state())));
    return 1;
}

/* The reaction to the last care action, and what it was a reaction to.
 * Integers rather than strings, unlike pet.stage(): a script showing a
 * reaction is picking a sprite or an animation, not printing a word, and
 * the cartridge layer owns what "liked" looks like for its creature. */
int lua_pet_last_reaction(lua_State *L) {
    lua_pushinteger(
        L, static_cast<lua_Integer>(kf_pet_session_state()->last_reaction));
    return 1;
}

int lua_pet_last_care_action(lua_State *L) {
    lua_pushinteger(L, static_cast<lua_Integer>(
                            kf_pet_session_state()->last_care_action));
    return 1;
}

/* pet.reaction_to(trait, action, variation) -- the table itself, queryable
 * without performing the action. This is what lets a cartridge build a
 * "what does this one like?" screen, or a test creature explain itself,
 * without the player having to try everything on a live creature first.
 *
 * kf_pet_reaction_to() already clamps out-of-range input to neutral, so a
 * script passing rubbish gets a dull answer rather than an error -- the
 * right shape for a cartridge API. luaL_checkinteger still rejects a
 * non-number outright, which is a script TYPE error rather than a value
 * error and should surface, unlike the value being out of range. */
int lua_pet_reaction_to(lua_State *L) {
    const lua_Integer trait = luaL_checkinteger(L, 1);
    const lua_Integer action = luaL_checkinteger(L, 2);
    const lua_Integer variation = luaL_checkinteger(L, 3);
    lua_pushinteger(L, static_cast<lua_Integer>(kf_pet_reaction_to(
                            static_cast<uint8_t>(trait),
                            static_cast<kf_pet_care_action>(action),
                            static_cast<uint8_t>(variation))));
    return 1;
}

const luaL_Reg kKfPetFuncs[] = {
    {"hunger", lua_pet_hunger},
    {"happiness", lua_pet_happiness},
    {"energy", lua_pet_energy},
    {"feed", lua_pet_feed},
    {"play", lua_pet_play},
    {"rest", lua_pet_rest},
    {"bath", lua_pet_bath},
    {"flush", lua_pet_flush},
    {"poops", lua_pet_poops},
    {"dirtiness", lua_pet_dirtiness},
    {"sick", lua_pet_sick},
    {"dead", lua_pet_dead},
    {"neglect_seconds", lua_pet_neglect_seconds},
    {"save", lua_pet_save},
    {"stage", lua_pet_stage},
    {"stage_seconds", lua_pet_stage_seconds},
    {"teen_form", lua_pet_teen_form},
    {"adult_branch", lua_pet_adult_branch},
    {"base_trait", lua_pet_base_trait},
    {"dominant_care_trait", lua_pet_dominant_care_trait},
    {"last_reaction", lua_pet_last_reaction},
    {"last_care_action", lua_pet_last_care_action},
    {"reaction_to", lua_pet_reaction_to},
    {nullptr, nullptr},
};

void register_pet_bindings(lua_State *L) {
    luaL_newlib(L, kKfPetFuncs);
    lua_setglobal(L, "pet");
}

/* creature.* -- Task 5 of the Lua game-layer plan. Read-only, plain getters
 * (no jQuery no-arg-read/arg-write pair: there is nothing to WRITE here --
 * the wander still lives in C++, shared through kf_creature_presenter.h;
 * moving it into the script itself, along with pose/sprite-name selection,
 * is real remaining work this task's own review deliberately did NOT take
 * on alongside the KF_HOME_SCREEN default flip and the repaint capability
 * -- see docs/architecture/adr-0043-lua-home-default.md for why bit-exact
 * fixed-point/RNG parity made that too large and too risky to fold into
 * the same change). Every one of these reads whatever kf_creature_
 * presenter_advance() most recently resolved for the CURRENT frame --
 * NOT called from kf_lua_port_frame() below (this file is generic Lua
 * glue shared by every script this codebase loads, including proof
 * scripts with no pet session at all), but from kf_lua_home_screen.cpp's
 * kf_lua_home_screen_frame(), the ONE caller that actually knows a pet
 * session exists, before it runs this script's on_frame -- the same
 * "advance once, then declare" shape kf_creature_screen_frame() (the C++
 * screen) already has, so both screens see identical numbers on a frame
 * they are both asked to render (the parity check's whole premise). See
 * ADR 0042's "Found by the check" section for why the call lives there
 * and not here. Meaningless before the first frame has ever advanced; a
 * script calling these from its own top-level code (before on_frame ever
 * runs) sees whatever kf_creature_presenter_reset() leaves behind (x=y=0,
 * an empty sprite name) -- exactly why the minimal-pet style is "read
 * these inside on_frame", not at load time. */
int lua_creature_x(lua_State *L) {
    lua_pushinteger(L, kf_creature_presenter_x());
    return 1;
}

int lua_creature_y(lua_State *L) {
    lua_pushinteger(L, kf_creature_presenter_y());
    return 1;
}

int lua_creature_sprite(lua_State *L) {
    lua_pushstring(L, kf_creature_presenter_sprite_name());
    return 1;
}

int lua_creature_mirrored(lua_State *L) {
    lua_pushboolean(L, kf_creature_presenter_mirrored() ? 1 : 0);
    return 1;
}

int lua_creature_frame(lua_State *L) {
    lua_pushinteger(L, kf_creature_presenter_anim_frame());
    return 1;
}

const luaL_Reg kKfCreatureFuncs[] = {
    {"x", lua_creature_x},
    {"y", lua_creature_y},
    {"sprite", lua_creature_sprite},
    {"mirrored", lua_creature_mirrored},
    {"frame", lua_creature_frame},
    {nullptr, nullptr},
};

void register_creature_bindings(lua_State *L) {
    luaL_newlib(L, kKfCreatureFuncs);
    lua_setglobal(L, "creature");
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
    register_pet_bindings(g.L);
    register_creature_bindings(g.L);
    /* Task 3 of the Lua game layer plan: the drawing surface over kf/
     * scene.h. Runs after register_bindings() on purpose -- it adds to the
     * `kf` table that call just created, rather than creating its own. */
    kf_lua_scene_register(g.L);

    g.disabled_after_error = false;
    g.last_error[0] = '\0';
    g.last_call_us = 0;
    g.last_info_call_us = 0;
    g.last_report = 0;
    g.frame_count = 0;
    /* g.home_screen_active is deliberately NOT touched here -- see its own
     * member-initializer comment above for why resetting it on every init
     * would be wrong. */

    if (luaL_loadbuffer(g.L, script_source, std::strlen(script_source),
                         chunk_name) != LUA_OK) {
        KF_LOGE(TAG, "script failed to load: %s", lua_tostring(g.L, -1));
        lua_close(g.L);
        g.L = nullptr;
        kf_lua_alloc_shutdown();
        return false;
    }

    if (lua_pcall(g.L, 0, 0, 0) != LUA_OK) {
        KF_LOGE(TAG, "script's top-level code raised an error: %s",
                lua_tostring(g.L, -1));
        lua_close(g.L);
        g.L = nullptr;
        kf_lua_alloc_shutdown();
        return false;
    }

    /* A latent gap in ADR 0044's original design, closed here because Task
     * 2 (ADR 0045) is the first thing to actually trip it: a script's
     * top-level kf.screen() calls just above CREATE every group's scene
     * objects visible-by-default (kf/scene.h's own "a new object starts
     * visible" rule), and nothing has ever hidden any of them yet -- the
     * only thing that hides an inactive group is kf_lua_scene_activate_
     * screen(), which normally runs from inside kf_screen_nav_show(), which
     * nothing has called yet either, this early. A script with exactly one
     * kf.screen() group (Home, before this task) never showed the gap: one
     * group, always the "active" one by default. A script with two or
     * more -- creature.lua's kf.screen("info") added this task -- would
     * otherwise sit fully visible on top of screen 0 from the very first
     * frame, until whatever first MENU/B press or screen:show() call
     * happened to hide it. kf_lua_scene_hide_other_screens(), not the
     * full kf_lua_scene_activate_screen(): this function has no guarantee
     * a framebuffer exists yet (several headless checks exercise
     * pet.* / on_frame logic with none at all), so it sets only the
     * visibility bookkeeping, not a repaint -- see that function's own
     * header comment for why that split is safe. Index 0, by the "Home
     * registers first" convention every caller of kf_screen_nav_register()
     * already follows -- a no-op for a script that never calls
     * kf.screen() at all, since there is nothing to hide. */
    kf_lua_scene_hide_other_screens(0);

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

    /* kf.on_button handlers fire before on_frame, each in their own
     * lua_pcall (kf_lua_scene.cpp), so a button press this frame is
     * already reflected in whatever pet.* state on_frame goes on to read
     * -- not one frame behind it. */
    kf_lua_scene_dispatch_buttons(g.L);

    lua_getglobal(g.L, "on_frame");
    if (!lua_isfunction(g.L, -1)) {
        lua_pop(g.L, 1);
        return; /* a script is not required to define on_frame */
    }
    lua_pushinteger(g.L, static_cast<lua_Integer>(dt_ms));
    if (lua_pcall(g.L, 1, 0, 0) != LUA_OK) {
        const char *msg = lua_tostring(g.L, -1);
        KF_LOGE(TAG,
                "on_frame raised an error, disabling further calls until "
                "the next kf_lua_port_init(): %s",
                msg);
        /* Task 5's error banner needs the same words the log just got --
         * copied here, not re-derived from the (already popped) Lua stack
         * later, since a screen implementation may not read this until
         * several frames after the error. */
        std::snprintf(g.last_error, sizeof(g.last_error), "%s",
                      msg != nullptr ? msg : "(no message)");
        lua_pop(g.L, 1);
        g.disabled_after_error = true;
        return;
    }
    g.frame_count++;
}

void kf_lua_port_info_frame(uint32_t synthetic_frame_delta_ms) {
    if (!g.ready || g.disabled_after_error) {
        return;
    }

    /* No kf_lua_scene_dispatch_buttons() call here, unlike kf_lua_port_
     * frame() above -- see this function's own header comment in kf_lua_
     * port.h for why Info's screen has no buttons to dispatch. `dt_ms` is
     * a real elapsed-time-tracking dance too, but its own INDEPENDENT one:
     * sharing g.last_call_us with kf_lua_port_frame() above would corrupt
     * BOTH functions' "time since I was last called" math the moment a
     * script's active screen alternates between calling one and the
     * other, which is exactly what happens every time the player presses
     * MENU. */
    uint32_t dt_ms = synthetic_frame_delta_ms;
    if (dt_ms == 0u) {
        const uint64_t now_us = kf_time_mono_us();
        dt_ms = g.last_info_call_us == 0u
                    ? 0u
                    : static_cast<uint32_t>(
                          (now_us >= g.last_info_call_us
                               ? now_us - g.last_info_call_us
                               : 0u) /
                          1000u);
        g.last_info_call_us = now_us;
    }

    lua_getglobal(g.L, "on_info_frame");
    if (!lua_isfunction(g.L, -1)) {
        lua_pop(g.L, 1);
        return; /* a script is not required to define on_info_frame */
    }
    lua_pushinteger(g.L, static_cast<lua_Integer>(dt_ms));
    if (lua_pcall(g.L, 1, 0, 0) != LUA_OK) {
        const char *msg = lua_tostring(g.L, -1);
        KF_LOGE(TAG,
                "on_info_frame raised an error, disabling further calls "
                "until the next kf_lua_port_init(): %s",
                msg);
        std::snprintf(g.last_error, sizeof(g.last_error), "%s",
                      msg != nullptr ? msg : "(no message)");
        lua_pop(g.L, 1);
        g.disabled_after_error = true;
        return;
    }
}

void kf_lua_port_shutdown() {
    if (!g.ready) {
        return;
    }
    KF_LOGI(TAG, "shutting down after %u frame(s)",
            static_cast<unsigned>(g.frame_count));
    lua_close(g.L);
    g.L = nullptr;
    g.ready = false;
    kf_lua_alloc_shutdown();
}

int64_t kf_lua_port_last_report() { return g.last_report; }
uint32_t kf_lua_port_frame_count() { return g.frame_count; }

void kf_lua_port_set_home_screen_active(bool active) {
    g.home_screen_active = active;
}
bool kf_lua_port_home_screen_active(void) { return g.home_screen_active; }

bool kf_lua_port_disabled_after_error(void) { return g.disabled_after_error; }
const char *kf_lua_port_last_error(void) { return g.last_error; }
