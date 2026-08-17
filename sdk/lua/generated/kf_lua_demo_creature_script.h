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
--
-- Also declares the whole home screen now (Task 5, the Lua game-layer plan;
-- grouped via kf.screen() since ADR 0044), gated by kf.home_screen_active()
-- so this one file runs under either KF_HOME_SCREEN build. Layout numbers
-- match kf_creature_screen.cpp's own constants -- see that file for the why.
--
-- The info screen (Task 2 of the screens/clock/sleep plan, ADR 0045) is
-- declared below unconditionally -- unlike Home, Info does not care which
-- build owns the creature's own screen, so it is not gated behind
-- kf.home_screen_active().
--
-- The settings screen (Task 4 of that same plan, ADR 0047) is declared
-- below unconditionally too, for the identical reason -- the global system
-- clock is not part of any one creature's screen either.

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

-- Sound foundation, part 2: the creature's voice. MIRRORS tools/kf_
-- chiptune.py's own SOUNDS table EXACTLY -- note names, ':ms' durations,
-- duty tier. THE TWO MUST BE CHANGED TOGETHER: kf_chiptune.py is the
-- preview tool (renders a .wav a human can audition before flashing
-- anything), this table is the shipping copy the device actually plays via
-- kf.melody(). See that file's own header comment for the full design
-- rationale -- the rising-major-triad motif, why duty separates "the
-- creature's own voice" (kf.DUTY_THIN) from "system fanfares" (kf.DUTY_
-- FAT), why a want reads as a QUESTION, not a doorbell.
local SOUNDS = {
    want_food     = {kf.DUTY_THIN, "C7:32 C7:32 C7:32 F7:75"},
    want_play     = {kf.DUTY_THIN, "G6:55 C7:55 E7:90"},
    want_rest     = {kf.DUTY_THIN, "E6:70 -:30 B6:120"},
    want_bath     = {kf.DUTY_THIN, "C7:50 A6:50 D7:95"},
    want_flush    = {kf.DUTY_THIN, "E6:55 A6:55 D7:100"},
    want_again    = {kf.DUTY_THIN, "A6:50 C7:65 -:60 C7:50 E7:85"},
    want_whine    = {kf.DUTY_THIN, "B6:40 A#6:40 B6:40 A#6:40 D7:85"},
    care_feed     = {kf.DUTY_MID, "C7:50 E7:70"},
    care_play     = {kf.DUTY_MID, "D7:50 F#7:70"},
    care_rest     = {kf.DUTY_MID, "E7:50 G#7:70"},
    care_bath     = {kf.DUTY_MID, "F6:50 A6:70"},
    care_flush    = {kf.DUTY_MID, "G6:50 B6:70"},
    care_disliked = {kf.DUTY_MID, "E6:60 C6:90"},
    wake          = {kf.DUTY_MID, "C6:70 E6:70 G6:110"},
    sleep         = {kf.DUTY_THIN, "G6:90 E6:90 C6:150"},
    hatch         = {kf.DUTY_FAT, "C6:60 E6:60 G6:60 C7:60 E7:60 G7:60 C8:220"},
    evolve        = {kf.DUTY_FAT, "G6:55 C7:55 E7:55 G7:55 E7:55 G7:55 C8:240"},
    death         = {kf.DUTY_MID, "G6:150 D#6:150 C6:150 G5:400"},
}

-- Declared here, at the top level, rather than inside the Home-only block
-- below: hatch/evolve/death fire from announce_stage()/announce_death(),
-- which run from the SCREEN-AGNOSTIC on_frame() (see that function's own
-- header comment for why) and so must work whether or not this build's
-- Home screen is the Lua one.
local function play_sound(name)
    local spec = SOUNDS[name]
    if spec then
        kf.melody(spec[2], spec[1])
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

-- Sound foundation, part 2: has the creature ever been observed dead
-- before? Same "nil/false means first observation, stay quiet" convention
-- as previous_stage above -- a pet LOADED already dead should not
-- immediately announce a death that happened in a previous session.
local previous_dead = false

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
    -- The hatch/evolve sounds: the egg's own first stage change gets the
    -- motif fully resolved (hatch); every later one gets the same
    -- extension a step further transposed (evolve) -- see SOUNDS' own
    -- header comment. Guarded by pet.asleep() -- "never while asleep,
    -- except wake itself" applies here exactly as it does to every other
    -- sound in this file, including the rare, once-per-life ones: a stage
    -- boundary crossed during an offline catch-up that also lands the
    -- creature back in a sleep segment should stay silent until it wakes,
    -- same as a want would.
    if not pet.asleep() then
        if previous == "egg" then
            play_sound("hatch")
        else
            play_sound("evolve")
        end
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

-- Sound foundation, part 2: the death edge, the same shape announce_
-- stage() gives the hatch/evolve edges above -- fires once, the frame
-- pet.dead() first goes from false to true, never again for this
-- creature's remaining lifetime (dead is terminal, kf/pet.h's own
-- comment). Guarded by pet.asleep() for the identical reason the stage
-- transitions above are: this fires from the screen-agnostic on_frame(),
-- which runs regardless of whether the creature happens to be asleep at
-- the moment neglect finally catches up with it.
local function announce_death()
    local dead = pet.dead()
    if dead and not previous_dead and not pet.asleep() then
        play_sound("death")
    end
    previous_dead = dead
end

