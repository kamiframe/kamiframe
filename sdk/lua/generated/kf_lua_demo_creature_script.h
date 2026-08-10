/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * GENERATED FILE -- do not edit by hand. Regenerate with:
 *     python3 tools/kf_embed_lua.py
 * from examples/creature_demo/creature.lua. lua_embed_check
 * (simulator/CMakeLists.txt) regenerates every embedded header into a
 * temp directory and diffs it against what is checked in here on every
 * `ctest` run, so a hand edit -- or a stale header after the .lua source
 * changed -- fails the build instead of silently drifting.
 */

#ifndef KF_LUA_DEMO_CREATURE_SCRIPT_H
#define KF_LUA_DEMO_CREATURE_SCRIPT_H

inline constexpr const char *kKfLuaDemoCreatureScriptSource = R"lua(
-- SPDX-License-Identifier: Apache-2.0
-- Copyright the Kamiframe contributors.
--
-- This CODE is Apache-2.0; the creature's art is not -- see ART-LICENSE.md
-- (this directory) and LICENSING.md. Placeholder flavour text throughout,
-- not a real creature's voice; teen_form/adult_branch below are logged as
-- raw indices, never invented names (kf/pet.h draws that line; full
-- rationale: docs/architecture/adr-0018-demo-creature-script.md). Kept
-- short: embedded verbatim into flash by tools/kf_embed_lua.py and parsed
-- by the Lua VM at every boot.

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

-- Previous life-stage (ADR 0021), same "nil means first observation, stay
-- quiet" convention as `bands` above -- a pet loaded already past the egg
-- stage should not announce "hatched!" on frame one just because this
-- script only just started watching it.
local previous_stage = nil

local kStageMessage = {
    baby = "the egg cracks open -- hello, little one!",
    child = "growing bigger every day!",
    teen = "quite the growth spurt lately...",
    adult = "all grown up now.",
}

local function announce_stage()
    local current = pet.stage()
    local previous = previous_stage
    previous_stage = current
    if previous == nil or previous == current then
        return
    end
    local message = kStageMessage[current]
    if message then
        kf.log(message)
    end
    -- The two branch points: which teen form care during Child picked,
    -- and which adult form care during Teen picked. Raw indices only --
    -- see this file's header comment on why nothing here invents a name.
    if current == "teen" then
        kf.log("care during childhood settled into teen form " ..
               pet.teen_form())
    elseif current == "adult" then
        kf.log("care during the teen years settled into adult form " ..
               pet.teen_form() .. "-" .. pet.adult_branch())
    end
end

function on_frame(dt_ms)
    announce_stage()
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
