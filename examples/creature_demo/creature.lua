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
--
-- Task 5 (docs/superpowers/plans/2026-08-12-lua-game-layer.md): this now
-- also declares the whole home screen -- background, creature, mess, stat
-- bars, care guide -- guarded by kf.home_screen_active(), so the SAME file
-- runs unchanged under either KF_HOME_SCREEN build: narration only under
-- "cpp" (the C++ screen owns drawing), narration AND drawing under "lua".
-- Layout numbers below are chosen to match simulator/src/pet/kf_creature_
-- screen.cpp's own constants exactly -- see that file for what each one is
-- FOR; the reasoning is not repeated here to keep this file cheap to boot.

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

-- The whole home screen -- forward-declared here so on_frame() below can
-- reach them as upvalues; only actually created (and only ever moved)
-- when this build's Lua owns Home. See kf.home_screen_active()'s own
-- comment (sdk/lua/kf_lua_port.cpp) for why one script file needs this
-- guard at all.
local body, shrine, poop, fill

if kf.home_screen_active() then
    local bg = kf.color(232, 240, 216)
    kf.background(bg)

    -- Placeholder name/position: on_frame() below sets the real ones every
    -- frame, including its very first call, before this scene ever commits
    -- -- see kf_creature_screen.cpp's own enter()/declare_creature() for
    -- why that ordering makes the placeholder harmless.
    body = kf.sprite("")
    body:layer(1) -- paints over the mess

    shrine = kf.sprite("shrine_idle_s")
    shrine:move(96, 106) -- centred in the 240x260 field, 48x48 sprite

    -- Mess: 8 fixed slots, field is 240px wide / 8 -- see kf_creature_
    -- screen.cpp's own poop_rect() for the identical arithmetic.
    poop = {}
    for i = 1, 8 do
        poop[i] = kf.box(12, 12, kf.color(92, 64, 51))
        poop[i]:move((i - 1) * 30 + 9, 232)
    end

    -- Stats band: a track + a fill box per need, label to its left.
    local names = {"HUNGER", "HAPPY", "ENERGY"}
    local colors = {
        kf.color(214, 118, 40), kf.color(224, 196, 32), kf.color(60, 140, 210),
    }
    local track_color = kf.color(190, 190, 190)
    fill = {}
    for i = 1, 3 do
        local y = 262 + (i - 1) * 9
        local track = kf.box(190, 8, track_color)
        track:move(42, y)
        fill[i] = kf.box(0, 8, colors[i])
        fill[i]:move(42, y)
        fill[i]:layer(1) -- paints over the track
        local label = kf.text(names[i])
        label:move(2, y)
        label:color(kf.BLACK, bg)
    end

    -- Care guide: five static labels, centred in their own slot, never
    -- touched again after this.
    local guide = {"1:FEED", "2:PLAY", "3:REST", "4:BATH", "5:FLUSH"}
    for i = 1, 5 do
        local slot_x = (i - 1) * 48
        local w = #guide[i] * 6 -- KF_FONT_CELL_W, fixed-width font
        local label = kf.text(guide[i])
        label:move(slot_x + (48 - w) // 2, 300)
        label:color(kf.BLACK, bg)
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

    if kf.home_screen_active() then
        fill[1]:size(pet.hunger() * 190 // 100000, 8)
        fill[2]:size(pet.happiness() * 190 // 100000, 8)
        fill[3]:size(pet.energy() * 190 // 100000, 8)

        if pet.dead() then
            shrine:show()
            body:hide()
        else
            shrine:hide()
            body:show()
            local poops = pet.poops()
            for i = 1, 8 do
                if i <= poops then poop[i]:show() else poop[i]:hide() end
            end
            body:sprite(creature.sprite())
            body:flip(creature.mirrored())
            body:frame(creature.frame())
            body:move(creature.x(), creature.y())
        end
    end
end

kf.log("the creature stirs")