-- Home's objects and on_home_frame() both live inside this one `if` block,
-- only created/defined under KF_HOME_SCREEN=lua (kf.home_screen_active())
-- -- unlike info/settings below, Home does not exist at all under
-- KF_HOME_SCREEN=cpp, so there is nothing to gate at call time: on_home_
-- frame() itself simply never gets defined as a global under that build,
-- and kf_lua_port_home_frame() (sdk/lua/kf_lua_port.cpp) already treats an
-- undefined on_home_frame the same as any script that never defined it.
if kf.home_screen_active() then
    -- ADR 0044: Home's objects are declared through a named screen group
    -- rather than bare kf.* calls -- a receiver change only, same layout,
    -- same object count, same declaration order.
    local home = kf.screen("home")
    -- A warm cream. MUST match KF_CREATURE_PRESENTER_BG in
    -- simulator/src/pet/kf_creature_presenter.h, which is where the C side
    -- owns this same colour -- a Lua script cannot include a C header, so
    -- this copy is hand-maintained and home_clock_check is what catches it
    -- drifting. Change one, change the other, same commit.
    local bg = kf.color(248, 240, 216)
    home:background(bg)

    -- Chris, 2026-08-11, after testing: a small always-on digital clock in
    -- the play room's upper-left corner, so the wall clock stays visible
    -- while poking the debug buttons. kf.time() already returns a ready-
    -- formatted 12-hour string ("9:05 AM", or "--:-- --" unset) -- shown
    -- as-is, no reformatting here. Layer 2 keeps it readable even if the
    -- creature happens to wander underneath it (body is layer 1).
    local clock = home:text("--:-- --")
    clock:move(2, 2)
    clock:color(kf.BLACK, bg)
    clock:layer(2)
    local last_clock_text = nil -- only :set() when the string actually
                                 -- changes; see on_home_frame() below

    -- on_home_frame() below sets the real sprite/position every frame,
    -- including its first -- this placeholder is never painted.
    local body = home:sprite("")
    body:layer(1) -- over the mess

    local shrine = home:sprite("shrine_idle_s")
    shrine:move(96, 106) -- centred, 48x48

    -- Task 7 (the screens/clock/sleep plan):
    -- the tuck-in interaction's bedding. A real sprite, "futon_idle_s" --
    -- one generic 48x48 entry for EVERY stage, no per-stage/per-direction
    -- variants -- scenery, looked up by literal name exactly like
    -- shrine_idle_s above, never through the creature name resolver.
    -- Starts hidden; on_home_frame() below is the only place that shows
    -- it.
    local futon = home:sprite("futon_idle_s")
    futon:hide()
    local zzz = home:text("ZZZ")
    zzz:color(kf.BLACK, bg)
    zzz:hide()

    -- Task 8 (the screens/clock/sleep plan):
    -- the attention signal's pulsing indicator. One text object, shown/
    -- hidden at 1 Hz by on_home_frame() below while pet.wants() is
    -- non-nil -- see that function for the blink timing.
    local want_bang = home:text("!")
    want_bang:color(kf.BLACK, bg)
    want_bang:hide()

    local poop = {} -- 8 fixed slots, field 240px / 8
    for i = 1, 8 do
        poop[i] = home:box(12, 12, kf.color(92, 64, 51))
        poop[i]:move((i - 1) * 30 + 9, 232)
    end

    local names = {"HUNGER", "HAPPY", "ENERGY"}
    local colors = {
        kf.color(214, 118, 40), kf.color(224, 196, 32), kf.color(60, 140, 210),
    }
    local track_color = kf.color(190, 190, 190)
    local fill = {}
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

    -- Task 8: kept as named locals (`guide_labels`), not thrown away after
    -- the loop like before -- paint_guide() below re-colours whichever one
    -- names the wanted action every frame, the same kf_scene_set_colors()
    -- inversion trick the Settings cursor uses.
    local guide = {"1:FEED", "2:PLAY", "3:REST", "4:BATH", "5:FLUSH"}
    local guide_labels = {}
    for i = 1, 5 do -- centred per 48px slot
        local slot_x = (i - 1) * 48
        local w = #guide[i] * 6 -- KF_FONT_CELL_W
        local label = home:text(guide[i])
        label:move(slot_x + (48 - w) // 2, 300)
        label:color(kf.BLACK, bg)
        guide_labels[i] = label
    end

    -- Which guide slot names the button for each want. FLUSH is slot 5
    -- even though kf/pet.h's KF_PET_CARE_ACTION_COUNT is 4 -- flush is
    -- deliberately outside that enum (kf/pet.h's own comment on why), and
    -- this map is keyed by the guide's own layout, not that enum.
    local kWantGuideIndex = {FOOD = 1, PLAY = 2, REST = 3, BATH = 4, FLUSH = 5}

    -- Inverts exactly the guide entry naming `want` (nil paints every entry
    -- normal). Idempotent, like every scene setter here -- safe to call
    -- every frame regardless of whether anything actually changed.
    local function paint_guide(want)
        local active = want and kWantGuideIndex[want]
        for i = 1, 5 do
            if i == active then
                guide_labels[i]:color(bg, kf.BLACK)
            else
                guide_labels[i]:color(kf.BLACK, bg)
            end
        end
    end

    -- Task 8: front-centre of the wander field
    -- (simulator/src/pet/kf_creature_presenter.h's KF_CREATURE_PRESENTER_
    -- FIELD is {0,0,240,260}, sprites are 48x48) -- x matches the shrine's
    -- own horizontal-centre convention above, y sits the creature at the
    -- field's bottom edge, closest to the viewer, well clear of the needs
    -- bars starting at y=262.
    local kWantPoseX = 96
    local kWantPoseY = 212
    -- Centred above the held sprite: 96 + 24 (half the 48px sprite) - 3
    -- (half of one 6px font cell, "!" being one character wide).
    local kWantBangX = 117
    local kWantBangY = 196

    -- The <stage><branch> token kf_creature_sprite_name() (hakoniwaos/src/
    -- creature.cpp) builds in C++, reimplemented here because that
    -- function is private to Core and pet.wants() firing is a game-layer
    -- decision, not a Core one -- see kf/pet.h's own line on why WHAT a
    -- stage/branch number means is not Core's business. Never asked for
    -- while stage is "egg": eggs never decay (kf_pet_default_config()'s
    -- EGG row is all-zero rates), so pet.wants() can never return non-nil
    -- for one in practice, and the pack has no egg_objecting_* art at all
    -- (egg collapses to a single "idle" state, see kf_creature_sprite_
    -- name()'s own EGG special case) -- egg falls through to the plain
    -- stage name below only as a defensive default, never expected to draw.
    local function want_stage_token()
        local stage = pet.stage()
        if stage == "teen" then
            return "teen" .. pet.teen_form()
        elseif stage == "adult" then
            return "adult" .. pet.teen_form() .. pet.adult_branch()
        else
            return stage
        end
    end

    -- Global, not local -- kf_lua_port_home_frame() (sdk/lua/kf_lua_
    -- port.cpp) calls this by name while Home is the active screen, its
    -- own dedicated entry point, the same reasoning on_info_frame/on_
    -- settings_frame below are separate from on_frame(): one screen's
    -- per-frame logic must never touch another screen's objects. THIS WAS
    -- A REAL HARDWARE BUG, not just a design preference: this block used
    -- to live inside on_frame() below, guarded by `if kf.home_screen_
    -- active() then` -- a check that reads like "is Home showing right
    -- now" but is actually a BUILD-TIME flag, true for this entire
    -- process on a KF_HOME_SCREEN=lua build. on_frame() runs every real
    -- frame regardless of the active screen (the main loop calls it
    -- unconditionally), so that guard never actually excluded Info or
    -- Settings -- Home's creature/poop/shrine kept getting `:show()`n on
    -- top of whichever screen was really active, every frame, frozen
    -- (the wander only advances inside kf_lua_home_screen_frame(), which
    -- only runs while Home actually is active) but never hidden. Defined
    -- as a closure over `body`/`shrine`/`poop`/`fill` above, rather than
    -- forward-declared outer locals the way this used to be structured,
    -- because nothing outside this `if` block needs to see them any more.

    -- The futon's subtle wiggle: the same integer triangle wave kf_
    -- creature_presenter.cpp's egg_bob_offset_y() uses, period and
    -- amplitude matched to it for the identical "subtle, not distracting"
    -- feel -- duplicated here in Lua because that helper is private to its
    -- own C++ file. Integer-only on purpose, matching hakoniwaos's own
    -- no-float rule even though this script is not built by check_no_
    -- heap.py.
    local kFutonWigglePeriodMs = 3000
    local kFutonWiggleAmplitudePx = 2
    local futon_elapsed_ms = 0
    local function futon_wiggle_offset()
        local quarter = kFutonWigglePeriodMs // 4
        local phase = futon_elapsed_ms % kFutonWigglePeriodMs
        local a = kFutonWiggleAmplitudePx
        if phase < quarter then
            return (phase * a) // quarter
        elseif phase < 2 * quarter then
            return ((2 * quarter - phase) * a) // quarter
        elseif phase < 3 * quarter then
            return -((phase - 2 * quarter) * a) // quarter
        else
            return -((4 * quarter - phase) * a) // quarter
        end
    end

    -- ZZZ blinks slowly over the futon -- "flashing here and there, not
    -- rapid" (the owner's own words). Visible 2000ms of every 3000ms:
    -- mostly on, a brief pause, never a fast flicker. Both numbers are
    -- feel, to be judged on the board.
    local kZzzCyclePeriodMs = 3000
    local kZzzVisibleMs = 2000
    local zzz_elapsed_ms = 0

    -- The ZZZ's top edge may never enter the clock's row. Both places that
    -- position it derive from the creature's own position, which the wander
    -- can carry to the top of the field, and both used to clamp at 0 --
    -- putting the ZZZ underneath the clock, where layer 0 loses to the
    -- clock's layer 2 and it renders as a smear behind the digits.
    -- Derived, not chosen: the clock sits at y=2 (clock:move above) and a
    -- glyph cell is KF_FONT_CELL_H = 8 tall (kf/font.h), so it occupies
    -- rows 2..9; 10 is the first clear row and 12 leaves two spare. Raise
    -- the clock or grow the font and this needs to move with it.
    local kZzzMinY = 12

    -- 2026-08-11 bedtime-behaviour extension (ADR 0052): "extend the
    -- 'drowsy' timeframe to start 10 minutes before actual bedtime" --
    -- pet.drowsy() now answers this directly from Core (kf/pet.h's
    -- kf_pet_drowsy(), against the SAME kNightStartHour Core's own asleep
    -- computation uses), replacing the old local `kDrowsyHour = 21` literal
    -- this file used to duplicate Core's bedtime hour into by hand.

    -- The nodding-off loop, while drowsy: wander a little, stop and hold
    -- the sleeping pose with a brief ZZZ, then wander again, repeating
    -- until bedtime -- Chris's own words. Driven by dt_ms (real frame
    -- time), NOT any pet/game-time clock, so the cycle keeps the same real-
    -- world pace regardless of the debug speed multiplier -- with that
    -- multiplier turned up, Core's own ten-minute drowsy window can pass in
    -- a couple of real seconds, and this cycle will simply show one nod, a
    -- partial one, or none at all, exactly as an unmultiplied ten-minute
    -- window would look sped up. 10s wandering / 4s nodding: long enough
    -- for each phase to read clearly rather than flicker, short enough that
    -- more than one nod is visible inside a real, unmultiplied ten-minute
    -- window (about 42 cycles). Feel, like the ZZZ/wiggle timings above,
    -- not a tuned value.
    local kNodWanderMs = 10000
    local kNodPoseMs = 4000
    local nod_elapsed_ms = 0
    local nod_posing = false -- which half of the cycle we are in right now
    local nod_x, nod_y = 0, 0 -- frozen position for the duration of a nod

    local tucked_in = false -- decorative only; Core's own sleep timing
    local was_asleep = false -- notices the wake edge, to put the bedding away
    local bed_x, bed_y = 0, 0

    -- Chris, 2026-08-11, after testing: "the futon art doesn't show when
    -- its asleep, just the zzz" -- the futon is now the sleep visual,
    -- always, not only after a tuck-in. `was_bed_shown` notices the edge
    -- a fresh night starts (early, via tuck-in, or on Core's own asleep
    -- edge if nobody tucked in). `futon_night_index` rotates through the
    -- pack's 7 alternative futon designs, one per night, incremented on
    -- that same edge -- a counter, not math.random(): this codebase's
    -- determinism checks hash the framebuffer and compare runs, and a
    -- real PRNG would make that non-reproducible.
    local was_bed_shown = false
    local futon_night_index = 0

    -- Task 8: 1 Hz blink timing for want_bang. Visible half the cycle,
    -- hidden the other half -- a plain on/off blink, unlike ZZZ's slower
    -- "mostly on, brief pause" cadence above, because this is meant to read
    -- as urgent rather than restful. Reset to a fresh, visible-first cycle
    -- the frame a want first appears (was_wanting flips false -> true), so
    -- an attention signal never starts invisible.
    local kWantBangCyclePeriodMs = 1000
    local kWantBangVisibleMs = 500
    local want_elapsed_ms = 0
    local was_wanting = false

    -- Sound foundation, part 2: the want-ping system, replacing the single
    -- 880Hz attention chirp above with three LEVEL-CROSSING pings per unmet
    -- need, mapped onto SOUNDS' own escalation rungs (ping1 = the want's
    -- own call, ping2 = want_again, ping3 = want_whine). Owner's own words,
    -- 2026-08-12: "The unmet need rechirp should ping once when the care
    -- level gets to that level, then ping again about 1/2 the rest of the
    -- way to zero, then one urgent ping at 5 points/units before 0." No
    -- timer, no interval -- self-limiting by construction: a need that
    -- stops falling stops crossing thresholds, so it stops ping-ing on its
    -- own, without anything having to notice it recovered and cancel a
    -- countdown.
    --
    -- FOOD/PLAY/REST drain toward 0 -- ping1 is the want's own ON
    -- threshold (25%, kf/pet.h's KF_PET_WANT_*_ON_MP), ping2 is halfway
    -- from there to zero (12.5%), ping3 (urgent) is 5 percentage points
    -- before zero (5%) -- read literally from the owner's own "5
    -- points/units before 0".
    local kPing1FoodPlayRestMp = 25000
    local kPing2FoodPlayRestMp = 12500
    local kPing3FoodPlayRestMp = 5000

    -- BATH is driven by dirtiness, which RISES rather than drains -- the
    -- owner's own confirmed mapping: mirror the INTENT (three evenly-
    -- spaced pings, worst last), not the literal "halfway to zero" formula,
    -- since there is no zero to approach here. ping1 (80%) matches kf/
    -- pet.h's KF_PET_WANT_BATH_ON_MP (== KF_PET_DIRTY_STINK_MP) exactly;
    -- 90%/95% are evenly spaced on toward the 100% ceiling.
    local kPing1BathMp = 80000
    local kPing2BathMp = 90000
    local kPing3BathMp = 95000

    -- FLUSH is a discrete poop COUNT, not a level -- same mirrored intent,
    -- owner-confirmed: ping1 (3) matches kf/pet.h's KF_PET_WANT_FLUSH_ON_
    -- POOPS exactly; ping2/ping3 (5, 7) are evenly spaced two apart, up to
    -- KF_PET_MAX_POOPS (8).
    local kPing1FlushPoops = 3
    local kPing2FlushPoops = 5
    local kPing3FlushPoops = 7

    -- Which raw need each want name reads, and (for FOOD/PLAY/REST) which
    -- direction "worse" is -- drain_level()/rise_level() below take the
    -- three thresholds already named above, not a fourth copy of them.
    local function drain_level(mp)
        if mp <= kPing3FoodPlayRestMp then
            return 3
        elseif mp <= kPing2FoodPlayRestMp then
            return 2
        elseif mp <= kPing1FoodPlayRestMp then
            return 1
        else
            return 0
        end
    end

    local function rise_level(value, ping1, ping2, ping3)
        if value >= ping3 then
            return 3
        elseif value >= ping2 then
            return 2
        elseif value >= ping1 then
            return 1
        else
            return 0
        end
    end

    -- The want name each ping level's sound comes from -- ping1 is the
    -- want's own call (SOUNDS' want_food/want_play/.../want_flush), ping2
    -- is always want_again, ping3 is always want_whine: the SAME two
    -- escalation rungs across every need, not five separate escalation
    -- sounds, matching kf_chiptune.py's own header comment ("Two rungs:
    -- asked again, then a whine... the SAME creature, not a new alarm").
    local kWantSoundName = {
        FOOD = "want_food", PLAY = "want_play", REST = "want_rest",
        BATH = "want_bath", FLUSH = "want_flush",
    }
    local function ping_sound_name(want_name, level)
        if level >= 3 then
            return "want_whine"
        elseif level >= 2 then
            return "want_again"
        else
            return kWantSoundName[want_name]
        end
    end

    -- One latched level (0..3) per need -- 0 means "not currently past
    -- ping1". A ping SOUND fires only the frame the level goes UP (a new,
    -- deeper threshold just crossed); the level dropping back down is
    -- ALWAYS silent (the need recovering, whether from care or simply not
    -- having fallen that far) -- this is the "the latch must reset when
    -- the need recovers back above the level" requirement, expressed as a
    -- single integer per need rather than three separate booleans: keeping
    -- only the DEEPEST currently-crossed level means a partial recovery
    -- that clears ping3 but not ping1 leaves ping1 still latched (it does
    -- not re-fire), while a full recovery clears all three silently,
    -- ready to ping again from ping1 the next time this need falls that
    -- far. `awake` gates the SOUND only -- the level itself is tracked
    -- every frame regardless, so a night's worth of unprotected decay
    -- (ADR 0053) does not surface as a burst of pings the instant the
    -- creature wakes; it simply reflects the current true level, silently
    -- caught up, same as every other muted-while-asleep sound in this
    -- file.
    local ping_level = {FOOD = 0, PLAY = 0, REST = 0, BATH = 0, FLUSH = 0}
    local function update_ping(want_name, current_level, awake)
        local previous = ping_level[want_name]
        ping_level[want_name] = current_level
        if current_level > previous and awake then
            play_sound(ping_sound_name(want_name, current_level))
        end
    end

    -- Care-response sounds: which sound plays for a JUST-accepted care
    -- action, given the reaction it produced. `2` is KF_PET_REACTION_
    -- DISLIKED (kf/pet.h) -- a raw literal, not a named Lua constant,
    -- because pet.last_reaction() itself is documented to return the raw
    -- enum integer (sdk/lua/kf_lua_port.cpp's own comment on why: "a
    -- script showing a reaction is picking a sprite... not printing a
    -- word"), and this file already reads pet.stage()/pet.teen_form() the
    -- same raw-index way elsewhere.
    local function care_sound(base_name)
        if pet.last_reaction() == 2 then
            play_sound("care_disliked")
        else
            play_sound(base_name)
        end
    end

    function on_home_frame(dt_ms)
        local clock_str = kf.time()
        if clock_str ~= last_clock_text then
            clock:set(clock_str)
            last_clock_text = clock_str
        end

        fill[1]:size(pet.hunger() * 190 // 100000, 8)
        fill[2]:size(pet.happiness() * 190 // 100000, 8)
        fill[3]:size(pet.energy() * 190 // 100000, 8)

        if pet.dead() then
            shrine:show()
            body:hide()
            futon:hide()
            zzz:hide()
            want_bang:hide()
            was_wanting = false
            paint_guide(nil)
        else
            shrine:hide()
            local poops = pet.poops()
            for i = 1, 8 do
                if i <= poops then poop[i]:show() else poop[i]:hide() end
            end

            -- Captured BEFORE the manual-wake check below can change
            -- `asleep` -- this is the state kf_home_screen_input.cpp (the
            -- shared C++ care-button handler, called earlier THIS SAME
            -- frame, before creature.lua ever runs) actually saw when it
            -- decided whether to apply a feed/play/rest/bath/flush press.
            -- The care-sound block further down gates on THIS value, not
            -- the post-wake `asleep` local -- otherwise pressing A to wake
            -- a sleeping creature would ALSO play care_feed the same
            -- frame, even though kf_home_screen_input.cpp's own `if (pet-
            -- >asleep) return;` guard means no feed actually happened.
            local raw_asleep = pet.asleep()
            local asleep = raw_asleep

            -- Waking it deliberately is allowed and costs happiness (the
            -- spec's own words, kept small -- pet.wake()). Checked before
            -- anything below reads `asleep` again, so a wake this frame
            -- shows immediately rather than one frame late.
            if asleep and kf.button("a") then
                pet.wake()
                asleep = false
            end

            -- Sound foundation, part 2: the wake/sleep edges -- fires on
            -- the SAME Core-driven asleep<->awake transition kf_pet_state's
            -- own `asleep` field defines, covering both a deliberate
            -- kf.button("a") wake (above, already reflected in `asleep`
            -- this same frame) and Core's own automatic morning wake.
            -- "Never while asleep, except wake itself" (this task's own
            -- rule): the sleep sound is the one place that guard does NOT
            -- apply -- it fires exactly AT the transition INTO asleep, by
            -- design, not "while" asleep.
            if not was_asleep and asleep then
                play_sound("sleep")
            end
            if was_asleep and not asleep then
                play_sound("wake")
            end

            -- The morning: Core wakes the creature on its own (the next
            -- clock-crossing segment simply computes asleep = false) --
            -- "it wakes, gets itself out of bed, and puts the bedding
            -- away" is this edge, not a player action.
            if was_asleep and not asleep then
                tucked_in = false
            end
            was_asleep = asleep

            -- Sound foundation, part 2: care-response sounds, edge-
            -- detected via the SAME debounced button press kf_home_screen_
            -- input.cpp (called earlier this frame, before this script's
            -- on_home_frame) already used to decide whether to feed/rest/
            -- bath/flush -- kf.button() here reads the identical bit, and
            -- pet.last_reaction() already reflects the JUST-applied result
            -- of that same press by the time this runs, not a stale one
            -- from an earlier frame. Gated on `raw_asleep`, NOT `asleep`
            -- -- see that local's own comment above for why.
            --
            -- UP/"care_play" is the one exception: since Task 5 of the
            -- Nibble-and-the-game-session plan, UP no longer triggers a
            -- care action here at all (kf_home_screen_input.cpp's own UP
            -- branch moved out -- see that header's own top-of-file
            -- comment) -- it opens the play picker instead (this file's
            -- own on_frame(), below). The SAME sound still plays on the
            -- SAME press, now as feedback for opening the picker rather
            -- than for a play action that already happened -- input
            -- feedback for the button, not a claim about what it did.
            if not raw_asleep then
                if kf.button("a") then care_sound("care_feed") end
                if kf.button("up") then care_sound("care_play") end
                if kf.button("down") then care_sound("care_rest") end
                if kf.button("left") then care_sound("care_bath") end
                if kf.button("right") then play_sound("care_flush") end
            end

            -- Sound foundation, part 2: the want-ping system itself --
            -- see kPing1FoodPlayRestMp/update_ping()'s own comments above.
            -- Evaluated every frame regardless of bed_shown/asleep (the
            -- level is tracked continuously; only the SOUND is gated,
            -- inside update_ping() via the `not asleep` argument here),
            -- and regardless of which want kf_pet_wants()'s own priority
            -- order currently displays -- this reads the five raw needs
            -- directly, not the single resolved `want` below, so a need
            -- crossing a threshold pings even while a DIFFERENT want has
            -- priority for the visual attention signal.
            update_ping("FOOD", drain_level(pet.hunger()), not asleep)
            update_ping("PLAY", drain_level(pet.happiness()), not asleep)
            update_ping("REST", drain_level(pet.energy()), not asleep)
            update_ping("BATH", rise_level(pet.dirtiness(), kPing1BathMp,
                                            kPing2BathMp, kPing3BathMp),
                         not asleep)
            update_ping("FLUSH", rise_level(pet.poops(), kPing1FlushPoops,
                                             kPing2FlushPoops,
                                             kPing3FlushPoops),
                         not asleep)

            -- Tuck-in: available only during Core's own ten-minute drowsy
            -- window (kf_pet_drowsy(), ADR 0052), while still awake and not
            -- already tucked in. Pressing B here does TWO things now, where
            -- it used to do only one: pet.tuck_in() sets the real, SAVED
            -- Core flag that pays off next morning (kf/pet.h's kf_pet_
            -- tuck_in(), a no-op if pet.drowsy() is somehow false by the
            -- time this runs -- it re-checks its own gate independently of
            -- this script's `drowsy` read the same frame), and the local
            -- `tucked_in` below stays exactly what it always was: purely
            -- decorative, making the futon appear EARLY, while still awake.
            -- Core still has no "settle early" mechanism (ADR 0048), so
            -- pressing B never changes WHEN the creature actually falls
            -- asleep, only the wake-up bonus and how early the bedding
            -- shows.
            local drowsy = pet.drowsy() and not tucked_in
            if drowsy and kf.button("b") then
                tucked_in = true
                pet.tuck_in()
            end

            -- The futon is the sleep visual now, whenever Core says
            -- asleep, not only after a tuck-in -- tuck-in just makes it
            -- appear earlier. Falling asleep afterwards simply continues
            -- showing the same futon rather than swapping to a new one.
            local bed_shown = tucked_in or asleep

            if bed_shown and not was_bed_shown then
                -- A fresh night begins: pick a bed position and a bedding
                -- design once, and hold both for the whole night. See
                -- was_bed_shown's own declaration above for why this is a
                -- rotation, not math.random().
                bed_x, bed_y = creature.x(), creature.y()
                futon:frame(futon_night_index % 7)
                futon_night_index = futon_night_index + 1
                futon_elapsed_ms = 0
            end
            was_bed_shown = bed_shown

            if bed_shown then
                -- The futon's own art already shows a sleeping shape, so
                -- the creature's own body sprite is hidden rather than
                -- drawn underneath it -- true for every stage, including
                -- adult, which has no *_sleeping_* art of its own at all.
                body:hide()
                futon_elapsed_ms = futon_elapsed_ms + dt_ms
                futon:show()
                futon:move(bed_x, bed_y + futon_wiggle_offset())

                zzz_elapsed_ms = zzz_elapsed_ms + dt_ms
                if (zzz_elapsed_ms % kZzzCyclePeriodMs) < kZzzVisibleMs then
                    zzz:show()
                else
                    zzz:hide()
                end
                zzz:move(bed_x + 30, math.max(kZzzMinY, bed_y - 10))

                -- Nothing to want while the bedding is out -- pet.wants()
                -- already reads nil the instant Core's own asleep actually
                -- flips true, but tucked_in can show the futon a little
                -- earlier than that (the drowsy hour, still technically
                -- awake), so this is cleared explicitly rather than left
                -- to coincide.
                want_bang:hide()
                was_wanting = false
                paint_guide(nil)
            else
                futon:hide()
                -- The ZZZ is the futon's companion and is hidden with it,
                -- here, rather than in each of the awake branches below.
                -- It used to be hidden only inside the `if want` else-branch
                -- (the nodding-off loop), which meant a want arriving while
                -- the creature was mid-nod-pose stranded a visible ZZZ:
                -- every hide it could reach had just become unreachable, so
                -- it stayed frozen on screen -- behind the clock, on layer
                -- 0 -- until the want was satisfied. Clearing it on the way
                -- out of the bed state holds no matter which branch runs
                -- next; the nodding-off loop re-shows it on its own frames.
                zzz:hide()
                local want = pet.wants()
                if want then
                    -- Task 8: the attention signal. The creature stops
                    -- wandering, moves to the front-centre of the field and
                    -- holds the "objecting" pose -- art that already exists
                    -- for every stage but egg (which never reaches this
                    -- branch in practice; see want_stage_token() above).
                    body:show()
                    body:sprite(want_stage_token() .. "_objecting_s")
                    body:flip(false)
                    body:frame(0) -- objecting is a single-frame pose
                    body:move(kWantPoseX, kWantPoseY)

                    -- VISUAL only now -- the "!" blink's own reset, unchanged
                    -- from Task 8. The SOUND used to fire here too (a single
                    -- kf.tone() on this exact edge); it is now driven
                    -- independently by the want-ping system above
                    -- (update_ping()), which pings on raw need-level
                    -- crossings rather than this coarse "is anything wanted"
                    -- boolean -- see that system's own header comment for
                    -- why (a want-TYPE change while `want` stays non-nil
                    -- throughout would never trip this edge at all).
                    if not was_wanting then
                        want_elapsed_ms = 0
                    end
                    was_wanting = true
                    want_elapsed_ms = want_elapsed_ms + dt_ms
                    if (want_elapsed_ms % kWantBangCyclePeriodMs) <
                        kWantBangVisibleMs then
                        want_bang:show()
                    else
                        want_bang:hide()
                    end
                    want_bang:move(kWantBangX, kWantBangY)

                    paint_guide(want)
                else
                    was_wanting = false
                    want_bang:hide()
                    paint_guide(nil)

                    -- Wide awake, not wanting anything -- creature.sprite()
                    -- already resolves the right pose (kf_creature_pose_
                    -- for()); bed_shown above has already handled asleep.
                    body:show()

                    if drowsy then
                        -- ADR 0052: the nodding-off loop. Wander a little,
                        -- stop and hold the sleeping pose with a brief ZZZ,
                        -- then wander again, repeating until bedtime --
                        -- Chris's own words. See kNodWanderMs/kNodPoseMs
                        -- above for the cycle length and why dt_ms drives
                        -- it. Reuses the SAME *_sleeping_* art the real
                        -- asleep pose uses (want_stage_token() is the exact
                        -- helper the attention signal above already builds
                        -- this token with) -- no new art for this.
                        nod_elapsed_ms = nod_elapsed_ms + dt_ms
                        local phase =
                            nod_elapsed_ms % (kNodWanderMs + kNodPoseMs)
                        local posing_now = phase >= kNodWanderMs
                        if posing_now and not nod_posing then
                            -- Just entered the pose half: freeze the
                            -- position ONCE, here, rather than every frame
                            -- of the pose -- that is what makes the
                            -- creature look like it actually stopped,
                            -- instead of merely posing while still sliding
                            -- toward wherever the underlying wander (which
                            -- keeps running regardless -- see kf_creature_
                            -- presenter.h's own header comment) has reached
                            -- by now.
                            nod_x, nod_y = creature.x(), creature.y()
                        end
                        nod_posing = posing_now

                        if nod_posing then
                            body:sprite(want_stage_token() .. "_sleeping_s")
                            body:flip(false)
                            body:frame(0) -- the sleeping art is single-frame
                            body:move(nod_x, nod_y)
                            zzz:show()
                            zzz:move(nod_x + 30, math.max(kZzzMinY, nod_y - 10))
                        else
                            body:sprite(creature.sprite())
                            body:flip(creature.mirrored())
                            body:frame(creature.frame())
                            body:move(creature.x(), creature.y())
                            zzz:hide()
                        end
                    else
                        -- Not drowsy: a fresh nodding cycle starts clean
                        -- the next time it becomes true, rather than
                        -- resuming mid-pose from whatever the last drowsy
                        -- window left behind.
                        nod_elapsed_ms = 0
                        nod_posing = false
                        zzz:hide()
                        body:sprite(creature.sprite())
                        body:flip(creature.mirrored())
                        body:frame(creature.frame())
                        body:move(creature.x(), creature.y())
                    end
                end
            end
        end
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
-- point from on_home_frame() above and the screen-agnostic on_frame()
-- below on purpose: a screen's own per-frame drawing must never run while
-- some OTHER screen is the one actually showing -- see kf_lua_port.h's own
-- header comment on kf_lua_port_home_frame() for the hardware bug that
-- taught this codebase why, the hard way.
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

-- Task 4 of the screens/clock/sleep plan, extended by the sound-foundation
-- follow-up's volume setting: the
-- global system clock AND volume -- read, edited and saved with the seven
-- hardware buttons, registered third so MENU cycles HOME -> INFO ->
-- SETTINGS -> HOME. The cursor logic (which field is selected, hold-to-
-- repeat) lives in C++ (kf_lua_settings_screen.cpp) and reads the buttons
-- directly, NOT through kf.on_button -- see that file's own header comment
-- for why a shared button registry would let this screen's buttons also
-- fire while Home is showing. This screen only ever DRAWS: on_settings_
-- frame below gets handed the current field/hour/minute/AM-PM/save-
-- result/volume every frame and sets text and colour, nothing else.
local settings_screen = kf.screen("settings")
local settings_bg = kf.color(20, 24, 32)
settings_screen:background(settings_bg)

local function settings_label(str, x, y)
    local t = settings_screen:text(str)
    t:move(x, y)
    t:color(kf.WHITE, settings_bg)
    return t
end

do
    local title = "SETTINGS"
    settings_label(title, (kf.width() - #title * 6) // 2, 4) -- 6 = KF_FONT_CELL_W
end
settings_label("HOUR", 16, 60)
settings_label("MIN", 96, 60)
settings_label("AM/PM", 160, 60)
local hour_value = settings_label("", 16, 80)
local min_value = settings_label("", 96, 80)
local ampm_value = settings_label("", 160, 80)
settings_label("VOLUME", 16, 110)
local volume_value = settings_label("", 16, 130)

-- Presentation-only follow-up to ADR 0057's volume setting (owner's own
-- request, after trying the control on real hardware): a small bar-meter
-- icon beside the VOLUME value, drawn from kf.box() primitives -- no new
-- art, no generation cost, no flash, no manifest entry, and it scales with
-- the level for free. Four bars of increasing height, one per non-off
-- level (1..4); "filled" (kVolumeBarLit) up to the current level, "hollow"
-- (kVolumeBarDim, a muted grey close to settings_bg) above it -- kf.box()
-- has no outline primitive, only a solid fill, so "hollow" here means
-- dim/background-toned rather than a true unfilled outline; at this size
-- (3x13px at most) a dim fill reads as unmistakably "not lit" at a glance,
-- which is the actual requirement. Fixed x position regardless of label
-- width (16 + 5*6 = 46, one cell past "100%", the widest label) so the
-- icon never jitters as the value text changes width.
local kVolumeIconX = 46
local kVolumeBarW = 3
local kVolumeBarGap = 2
local kVolumeBarBottomY = 138 -- bottom-aligned with the value text's own
                               -- cell (y=130, KF_FONT_CELL_H=8 tall)
local kVolumeBarHeights = {4, 7, 10, 13}
local kVolumeBarLit = kf.WHITE
local kVolumeBarDim = kf.color(70, 74, 86)

local volume_bars = {}
for i = 1, 4 do
    local h = kVolumeBarHeights[i]
    local b = settings_screen:box(kVolumeBarW, h, kVolumeBarDim)
    b:move(kVolumeIconX + (i - 1) * (kVolumeBarW + kVolumeBarGap),
           kVolumeBarBottomY - h)
    volume_bars[i] = b
end

-- OFF is genuinely silent (kf_audio_set_volume()'s own contract, kf/hal/
-- audio.h) and must not LOOK like "all four bars quiet" -- that is exactly
-- the "silent and quiet must not look alike at a glance" failure mode --
-- so OFF hides every bar and shows this single "X" glyph instead, an
-- unmistakably different shape from any partial-bars configuration (bars
-- always show an increasing-height run, never a lone centred glyph).
local volume_mute = settings_label("X", kVolumeIconX, 130)
volume_mute:hide()

-- BRIGHTNESS, laid out to mirror VOLUME exactly one block below it: same
-- label/value/meter arrangement, same bar geometry, same lit/dim colours.
-- Deliberately identical rather than merely similar -- these two are the
-- same KIND of control (a small ordered set of levels, edited with UP/DOWN,
-- committed on SAVE), and a player who has learned one has learned the
-- other.
--
-- The ONE difference is that there is no OFF position and therefore no "X"
-- glyph: brightness runs 1..4. See kf/settings.h for why a zero-brightness
-- position would be a device that looks broken with no way to show you the
-- menu that would fix it.
settings_label("BRIGHTNESS", 16, 160)
local brightness_value = settings_label("", 16, 180)

local kBrightBarBottomY = 188 -- bottom-aligned with the value text at y=180
local brightness_bars = {}
for i = 1, 4 do
    local h = kVolumeBarHeights[i]
    local b = settings_screen:box(kVolumeBarW, h, kVolumeBarDim)
    b:move(kVolumeIconX + (i - 1) * (kVolumeBarW + kVolumeBarGap),
           kBrightBarBottomY - h)
    brightness_bars[i] = b
end

local save_row = settings_label("SAVE", 16, 215)
settings_label("B: CANCEL", 16, 280)

-- Minutes read "05", not "5" -- hours do not: kf.time()'s own "9:05 AM"
-- never zero-pads the hour, and this editor should not invent a
-- convention kf.time() itself does not use.
local function pad2(n)
    if n < 10 then
        return "0" .. n
    end
    return "" .. n
end

-- 0..4 (KF_VOLUME_OFF..KF_VOLUME_4, kf/hal/audio.h) -> what the field
-- shows -- "OFF" reads unambiguously as genuinely silent, matching kf_
-- audio_set_volume()'s own contract, and is NEVER "0%" (the owner's own
-- explicit instruction: 0% reads as a quantity, "OFF" reads as a state).
-- Levels 1..4 are the SAME four positions kf_audio_set_volume() has always
-- had, now labelled as the amplitude fraction each one actually sets
-- (volume_gain_permille() in simulator/src/sdl/sdl_audio.cpp and ports/
-- esp32/hal/esp_audio.cpp: 250/500/750/1000 permille of full amplitude) --
-- a presentation change only, per the owner's own request; the gain curve
-- itself is untouched. See this file's own header comment above the
-- volume bars for why these are amplitude percentages, not perceived-
-- loudness ones, and why that is flagged rather than silently "fixed".
local kVolumeLabel = {
    [0] = "OFF", [1] = "25%", [2] = "50%", [3] = "75%", [4] = "100%",
}

-- DEBUG/TEST ONLY -- read back by kf_lua_port_debug_settings_volume_label()/
-- _bars() (sdk/lua/kf_lua_port.cpp), for exactly the reason those two
-- functions' own header comments give: kf.report()'s one-way channel
-- carries a single integer, and nothing on the C++ side of the Lua/SDK
-- boundary can read a live scene object's text or colour back (kf_lua_
-- scene.cpp's own comment on LuaSceneObject explains why -- kf/scene.h's
-- Core API is write-only past kf_scene_bounds()). Two plain globals,
-- written every on_settings_frame() call alongside the real drawing below,
-- never read by anything else in this script. kf_settings_debug_volume_
-- bars is 4 characters, one per bar ('1' lit, '0' dim, left to right), or
-- the literal string "MUTE" at OFF.
kf_settings_debug_volume_label = ""
kf_settings_debug_volume_bars = ""

-- Brightness percentages, matching kf_settings_brightness_duty()'s own
-- curve (hakoniwaos/src/settings.cpp) rather than being evenly spaced
-- numbers that happen to look tidy: those duties are 26/64/140/255, which
-- ARE these percentages. Perceptual spacing, for the same reason the volume
-- gain curve is -- see kf/settings.h. If that curve changes, these change
-- with it; they are two spellings of one set of numbers.
local kBrightnessLabel = {
    [1] = "10%", [2] = "25%", [3] = "55%", [4] = "100%",
}

kf_settings_debug_brightness_label = ""
kf_settings_debug_brightness_bars = ""

-- Draws the bar-meter icon for the CURRENT (unsaved) volume field value --
-- called from on_settings_frame() below, every frame, the same "idempotent,
-- safe to call regardless of whether anything actually changed" contract
-- paint_guide()/paint() already have on this screen. Bar objects are boxes
-- (kf.box()): only :size()/:color()/position/visibility ever change a
-- box's declared state (hakoniwaos/src/scene.cpp's own changed() switch),
-- and this function only ever touches :color()/:show()/:hide() -- never
-- :size() or :move() -- so a level that repeats frame to frame declares
-- nothing different and the retained scene's own diff drops it before it
-- ever becomes a dirty rectangle.
-- Brightness's own meter. Same contract as paint_volume_meter() below and
-- the same reason it only touches :color() -- see that function's comment.
-- Simpler because there is no OFF case to special-case.
local function paint_brightness_meter(level)
    local bits = {}
    for i = 1, 4 do
        if i <= level then
            brightness_bars[i]:color(kVolumeBarLit)
            bits[i] = "1"
        else
            brightness_bars[i]:color(kVolumeBarDim)
            bits[i] = "0"
        end
    end
    kf_settings_debug_brightness_bars = table.concat(bits)
end

local function paint_volume_meter(level)
    if level <= 0 then
        for i = 1, 4 do
            volume_bars[i]:hide()
        end
        volume_mute:show()
        kf_settings_debug_volume_bars = "MUTE"
        return
    end
    volume_mute:hide()
    local bits = {}
    for i = 1, 4 do
        volume_bars[i]:show()
        if i <= level then
            volume_bars[i]:color(kVolumeBarLit)
            bits[i] = "1"
        else
            volume_bars[i]:color(kVolumeBarDim)
            bits[i] = "0"
        end
    end
    kf_settings_debug_volume_bars = table.concat(bits)
end

-- Global, not local -- kf_lua_port_settings_frame() (sdk/lua/kf_lua_
-- port.cpp) calls this by name while Settings is the active screen, its
-- own dedicated entry point for the identical reason on_info_frame() is
-- separate from on_frame(): one screen's per-frame logic must never touch
-- another screen's objects. `saved` is nil (no save attempted since this
-- screen was opened), true, or false -- ONE result covering both the clock
-- and the volume (kf_lua_settings_screen.cpp's own commit_save()).
function on_settings_frame(dt_ms, field, hour, minute, ampm, saved, volume,
                            brightness)
    hour_value:set("" .. hour)
    min_value:set(pad2(minute))
    ampm_value:set(ampm)
    local volume_label = kVolumeLabel[volume] or "?"
    volume_value:set(volume_label)
    kf_settings_debug_volume_label = volume_label
    paint_volume_meter(volume)
    local brightness_label = kBrightnessLabel[brightness] or "?"
    brightness_value:set(brightness_label)
    kf_settings_debug_brightness_label = brightness_label
    paint_brightness_meter(brightness)

    if saved == true then
        save_row:set("SAVED")
    elseif saved == false then
        save_row:set("SAVE FAILED")
    else
        save_row:set("SAVE")
    end

    -- Highlights exactly the selected field by inverting its colours --
    -- kf_scene_set_colors() already does this, no new drawing primitive
    -- needed (the plan's own answer to the button-map question).
    local function paint(obj, name)
        if field == name then
            obj:color(settings_bg, kf.WHITE)
        else
            obj:color(kf.WHITE, settings_bg)
        end
    end
    paint(hour_value, "hour")
    paint(min_value, "minute")
    paint(ampm_value, "ampm")
    paint(volume_value, "volume")
    paint(brightness_value, "brightness")
    paint(save_row, "save")
end

----------------------------------------------------------------------
-- Task 5 of the Nibble-and-the-game-session plan: the play picker.
-- Home's 2:PLAY (UP) used to call pet.play() directly -- see kf_home_
-- screen_input.h's own top-of-file comment for exactly what moved and
-- why. It now opens THIS screen instead: Quick play (the identical care
-- action, unchanged, via pet.quick_play()) or Nibble.
--
-- ONE text object, not two separate "Quick play"/"Nibble" lines: see
-- nibble.lua's own object-budget comment (examples/creature_demo/
-- nibble.lua) for why -- creature.lua's existing Home/Info/Settings
-- objects already hold the retained scene close to KF_SCENE_MAX_OBJECTS
-- (64), and this screen has exactly as little room to spend as Nibble
-- did.
--
-- Declared here, not inside the Home-only `do ... end` block above:
-- that block's locals (raw_asleep, care_sound, `home` itself) are not
-- visible outside their own scope, and this screen's own logic (below,
-- inside on_frame()) needs none of them -- kf.screen(name) is
-- create-or-fetch, so re-fetching "home"/"nibble" by name here is the
-- same call every OTHER cross-screen reference in this codebase already
-- makes rather than threading a local through.
----------------------------------------------------------------------
local play_menu = kf.screen("play_menu")
play_menu:background(kf.color(18, 22, 26))
local play_menu_label = play_menu:text("")
play_menu_label:move(8, 8)
play_menu_label:color(kf.WHITE, kf.color(18, 22, 26))

local play_menu_was_showing = false

-- Screen-agnostic: the main loop (sdl_main.cpp/app_main.cpp) calls this
-- unconditionally, every real frame, regardless of which screen is active
-- -- these three announcements are pet-state observations, not any one
-- screen's pixels, so they keep firing whether the owner is looking at
-- Home, Info or Settings. A screen's own drawing belongs in that screen's
-- OWN entry point instead (on_home_frame/on_info_frame/on_settings_frame
-- above) -- see kf_lua_port.h's own comment on kf_lua_port_home_frame()
-- for why Home's drawing used to live here, and what that cost.
--
-- The play picker's own logic lives here too, at the bottom, for the
-- identical reason Nibble's does (examples/creature_demo/nibble.lua's own
-- header comment): a kf.screen()-declared screen with no C-side update
-- function has no dedicated per-frame entry point of its own, only this
-- shared one -- gated on kf.active_screen() so none of it runs, and
-- nothing it reads (pet.asleep(), kf.button()) is even evaluated, while
-- some OTHER screen is the one actually showing.
function on_frame(dt_ms)
    announce_stage()
    announce_death()
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

    -- Opens the picker on UP, from Home only -- gated on `not pet.
    -- asleep()`, matching exactly what kf_home_screen_input.cpp's own
    -- leading guard used to block the direct play action on, before this
    -- task moved it out (kf_home_screen_quick_play()'s own header
    -- comment says the same thing from the other side).
    if kf.active_screen() == "home" and not pet.asleep() and
        kf.button("up") then
        play_menu:show()
    end

    local showing_menu = kf.active_screen() == "play_menu"
    if showing_menu then
        if not play_menu_was_showing then
            -- Entered just this frame: refresh Nibble's best/streak --
            -- game.record() (Task 3), nil if Nibble has never been
            -- played, matching game.record()'s own documented contract.
            --
            -- KEPT DELIBERATELY SHORT: KF_SCENE_TEXT_MAX (kf/scene.h) is
            -- 40 characters, and a longer string is silently truncated,
            -- not rejected -- an earlier version of this line ran to 52
            -- and lost its own streak number off the end without any
            -- error, only a log line nobody watching the panel would
            -- see. This format stays under 40 even at record.best's
            -- worst case (a full 10-digit uint32) and record.streak's
            -- (3-digit, capped at 255 -- kf/game.h's own streak_days).
            local record = game.record("nibble")
            local best = record ~= nil and record.best or 0
            local streak = record ~= nil and record.streak or 0
            play_menu_label:set("L:QUICKPLAY R:NIBBLE B" .. best .. " S" ..
                                     streak)
        end
        -- Quick play returns to Home immediately -- the whole point of
        -- Chris's "a player with ten seconds still has a way to meet the
        -- play need" decision is that this is the FAST path, not a second
        -- menu to navigate out of by hand. Nibble instead stays on its
        -- own screen, exactly like choosing it is meant to.
        if kf.button("left") then
            pet.quick_play()
            kf.screen("home"):show()
        elseif kf.button("right") then
            kf.screen("nibble"):show()
        end
        -- B returns Home from the picker too, for free -- kf_screen_nav_
        -- frame() (simulator/src/pet/kf_screen_nav.cpp) already jumps to
        -- Home on a B edge from ANY screen, before this function ever
        -- runs this same frame; nothing here needs to special-case it.
    end
    play_menu_was_showing = showing_menu
end

kf.log("the creature stirs")
)lua";

/* Leading '=' is Lua's own convention for "show this name verbatim in
 * error messages" -- see kf_lua_proof_script.h's identical comment on its
 * own chunk name for why, unchanged here. */
inline constexpr const char *kKfLuaDemoCreatureScriptChunkName =
    "=demo_creature";

#endif /* KF_LUA_DEMO_CREATURE_SCRIPT_H */
