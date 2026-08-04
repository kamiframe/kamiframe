/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * The demo creature -- see README.md's "What's in the box" table ("Write
 * creatures in Lua against a small, documented API") and its Licensing
 * section ("The demo creature's code is Apache 2.0"). Not a proof script:
 * every earlier Lua script in this codebase (kf_lua_proof_script.h,
 * kf_lua_pet_proof_script.h) exists to prove a MECHANISM works and says so
 * in its own header comment. This one exists to be looked at. It is the
 * first Lua script in this repository that expresses actual creature
 * behaviour -- reacting to the pet's own state -- entirely in Lua, against
 * the pet.* and kf.* surface ADR 0016 built and nothing else, which is the
 * whole promise of "no C required" made real for the first time.
 *
 * There is no sprite, no audio, and no second screen yet (see ADR 0017's
 * "Later" section), so the only channel this script has to say anything is
 * kf.log() -- every message below shows up in the console, not on the
 * pet screen. That is a real, current limitation, not a design choice this
 * script is making on purpose; see docs/architecture/adr-0018-demo-
 * creature-script.md for what a richer expression channel would need.
 *
 * Behaviour: classify each of the three needs into one of four bands
 * (critical / low / ok / full) every frame, and log a short, in-character
 * line only on the frame a need actually CROSSES into a new band -- not
 * every frame it happens to be in one, which would be constant noise at
 * 30fps. The "ok" band is deliberately silent in both directions: it is
 * the pet's normal resting state, and a message every time a need drifts
 * a little either side of the low/full boundaries would drown out the
 * bands that are actually meant to prompt a person to do something.
 */

#ifndef KF_LUA_DEMO_CREATURE_SCRIPT_H
#define KF_LUA_DEMO_CREATURE_SCRIPT_H

inline constexpr const char *kKfLuaDemoCreatureScriptSource = R"lua(
-- kf_pet_millipercent is 0..100000 (kf/pet.h) -- these thresholds are
-- fractions of that same range, not raw percent, so they read directly
-- against pet.hunger()/happiness()/energy()'s own return values.
local function classify(mp)
    if mp < 10000 then
        return "critical"
    elseif mp < 30000 then
        return "low"
    elseif mp < 70000 then
        return "ok"
    else
        return "full"
    end
end

-- Previous band per need, so a message fires only on the frame a need
-- actually crosses a boundary. Starts empty: the very first observation
-- of each need records its band silently, with nothing to compare against
-- yet -- a fresh pet starting at max should not immediately announce
-- "topped up" on frame one.
local bands = {}

local function announce(name, mp, messages)
    local current = classify(mp)
    local previous = bands[name]
    bands[name] = current
    if previous == nil or previous == current then
        return
    end
    local message = messages[current]
    if message then
        kf.log(message)
    end
end

function on_frame(dt_ms)
    announce("hunger", pet.hunger(), {
        critical = "hunger is critical -- feed me!",
        low = "starting to get hungry...",
        full = "all fed up, thanks!",
    })
    announce("happiness", pet.happiness(), {
        critical = "feeling really down...",
        low = "could use some playtime",
        full = "having a great time!",
    })
    announce("energy", pet.energy(), {
        critical = "exhausted -- I need rest",
        low = "getting a little tired",
        full = "fully rested!",
    })
end

kf.log("the creature stirs")
)lua";

/* Leading '=' is Lua's own convention for "show this name verbatim in
 * error messages" -- see kf_lua_proof_script.h's identical comment on its
 * own chunk name for why, unchanged here. */
inline constexpr const char *kKfLuaDemoCreatureScriptChunkName =
    "=demo_creature";

#endif /* KF_LUA_DEMO_CREATURE_SCRIPT_H */
