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
 *
 * Also reacts to life-stage transitions (ADR 0021), the same "log only on
 * the frame it actually changes" pattern as the need bands above, via
 * pet.stage()/pet.teen_form()/pet.adult_branch(). Deliberately generic
 * flavour text, not a real creature's actual voice or character name --
 * this file is placeholder demo content (see this header's own first
 * paragraph), same status as every need-band message above it; Chris's
 * real characters are a separate, later effort with his designer, and
 * kf/pet.h's own header comment already draws the line that teen_form/
 * adult_branch are opaque indices, not names, so that is genuinely all
 * this script (or any script, today) has to say about which branch a pet
 * took -- it logs the raw index, it does not invent a name for it.
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
