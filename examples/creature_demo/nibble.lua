-- SPDX-License-Identifier: Apache-2.0
-- Copyright the Kamiframe contributors.
--
-- Nibble: the first minigame, and the plumbing every later game inherits
-- (see the Nibble-and-the-game-session design and the plan of the same
-- name). Eight rounds; a food box drifts toward the pet's mouth; press A
-- when it arrives. Chosen first because it needs no sensors and no
-- fixed-point maths, so the session/tier/reward pipeline gets built and
-- debugged with nothing else in the way -- the game itself is deliberately
-- small.
--
-- LOADED AS A SEPARATE CHUNK via kf_lua_port_load(), after creature.lua's
-- own kf_lua_port_init() has already run -- NOT a module, NOT required():
-- this file shares creature.lua's global environment and its `kf`/`pet`/
-- `game`/`creature` bindings, but is its own file on disk because the
-- cartridge boundary this slice needs is "a game is its own file" (the
-- design's section 4.2). If you are looking for the picker that opens
-- this screen, that is creature.lua's own Home-only block (2:PLAY) --
-- see that file's own comment on why the picker lives there instead of
-- here.
--
-- WHY THIS FILE CANNOT USE on_frame() THE WAY creature.lua's OWN Home/
-- Info/Settings blocks do: on_home_frame()/on_info_frame()/on_settings_
-- frame() are ALREADY CLAIMED, one each, by creature.lua, and Lua globals
-- are shared across every chunk in one lua_State -- if this file declared
-- its own on_home_frame it would silently REPLACE creature.lua's. Instead
-- this file CHAINS onto the one shared on_frame() (kf_lua_port_frame()'s
-- own entry point, called unconditionally every real frame regardless of
-- which screen is active -- see kf_lua_port.h's own comment): it reads
-- whatever on_frame creature.lua already defined, keeps calling it, and
-- adds its own logic afterward, GATED on kf.active_screen() == "nibble"
-- so none of it runs, and no kf_rng draws or state changes happen, while
-- any other screen is the one actually showing -- the exact bug class
-- ADR 0042/0043 already found and fixed for Home's own creature/poop/
-- shrine objects, applied here to a second script instead of a second
-- copy-paste inside the first one.

----------------------------------------------------------------------
-- The manifest. Chris approved these as a starting point on 2026-08-16,
-- to be retuned after play -- see the plan's Task 4. `needs`/`wants` are
-- empty (Nibble uses no sensors and runs on any board) but the fields
-- exist from day one: they are the answer to the minimum-hardware-profile
-- question, and game six onward cannot run without them.
----------------------------------------------------------------------
local manifest = {
    id = "nibble",
    needs = {},
    wants = {},
    reward = {
        energy_cost_mp = 6000,
        need = "hunger",
        need_fraction_percent = 40,
        good = 40,
        great = 75,
    },
}
local ROUNDS = 8

----------------------------------------------------------------------
-- Tunables. Named constants, not magic numbers scattered through the
-- round logic below, so tuning the feel is a one-line edit -- the design's
-- own requirement for the handicap in particular ("tunable in Lua without
-- a rebuild").
----------------------------------------------------------------------

-- How long, at zero handicap, the food takes to drift from its start
-- position to the strike point. Comfortably reactable at a glance --
-- this is a timing game, not a reflex-test one.
local BASE_LEAD_TIME_MS = 1400
-- The window either side of the exact arrival moment that counts as a
-- hit at all (GOOD) or the tighter one inside it that counts as PERFECT.
-- Pressing outside the good window, or never pressing before the round
-- times out, is a MISS.
local BASE_GOOD_WINDOW_MS = 220
local BASE_PERFECT_WINDOW_MS = 90
-- The handicap (0..100, from game.begin()'s returned context) narrows
-- the strike window and speeds the food up, by up to these fractions at
-- handicap == 100. Chris's "visible handicap, never a lockout" decision:
-- a tired pet plays worse, it is never refused the game outright.
local HANDICAP_SPEED_FRACTION = 0.4  -- up to 40% faster lead time
local HANDICAP_WINDOW_FRACTION = 0.5 -- up to 50% narrower windows

-- The tell: how far ahead of the exact arrival moment the creature
-- leans in, and by how many pixels -- the ONLY visual cue once the
-- strike zone stops being drawn (round 4 on). Both constants, both
-- tunable without touching the round logic below.
local TELL_LEAD_MS = 260
local TELL_OFFSET_PX = 5

-- How many points a perfect/good hit is worth. All-perfect across 8
-- rounds is 120, comfortably past `great` (75); all-good is 64,
-- comfortably past `good` (40) but short of `great` -- reaching GREAT
-- needs real precision, not just consistent contact.
local POINTS_PERFECT = 15
local POINTS_GOOD = 8

-- A short pause after a round resolves (hit, or the timeout miss) before
-- the next one begins, so the player has a moment to see what just
-- happened rather than the food instantly resetting.
local ROUND_SETTLE_MS = 350

----------------------------------------------------------------------
-- Layout. 240x320 panel (kf.width()/kf.height()); the field sits in the
-- upper-middle band, clear of a status line at the top and the result/
-- hint text at the bottom.
----------------------------------------------------------------------
local FIELD_Y = 120
local FOOD_START_X = 8
local STRIKE_X = 150
local FOOD_SIZE = 14
local STRIKE_MARKER_SIZE = 22
-- Creature sprites are 48x48 (kf_creature_presenter.h's own kSpriteSize).
-- Placed with a real gap clear of the strike marker, not flush against
-- it: an earlier version put the creature's own left edge AT STRIKE_X,
-- which put its 48px body directly on top of the strike-zone marker --
-- the marker was visible in NAME only, hidden under the creature every
-- round it was supposed to show. CREATURE_BASE_X starts past the
-- marker's own right edge (STRIKE_X + half its width) plus CREATURE_
-- GAP_PX of clearance, comfortably wider than TELL_OFFSET_PX so even
-- mid-lean the creature never reaches back over the marker.
local CREATURE_GAP_PX = 10
local CREATURE_BASE_X = STRIKE_X + (STRIKE_MARKER_SIZE // 2) + CREATURE_GAP_PX
local CREATURE_Y = 96

----------------------------------------------------------------------
-- The <stage><branch>_<pose>_<facing> sprite name kf_creature_sprite_
-- name() (hakoniwaos/src/creature.cpp) builds in C++, reimplemented here
-- for the SAME reason creature.lua's own want_stage_token() already
-- reimplements it rather than calling into Core: what a stage/branch
-- number means is game-layer content, not Core's business (kf/pet.h's
-- own words), and this file cannot reach creature.lua's own LOCAL want_
-- stage_token() -- it is a different chunk (see this file's own header
-- comment on why). "s" (front-facing, idle) is the only facing Nibble
-- ever asks for: the creature does not walk here, it only leans a few
-- pixels toward the food, which is a POSITION offset, not a facing
-- change. A stage with no such sprite (adult has no art at all -- the
-- design's own section 2) resolves to kf/scene.h's placeholder box, the
-- same graceful "missing sprite" fallback every other absent name in
-- this codebase already gets -- the OFFSET tell still reads even when
-- the pose swap draws a placeholder both times.
local function nibble_sprite_name(pose)
    local stage = pet.stage()
    if stage == "egg" then
        return "egg_idle_s" -- the manifest gives egg exactly one state
    elseif stage == "teen" then
        return "teen" .. pet.teen_form() .. "_" .. pose .. "_s"
    elseif stage == "adult" then
        return "adult" .. pet.teen_form() .. pet.adult_branch() .. "_" ..
                   pose .. "_s"
    else
        return stage .. "_" .. pose .. "_s"
    end
end

----------------------------------------------------------------------
-- Scene: the field, the food, the strike-zone marker, and the two text
-- lines that tell the player what is happening.
--
-- THE THREE-OBJECT VERSION OF THIS FILE IS WHY ADR 0061 EXISTS. Before
-- it, every screen in the cartridge held its scene objects for the life
-- of the process whether or not anyone was looking at it, so
-- creature.lua's Home, Info, Settings and play picker sat at 61 of the
-- 64 slots between them and this file had exactly three to spend -- on a
-- creature, a piece of food and a target. Round, score and result had to
-- go out through kf.log() instead, which meant a scoring game that never
-- showed the player a number. ADR 0061 made the 64 a PER-SCREEN budget:
-- a screen that is not showing holds nothing, so Nibble now has the
-- whole scene to itself while it is the active screen and these two text
-- objects cost nothing that any other screen wanted.
----------------------------------------------------------------------
local SCREEN_BG = kf.color(18, 22, 26)

local screen = kf.screen("nibble")
screen:background(SCREEN_BG)

local creature_obj = screen:sprite(nibble_sprite_name("neutral"))
creature_obj:move(CREATURE_BASE_X, CREATURE_Y)

-- A coloured box, per Chris's "existing art plus placeholder coloured
-- boxes" decision -- no new sprites in this slice. Swapping in real food
-- art later is a one-line change (screen:sprite() instead of
-- screen:box()), nothing about the round logic below has to change.
local food = screen:box(FOOD_SIZE, FOOD_SIZE, kf.color(224, 160, 48))
food:move(FOOD_START_X, FIELD_Y)

-- Visible for rounds 1-3 only (round_started_visible below); a thin
-- outline colour so it reads as a TARGET, not a second piece of food.
local strike_marker =
    screen:box(STRIKE_MARKER_SIZE, STRIKE_MARKER_SIZE, kf.color(70, 90, 70))
strike_marker:move(STRIKE_X - (STRIKE_MARKER_SIZE - FOOD_SIZE) // 2, FIELD_Y -
                        (STRIKE_MARKER_SIZE - FOOD_SIZE) // 2)
strike_marker:layer(-1) -- behind the food, so the food is never hidden by it

-- The persistent line: which round this is and what the score is. Top-
-- left, clear of the field (FIELD_Y = 120) and of the creature.
local status_line = screen:text("")
status_line:move(8, 8)
status_line:color(kf.WHITE, SCREEN_BG)

-- The transient line: what just happened, or what to press. Bottom of
-- the panel, well below the field, so a result never lands on top of the
-- thing the player is watching.
local result_line = screen:text("")
result_line:move(8, 292)
result_line:color(kf.WHITE, SCREEN_BG)

-- Both lines stay under KF_SCENE_TEXT_MAX (kf/scene.h) -- 40 characters,
-- silently truncated past that, which is exactly the failure the play
-- picker's own label hit and had to be shortened for. The longest string
-- either of these is ever handed is finish_session()'s tier line, and
-- that is why the tier and the record are on separate lines rather than
-- one: together they ran past 40.
local function set_status(text)
    status_line:set(text)
end

-- Still logs, exactly as it did when the log WAS the status line: the
-- headless checks and anything watching a device over the debug bridge
-- read these, and an on-screen line the player can see is an addition to
-- that, not a replacement for it.
local function report_status(text)
    result_line:set(text)
    kf.log("nibble: " .. text)
end

----------------------------------------------------------------------
-- Round state. `active` mirrors "did game.begin() succeed" -- a dead or
-- asleep pet (kf_game_session_begin()'s own two refusal cases) means
-- Nibble is entered but there is no session to play; the field still
-- draws, nothing scores, and report_status() explains why.
----------------------------------------------------------------------
local was_showing = false
local active = false
local handicap = 0
local round = 0
local round_elapsed_ms = 0
local round_resolved = false
local total_score = 0
local finished = false

local function lead_time_ms()
    return BASE_LEAD_TIME_MS -
               (BASE_LEAD_TIME_MS * HANDICAP_SPEED_FRACTION * handicap) // 100
end

local function good_window_ms()
    return BASE_GOOD_WINDOW_MS -
               (BASE_GOOD_WINDOW_MS * HANDICAP_WINDOW_FRACTION * handicap) //
               100
end

local function perfect_window_ms()
    return BASE_PERFECT_WINDOW_MS -
               (BASE_PERFECT_WINDOW_MS * HANDICAP_WINDOW_FRACTION *
                   handicap) // 100
end

-- Begins a brand new round: resets the timer, un-resolves it, and shows
-- (or hides) the strike-zone marker -- rounds 1-3 show it, from round 4
-- it is invisible, the twist the design's section 5 calls for.
local function start_round()
    round_elapsed_ms = 0
    round_resolved = false
    strike_marker:visible(round <= 3)
    set_status("ROUND " .. round .. "/" .. ROUNDS .. "  SCORE " ..
                    total_score)
    -- Clears last round's verdict as the new one starts, so PERFECT from
    -- three rounds ago is never still sitting there while the player is
    -- waiting to see what this press did.
    report_status("")
end

-- Called once, the instant Nibble becomes the active screen (the
-- was_showing edge below) -- NOT every frame, the same "begin spends the
-- energy cost, this must happen exactly once per playthrough" reasoning
-- kf_game_session_begin() itself is built around.
local function enter_nibble()
    total_score = 0
    round = 1
    finished = false

    local ctx = game.begin(manifest.id, manifest.reward.energy_cost_mp,
                            manifest.reward.need,
                            manifest.reward.need_fraction_percent)
    if ctx == nil then
        active = false
        set_status("NIBBLE")
        report_status("TOO TIRED TO PLAY -- PRESS B")
        strike_marker:visible(false)
        return
    end

    active = true
    handicap = ctx.handicap
    creature_obj:sprite(nibble_sprite_name("neutral"))
    creature_obj:move(CREATURE_BASE_X, CREATURE_Y)
    start_round()
end

-- Judges a single A-press against how far into this round's lead time it
-- landed. Called at most once per round (round_resolved guards it).
local function judge_press()
    local delta = round_elapsed_ms - lead_time_ms()
    if delta < 0 then
        delta = -delta
    end
    local verdict
    if delta <= perfect_window_ms() then
        total_score = total_score + POINTS_PERFECT
        game.score(POINTS_PERFECT)
        game.event("perfect")
        verdict = "PERFECT!"
    elseif delta <= good_window_ms() then
        total_score = total_score + POINTS_GOOD
        game.score(POINTS_GOOD)
        verdict = "GOOD"
    else
        -- Outside the good window: a miss, worth nothing -- Chris's
        -- "losing costs only the energy already spent" decision, already
        -- paid at game.begin() above.
        verdict = "MISS"
    end
    round_resolved = true
    set_status("ROUND " .. round .. "/" .. ROUNDS .. "  SCORE " ..
                    total_score)
    report_status(verdict)
end

local function finish_session()
    finished = true
    local tier = game.finish(manifest.reward.good, manifest.reward.great)
    local record = game.record(manifest.id)
    local streak = record ~= nil and record.streak or 0
    local best = record ~= nil and record.best or total_score
    set_status("SCORE " .. total_score .. "  BEST " .. best .. "  STREAK " ..
                    streak)
    report_status(string.upper(tier) .. "! -- PRESS B")
    strike_marker:visible(false)
    creature_obj:sprite(nibble_sprite_name("neutral"))
    creature_obj:move(CREATURE_BASE_X, CREATURE_Y)
end

-- Advances the food's position and the tell (creature lean + pose) for
-- the CURRENT round, purely a function of round_elapsed_ms -- no pixel
-- math anywhere else needs to agree with this.
local function update_food_and_tell()
    local lead = lead_time_ms()
    local t = round_elapsed_ms
    if t > lead then
        t = lead
    end
    local fraction = lead > 0 and (t / lead) or 1
    local x = FOOD_START_X + (STRIKE_X - FOOD_START_X) * fraction
    food:move(x // 1, FIELD_Y)

    local telling = (lead - round_elapsed_ms) <= TELL_LEAD_MS and
                         round_elapsed_ms <= lead
    if telling then
        creature_obj:move(CREATURE_BASE_X - TELL_OFFSET_PX, CREATURE_Y)
        creature_obj:sprite(nibble_sprite_name("happy"))
    else
        creature_obj:move(CREATURE_BASE_X, CREATURE_Y)
        creature_obj:sprite(nibble_sprite_name("neutral"))
    end
end

-- Chains onto whatever on_frame creature.lua already declared -- see
-- this file's own header comment for why this cannot be on_home_frame/
-- on_nibble_frame/anything screen-dedicated: there is no such per-screen
-- entry point for a script-declared screen, only the one shared
-- on_frame() every chunk in this VM already gets called through.
local previous_on_frame = on_frame

function on_frame(dt_ms)
    if previous_on_frame ~= nil then
        previous_on_frame(dt_ms)
    end

    local showing = kf.active_screen() == manifest.id
    if showing and not was_showing then
        enter_nibble()
    end
    was_showing = showing
    if not showing then
        return -- gated: no state changes, no drawing work, while some
               -- OTHER screen is the one actually on screen.
    end

    if not active or finished then
        return
    end

    round_elapsed_ms = round_elapsed_ms + dt_ms
    update_food_and_tell()

    if not round_resolved then
        if kf.button("a") then
            judge_press()
        elseif round_elapsed_ms > lead_time_ms() + good_window_ms() then
            round_resolved = true -- timed out with no press: a miss
        end
    end

    if round_resolved and
        round_elapsed_ms > lead_time_ms() + good_window_ms() +
            ROUND_SETTLE_MS then
        if round >= ROUNDS then
            finish_session()
        else
            round = round + 1
            start_round()
        end
    end
end
