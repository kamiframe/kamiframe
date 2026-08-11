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
-- Also declares the whole home screen now (Task 5, docs/superpowers/plans/
-- 2026-08-12-lua-game-layer.md; grouped via kf.screen() since ADR 0044),
-- gated by kf.home_screen_active() so this one file runs under either
-- KF_HOME_SCREEN build. Layout numbers match kf_creature_screen.cpp's own
-- constants -- see that file for the why.
--
-- The info screen (Task 2 of docs/superpowers/plans/2026-08-13-screens-
-- clock-sleep.md, ADR 0045) is declared below unconditionally -- unlike
-- Home, Info does not care which build owns the creature's own screen, so
-- it is not gated behind kf.home_screen_active().

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

-- Forward-declared for on_frame() below; only created under KF_HOME_
-- SCREEN=lua (kf.home_screen_active()).
local body, shrine, poop, fill

if kf.home_screen_active() then
    -- ADR 0044: Home's objects are declared through a named screen group
    -- rather than bare kf.* calls -- a receiver change only, same layout,
    -- same object count, same declaration order.
    local home = kf.screen("home")
    local bg = kf.color(232, 240, 216)
    home:background(bg)

    -- on_frame() sets the real sprite/position every frame, including its
    -- first -- this placeholder is never painted.
    body = home:sprite("")
    body:layer(1) -- over the mess

    shrine = home:sprite("shrine_idle_s")
    shrine:move(96, 106) -- centred, 48x48

    poop = {} -- 8 fixed slots, field 240px / 8
    for i = 1, 8 do
        poop[i] = home:box(12, 12, kf.color(92, 64, 51))
        poop[i]:move((i - 1) * 30 + 9, 232)
    end

    local names = {"HUNGER", "HAPPY", "ENERGY"}
    local colors = {
        kf.color(214, 118, 40), kf.color(224, 196, 32), kf.color(60, 140, 210),
    }
    local track_color = kf.color(190, 190, 190)
    fill = {}
    for i = 1, 3 do
        local y = 262 + (i - 1) * 9
        local track = home:box(190, 8, track_color)
        track:move(42, y)
        fill[i] = home:box(0, 8, colors[i])
        fill[i]:move(42, y)
        fill[i]:layer(1) -- over the track
        local label = home:text(names[i])
        label:move(2, y)
        label:color(kf.BLACK, bg)
    end

    local guide = {"1:FEED", "2:PLAY", "3:REST", "4:BATH", "5:FLUSH"}
    for i = 1, 5 do -- centred per 48px slot, never touched again
        local slot_x = (i - 1) * 48
        local w = #guide[i] * 6 -- KF_FONT_CELL_W
        local label = home:text(guide[i])
        label:move(slot_x + (48 - w) // 2, 300)
        label:color(kf.BLACK, bg)
    end
end

-- ADR 0045: Info, the read-only stage/personality readout -- eight text
-- objects, replacing kf_pet_info_screen.cpp's LVGL widget tree. Positions
-- match that screen's own layout. info_stage/info_time/info_branch/
-- info_trait are updated every frame below; the other four are captions,
-- set once here and never touched again.
local info = kf.screen("info")
local info_bg = kf.color(20, 24, 32)
info:background(info_bg)

local function info_label(str, x, y)
    local t = info:text(str)
    t:move(x, y)
    t:color(kf.WHITE, info_bg)
    return t
end

do
    local title = "INFO"
    info_label(title, (kf.width() - #title * 6) // 2, 4) -- 6 = KF_FONT_CELL_W
end
info_label("STAGE", 12, 40)
info_label("TIME IN STAGE", 12, 96)
info_label("PERSONALITY", 12, 188)
local info_stage = info_label("", 12, 60)
local info_time = info_label("", 12, 116)
-- Blank until TEEN picks a form -- kf_pet_info_screen.cpp:118-131 (deleted
-- this task) explains why; the reasoning is copied into ADR 0045, not
-- here, since script comments cost flash.
local info_branch = info_label("", 12, 152)
local info_trait = info_label("", 12, 208)

-- "2D 4H" / "3H 12M" / "5M 09S" / "42S" -- the largest two units that keep
-- the value readable. Moved from kf_pet_info_screen.cpp's set_duration_
-- label(), uppercased: the bitmap font has no lowercase and LVGL's did.
local function format_duration(s)
    if s >= 86400 then
        return (s // 86400) .. "D " .. ((s % 86400) // 3600) .. "H"
    elseif s >= 3600 then
        return (s // 3600) .. "H " .. ((s % 3600) // 60) .. "M"
    elseif s >= 60 then
        return string.format("%dM %02dS", s // 60, s % 60)
    else
        return s .. "S"
    end
end

-- Global, not local -- kf_lua_port_info_frame() (sdk/lua/kf_lua_port.cpp)
-- calls this by name while Info is the active screen, a SEPARATE entry
-- point from on_frame() below on purpose: on_frame()'s own Home block
-- mutates Home's scene objects unconditionally, which would un-hide them
-- on top of Info if it ran while Info is showing -- see kf_lua_port.h's
-- own header comment on kf_lua_port_info_frame() for the full reasoning.
function on_info_frame(dt_ms)
    local stage = pet.stage()
    info_stage:set(stage) -- kf.text auto-uppercases, e.g. "egg" -> "EGG"
    info_time:set(format_duration(pet.stage_seconds()))

    if stage == "teen" then
        info_branch:set("TEEN FORM " .. pet.teen_form())
    elseif stage == "adult" then
        info_branch:set("ADULT FORM " .. pet.teen_form() .. "-" ..
                         pet.adult_branch())
    else
        info_branch:set("")
    end

    -- base_trait is meaningful from the moment a pet exists; care_trait
    -- reads 0/hunger-leaning by default before any care has accumulated
    -- (still an egg) -- see ADR 0045 for the full reasoning.
    if stage == "egg" then
        info_trait:set("BASE TRAIT " .. pet.base_trait())
    else
        info_trait:set("BASE TRAIT " .. pet.base_trait() ..
                        ", CARE TRAIT " .. pet.dominant_care_trait())
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
