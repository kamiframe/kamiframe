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
    local bg = kf.color(232, 240, 216)
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

    -- Task 7 (docs/superpowers/plans/2026-08-13-screens-clock-sleep.md):
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

    -- Task 8 (docs/superpowers/plans/2026-08-13-screens-clock-sleep.md):
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

            local asleep = pet.asleep()

            -- Waking it deliberately is allowed and costs happiness (the
            -- spec's own words, kept small -- pet.wake()). Checked before
            -- anything below reads `asleep` again, so a wake this frame
            -- shows immediately rather than one frame late.
            if asleep and kf.button("a") then
                pet.wake()
                asleep = false
            end

            -- The morning: Core wakes the creature on its own (the next
            -- clock-crossing segment simply computes asleep = false) --
            -- "it wakes, gets itself out of bed, and puts the bedding
            -- away" is this edge, not a player action.
            if was_asleep and not asleep then
                tucked_in = false
            end
            was_asleep = asleep

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
                zzz:move(bed_x + 30, math.max(0, bed_y - 10))

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
                            zzz:move(nod_x + 30, math.max(0, nod_y - 10))
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

-- Task 4 of docs/superpowers/plans/2026-08-13-screens-clock-sleep.md: the
-- global system clock -- read, edited and saved with the seven hardware
-- buttons, registered third so MENU cycles HOME -> INFO -> SETTINGS ->
-- HOME. The cursor logic (which field is selected, hold-to-repeat) lives
-- in C++ (kf_lua_settings_screen.cpp) and reads the buttons directly, NOT
-- through kf.on_button -- see that file's own header comment for why a
-- shared button registry would let this screen's buttons also fire while
-- Home is showing. This screen only ever DRAWS: on_settings_frame below
-- gets handed the current field/hour/minute/AM-PM/save-result every frame
-- and sets text and colour, nothing else.
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
local save_row = settings_label("SAVE", 16, 140)
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

-- Global, not local -- kf_lua_port_settings_frame() (sdk/lua/kf_lua_
-- port.cpp) calls this by name while Settings is the active screen, its
-- own dedicated entry point for the identical reason on_info_frame() is
-- separate from on_frame(): one screen's per-frame logic must never touch
-- another screen's objects. `saved` is nil (no save attempted since this
-- screen was opened), true, or false.
function on_settings_frame(dt_ms, field, hour, minute, ampm, saved)
    hour_value:set("" .. hour)
    min_value:set(pad2(minute))
    ampm_value:set(ampm)

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
    paint(save_row, "save")
end

-- Screen-agnostic: the main loop (sdl_main.cpp/app_main.cpp) calls this
-- unconditionally, every real frame, regardless of which screen is active
-- -- these three announcements are pet-state observations, not any one
-- screen's pixels, so they keep firing whether the owner is looking at
-- Home, Info or Settings. A screen's own drawing belongs in that screen's
-- OWN entry point instead (on_home_frame/on_info_frame/on_settings_frame
-- above) -- see kf_lua_port.h's own comment on kf_lua_port_home_frame()
-- for why Home's drawing used to live here, and what that cost.
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
