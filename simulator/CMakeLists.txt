## SPDX-License-Identifier: Apache-2.0
## Copyright the Kamiframe contributors.
##
## Desktop HAL backends.
##
## There are two of them, and that is the point. Both link the SAME
## hakoniwaos library, containing the same drawing code, the same arenas and
## the same frame loop. Only the bottom layer differs:
##
##     kamiframe-sim       SDL3 window, keyboard, real-time pacing
##     kamiframe-headless  no window, checksums frames, runs flat out
##
## Swapping the bottom layer by linking different sources is the exact
## mechanism the ESP32 backend will use. Having two working backends now means
## that mechanism is proven long before there is any hardware to prove it on.
##
## This directory is host-only and is not part of the ESP-IDF build.

if(ESP_PLATFORM)
    return()
endif()

# Shared by both backends: pools, logging, entropy, clocks, storage, power,
# assets. Nothing here touches a display or an input device.
set(KF_HOST_COMMON_SRCS
    src/host/host_memory.cpp
    src/host/host_log.cpp
    src/host/host_time.cpp
    src/host/host_entropy.cpp
    src/host/host_storage.cpp
    src/host/host_power.cpp
    src/host/host_assets.cpp
)

add_library(kamiframe_host_common STATIC ${KF_HOST_COMMON_SRCS})
target_include_directories(kamiframe_host_common
    PUBLIC ${CMAKE_CURRENT_LIST_DIR}/src/host)
target_link_libraries(kamiframe_host_common
    PUBLIC hakoniwaos kamiframe_options)

# host_assets.cpp's default asset pack path: an absolute, source-tree-
# relative path baked in at configure time, not a relative "wherever you
# run it from" default -- see that file's own header comment for why. Both
# kamiframe-sim and kamiframe-headless link kamiframe_host_common, so both
# get the same default with no per-target duplication.
#
# A cache variable rather than a literal so a freshly generated pack can be
# previewed without overwriting the checked-in example:
#   cmake -B build -DKF_ASSET_PACK=/abs/path/to/some.kfpack
# Unset, it is exactly the old hardcoded path, so a plain `cmake -B build`
# and dev.sh behave as before.
#
# EXPECT TESTS TO FAIL WHILE THIS POINTS SOMEWHERE ELSE, and do not go
# hunting for the bug. headless_determinism, headless_fullscreen and
# asset_pipeline_check all checksum rendered output or read the pack
# directly, so a different pack legitimately produces different numbers.
# Point it back at the default before trusting a test run:
#   cmake -B build -DKF_ASSET_PACK=${CMAKE_SOURCE_DIR}/examples/hello_sprite/assets.kfpack
set(KF_ASSET_PACK "${CMAKE_SOURCE_DIR}/examples/hello_sprite/assets.kfpack"
    CACHE FILEPATH "Asset pack the host builds mount by default")

target_compile_definitions(kamiframe_host_common PUBLIC
    KF_HOST_DEFAULT_ASSET_PACK_PATH="${KF_ASSET_PACK}"
)

# ---------------------------------------------------------------------------
# Pet session -- owns the live kf_pet_state instance and drives its two
# "ages" (offline fast-forward at boot, live decay per frame). See ADR
# 0016. Lives here, not in hakoniwaos/, for the same reason
# kamiframe_lvgl_port and kamiframe_lua_port do: this is simulator-only
# orchestration around a Core mechanism, not a claim that the ESP32 build
# has it wired up. Built before both the LVGL and Lua port glue below,
# because kamiframe_lvgl_port's pet screen (ADR 0017) and
# kamiframe_lua_port's pet.* binding (ADR 0016) both link against it.
#
# kf_creature_screen.cpp (Task 4 of the pet-screen plan) lives here too,
# alongside kf_pet_session.cpp, rather than inside kamiframe_lvgl_port
# below even though its only current caller (kf_screen_nav.cpp) lives
# there: it has no LVGL dependency of its own -- it draws through kf/
# blit.h straight into the framebuffer -- only a dependency on this
# library's own kf_pet_session.h, and kamiframe_lvgl_port already links
# this library PUBLIC, so putting it here keeps the dependency arrow
# pointing the direction it actually runs (creature screen depended ON,
# never depending on LVGL) instead of the reverse.
# ---------------------------------------------------------------------------
add_library(kamiframe_pet_port STATIC
    src/pet/kf_pet_session.cpp
    src/pet/kf_creature_screen.cpp
)
target_include_directories(kamiframe_pet_port
    PUBLIC ${CMAKE_CURRENT_LIST_DIR}/src/pet)
target_link_libraries(kamiframe_pet_port
    PUBLIC hakoniwaos kamiframe_host_common kamiframe_options)

# ---------------------------------------------------------------------------
# LVGL port glue -- menus and stat displays, see ADR 0013. Its pet screen
# (ADR 0017) is what ties it to kamiframe_pet_port above.
#
# Built unconditionally, same as kamiframe_host_common above: both desktop
# backends link it. Deliberately lives here, not in hakoniwaos/, because this
# slice does not claim LVGL works on the ESP32 build -- see the ADR's "what
# this slice actually builds".
# ---------------------------------------------------------------------------
include(fetch_lvgl)

add_library(kamiframe_lvgl_port STATIC
    src/lvgl/kf_lvgl_pool.cpp
    src/lvgl/kf_lvgl_tick.cpp
    src/lvgl/kf_lvgl_display.cpp
    src/lvgl/kf_lvgl_input.cpp
    src/lvgl/kf_lvgl_pointer.cpp
    src/lvgl/kf_lvgl_port.cpp
    src/lvgl/kf_lvgl_proof_screen.cpp
    src/lvgl/kf_pet_screen.cpp
    src/lvgl/kf_pet_info_screen.cpp
    src/lvgl/kf_screen_nav.cpp
)
target_include_directories(kamiframe_lvgl_port
    PUBLIC ${CMAKE_CURRENT_LIST_DIR}/src/lvgl)
target_link_libraries(kamiframe_lvgl_port
    PUBLIC hakoniwaos lvgl kamiframe_pet_port kamiframe_options)

# Genuinely circular at the symbol level, not a layering mistake: LVGL calls
# kf_lvgl_mem_pool_alloc() (defined in kamiframe_lvgl_port, wired in via
# LV_MEM_POOL_ALLOC in lv_conf.h) once, at lv_init() time, and everything
# else in kamiframe_lvgl_port calls into LVGL. CMake resolves the cycle by
# repeating both archives on the final link line.
target_link_libraries(lvgl PRIVATE kamiframe_lvgl_port)

# ---------------------------------------------------------------------------
# Lua port glue -- the SDK language, see ADR 0014. Its pet.* binding (ADR
# 0016) is what ties it to kamiframe_pet_port above.
#
# Built unconditionally, same as kamiframe_host_common and
# kamiframe_lvgl_port above: both desktop backends link it. Lives here, not
# in hakoniwaos/, for exactly the reason ADR 0013 already gives for LVGL:
# this slice does not claim Lua works on the ESP32 build.
# ---------------------------------------------------------------------------
include(fetch_lua)

add_library(kamiframe_lua_port STATIC
    src/lua/kf_lua_alloc.cpp
    src/lua/kf_lua_port.cpp
)
target_include_directories(kamiframe_lua_port
    PUBLIC ${CMAKE_CURRENT_LIST_DIR}/src/lua)
target_link_libraries(kamiframe_lua_port
    PUBLIC hakoniwaos lua_core kamiframe_pet_port kamiframe_options)

# ---------------------------------------------------------------------------
# SDL3 simulator
# ---------------------------------------------------------------------------
if(KAMIFRAME_BUILD_SIMULATOR)
    include(fetch_sdl)

    add_executable(kamiframe-sim
        src/sdl/sdl_debug_window.cpp
        src/sdl/sdl_display.cpp
        src/sdl/sdl_input.cpp
        src/sdl/sdl_main.cpp
    )
    target_link_libraries(kamiframe-sim
        PRIVATE hakoniwaos kamiframe_host_common kamiframe_lvgl_port
                kamiframe_lua_port kamiframe_pet_port kamiframe_options
                SDL3::SDL3)
    set_target_properties(kamiframe-sim PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
endif()

# ---------------------------------------------------------------------------
# Headless runner
# ---------------------------------------------------------------------------
if(KAMIFRAME_BUILD_HEADLESS)
    add_executable(kamiframe-headless
        src/headless/headless_display.cpp
        src/headless/headless_input.cpp
        src/headless/headless_main.cpp
    )
    target_include_directories(kamiframe-headless
        PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src/headless)
    target_link_libraries(kamiframe-headless
        PRIVATE hakoniwaos kamiframe_host_common kamiframe_lvgl_port
                kamiframe_lua_port kamiframe_pet_port kamiframe_options)
    set_target_properties(kamiframe-headless PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

    # An absolute, source-tree-relative path to the demo creature pack, baked
    # in at configure time -- the same reasoning KF_HOST_DEFAULT_ASSET_PACK_
    # PATH above gets, and for the same reason: a relative path would break
    # the moment ctest's working directory is not the repo root, which is not
    # a promise it makes. PRIVATE and on this target alone, not KF_ASSET_PACK
    # or kamiframe_host_common's shared define -- this is not a new default
    # pack, only run_creature_screen_sprite_check() (headless_main.cpp) ever
    # reads this macro, via a runtime kf_host_assets_set_pack_path() override
    # it restores before returning, exactly the mechanism host_assets.h's own
    # header comment describes and the same one kamiframe-sim's `--pack` flag
    # already uses. Every other test keeps mounting the untouched default.
    target_compile_definitions(kamiframe-headless PRIVATE
        KF_CREATURE_DEMO_PACK_PATH="${CMAKE_SOURCE_DIR}/examples/creature_demo/assets.kfpack"
    )

    # Same reasoning, same mechanism, one entry above: run_indexed_asset_
    # check() (headless_main.cpp) is the only reader, via the same runtime
    # kf_host_assets_set_pack_path() override it restores before returning.
    # examples/hello_sprite/assets_indexed.kfpack is a permanent, checked-in
    # fixture -- ASSET_TYPE_SPRITE_INDEXED's 'test_sprite' (1 frame) and
    # 'test_sprite_anim' (3 frames) -- not the default pack every other test
    # keeps mounting untouched.
    target_compile_definitions(kamiframe-headless PRIVATE
        KF_INDEXED_FIXTURE_PACK_PATH="${CMAKE_SOURCE_DIR}/examples/hello_sprite/assets_indexed.kfpack"
    )

    # Same reasoning, same mechanism, two entries above: run_indexed_asset_
    # check() and run_indexed_blit_check() (headless_main.cpp) both need an
    # RGB565 test_sprite to compare the indexed decode/blit against. Before
    # the animated-indexed-sprites plan's Task 3, they read that RGB565
    # sprite from "the default pack" (examples/hello_sprite/assets.kfpack)
    # directly, with no override. Task 3 converted the default pack itself
    # to indexed, so that reference moved to its own permanent, checked-in
    # fixture -- ASSET_TYPE_SPRITE's 'test_sprite', the exact bytes the
    # default pack used to hold -- read via the same runtime
    # kf_host_assets_set_pack_path() override, restored before returning.
    target_compile_definitions(kamiframe-headless PRIVATE
        KF_RGB565_FIXTURE_PACK_PATH="${CMAKE_SOURCE_DIR}/examples/hello_sprite/assets_rgb565.kfpack"
    )

    # Golden-frame test. Runs the real firmware for 300 frames and checks the
    # rendered output byte for byte, via an FNV-1a hash of every presented
    # framebuffer.
    #
    # If this fails, rendering changed. That is often intentional. When it is,
    # run the binary by hand, take the checksum it prints, and update the
    # value here in the same commit as the change that caused it, so the diff
    # says "these pixels moved on purpose".
    #
    # Updated 2026-08-04 alongside headless_input.cpp's switch to a simulated
    # per-frame clock (see ADR 0011): the old values were only ever correct
    # by luck, produced by real elapsed host time driving core's button
    # debounce. That made the scripted button-holds clear the 8ms debounce on
    # a frame that depended on host speed, not on the script -- deterministic
    # on an idle machine, and provably not otherwise (see ADR 0011). These
    # values are what a correctly-paced 30fps run has always been meant to
    # produce, verified deterministic across 30+ runs under artificial CPU
    # load.
    set(KAMIFRAME_GOLDEN_CHECKSUM        "2aceae654b21ca1b")
    set(KAMIFRAME_GOLDEN_CHECKSUM_STRESS "3fbdcabfba49e9ef")

    add_test(NAME headless_determinism
             COMMAND kamiframe-headless --frames 300 --seed 0x5EEDCAFE
                     --expect-checksum ${KAMIFRAME_GOLDEN_CHECKSUM})

    # Guards the dirty-rectangle behaviour. The demo redraws two small
    # rectangles per frame, so the mean should sit in the low single digits.
    # If a change starts redrawing the whole screen this fails, which on the
    # device is the difference between 30fps and 60.
    add_test(NAME headless_dirty_area
             COMMAND kamiframe-headless --frames 300 --seed 0x5EEDCAFE
                     --max-dirty-percent 12)

    # The opposite case, and the one that answers "can the device animate the
    # whole screen?". A scrolling tilemap under 12 sprites, every pixel
    # redrawn every frame, dirty area 100%. It is expected to be expensive;
    # the test is that it stays deterministic and that the budget report keeps
    # telling the truth about what it would cost on hardware.
    add_test(NAME headless_fullscreen
             COMMAND kamiframe-headless --frames 300 --seed 0x5EEDCAFE --stress
                     --expect-checksum ${KAMIFRAME_GOLDEN_CHECKSUM_STRESS})

    # Proves the actual hardware-purchase trigger: save state survives, and
    # offline ageing works, deterministically. See
    # docs/architecture/adr-0012-storage-and-power.md. Bypasses the frame
    # loop entirely -- this is a storage/power check, not a rendering one.
    add_test(NAME storage_power_check
             COMMAND kamiframe-headless --verify-storage-power)

    # Proves the LVGL port glue renders deterministically frame to frame,
    # the same property headless_determinism proves for the custom engine.
    # See ADR 0013. Bypasses kf_app_frame() entirely -- this exercises LVGL
    # directly, not the placeholder demo, and does not touch the golden
    # checksums above.
    #
    # KAMIFRAME_LVGL_GOLDEN_CHECKSUM: an FNV-1a hash of the framebuffer after
    # the proof screen has rendered, using headless's synthetic per-frame
    # clock so it is host-speed-independent (the same reasoning as ADR 0011).
    # If this fails because the proof screen deliberately changed, run
    # `kamiframe-headless --verify-lvgl`, take the checksum it prints, and
    # update the value here in the same commit.
    set(KAMIFRAME_LVGL_GOLDEN_CHECKSUM "70c00d13cdfb6d97")

    add_test(NAME lvgl_determinism_check
             COMMAND kamiframe-headless --verify-lvgl
                     --expect-checksum ${KAMIFRAME_LVGL_GOLDEN_CHECKSUM})

    # Proves the Lua port glue -- the sandboxed VM, the arena-backed
    # allocator's free/realloc path under real churn (see
    # kf_lua_alloc.h and kf_lua_proof_script.h), and both directions of
    # the kf.* binding -- behave identically frame to frame. See
    # ADR 0014. Bypasses kf_app_frame() entirely, the same way
    # lvgl_determinism_check bypasses it for LVGL: this exercises Lua
    # directly, not the placeholder demo.
    #
    # No golden hash here, unlike the LVGL and rendering checks above:
    # the proof script's own arithmetic makes `total` after N frames
    # exactly 32*N, an invariant --verify-lua checks directly rather than
    # via a checksum that would need updating every time the script's
    # wording changed. See headless_main.cpp's run_lua_check().
    add_test(NAME lua_determinism_check
             COMMAND kamiframe-headless --verify-lua --frames 300)

    # Proves the pet simulation framework's decay maths and, most
    # importantly, its offline fast-forward mechanism: save, sleep (via
    # kf_power_deep_sleep_until(), the same instant-fast-forward
    # storage_power_check above already proves), reload, and confirm the
    # result is identical to one direct kf_pet_advance() call with the same
    # elapsed seconds. See ADR 0015 and headless_main.cpp's
    # run_pet_check().
    add_test(NAME pet_offline_ageing_check
             COMMAND kamiframe-headless --verify-pet)

    # Proves the life-stages/evolution mechanic (ADR 0021): the egg's
    # no-decay guarantee, a single kf_pet_advance() call correctly crossing
    # all four real stage boundaries at once, the care-integral accumulator
    # resetting cleanly at each transition, select_branch()'s equal-band
    # mapping landing in the expected band for known care scores, Adult
    # being terminal, the v2 save format round-tripping every new field,
    # and the version-incompatible-save fallback bug fixed alongside this
    # work. See headless_main.cpp's run_pet_stage_check().
    add_test(NAME pet_stage_evolution_check
             COMMAND kamiframe-headless --verify-pet-stage)

    # Proves the evolution tree's SHAPE directly: four verb families, with
    # an uneven adult count per family (Cut 2, Hold 3, Mark 3, Go 1), per the
    # character bible's sections 6/7 and
    # docs/superpowers/plans/2026-08-09-evolution-tree-reconciliation.md.
    # Asserted against the bible's numbers rather than trusted as a passing
    # constant, so a future roster change (the bible's section 11 says Go and
    # Cut are still short) fails this test first. See headless_main.cpp's
    # run_evolution_tree_shape_check().
    add_test(NAME evolution_tree_shape_check
             COMMAND kamiframe-headless --verify-tree-shape)

    # Proves the never-interacted path to Hokorimaru (character bible section
    # 8): a creature that has never received a single care action becomes the
    # dust form at the Child->Teen transition, regardless of what the
    # care-quality average would otherwise say -- deliberately a different
    # condition from every other branch. A creature touched even once lands
    # in an ordinary verb family instead. See headless_main.cpp's
    # run_hokorimaru_check().
    add_test(NAME hokorimaru_check
             COMMAND kamiframe-headless --verify-hokorimaru)

    # Proves the personality-traits mechanic (ADR 0023): base_trait rolled
    # once via kf_rng_below() and fixed for the pet's whole life, the three
    # care-derived accumulators building whole-life rather than resetting
    # at a stage transition, the periodic-halving EMA math itself,
    # kf_pet_dominant_care_trait()'s band selection (including its tie-
    # break default), and the v3 save format round-tripping every new
    # field. See headless_main.cpp's run_pet_personality_check().
    add_test(NAME pet_personality_check
             COMMAND kamiframe-headless --verify-pet-personality)

    # Proves poops (a count, not a list) accumulate on a timer, saturate at
    # KF_PET_MAX_POOPS rather than growing forever, are cleared by
    # kf_pet_clean(), and that feeding shortens the wait for the next one.
    # See headless_main.cpp's run_pet_mess_check() and the care-loop spec's
    # section 4.
    add_test(NAME pet_mess_check
             COMMAND kamiframe-headless --verify-mess)

    # Proves dirtiness (the continuous half of mess) rises with time, rises
    # faster while poops are waiting, and is reset by kf_pet_clean(). See
    # headless_main.cpp's run_pet_dirtiness_check().
    add_test(NAME pet_dirtiness_check
             COMMAND kamiframe-headless --verify-dirtiness)

    # Proves sickness (the care-loop spec's section 7): sustained neglect
    # (a need at or below neglect_need_mp, too many poops waiting, or
    # dirtiness past the stink threshold) accumulates into neglect_seconds,
    # crossing sickness_onset_seconds sets sick, and only returning to zero
    # clears it -- a single round of care does not cure it on the spot.
    # A creature that has never received a single care action is exempt
    # (the dust path, not the sick path). See headless_main.cpp's
    # run_pet_sickness_check().
    add_test(NAME pet_sickness_check
             COMMAND kamiframe-headless --verify-pet-sickness)

    # Proves death: sustained critical neglect (neglect_seconds reaching
    # sickness_death_seconds) is fatal, and is heavily telegraphed -- a real
    # window between falling ill and dying, not a surprise. Once dead,
    # nothing decays, no stage transition happens, and care actions do not
    # even count as care. sickness_death_seconds == 0 is the off switch
    # (the same sentinel shape poop_interval_seconds == 0 uses for "no
    # mess") and a creature that has never been touched at all does not die
    # of it either -- see headless_main.cpp's run_pet_death_check().
    add_test(NAME pet_death_check
             COMMAND kamiframe-headless --verify-pet-death)

    # Proves the care-variation preference table (docs/superpowers/plans/
    # 2026-08-09-care-variations.md) is exhaustively well-shaped: all six
    # traits x four actions x three variations, every trait has exactly one
    # liked/neutral/disliked variation of every action, no two traits agree
    # on everything, and out-of-range input degrades to neutral rather than
    # reading past the table. See headless_main.cpp's
    # run_pet_preferences_check().
    add_test(NAME pet_preferences_check
             COMMAND kamiframe-headless --verify-pet-preferences)

    # Proves the variation argument actually reaches all four care actions
    # and changes what they're worth: the variation this creature's
    # base_trait likes restores more than the one it dislikes, the reaction
    # is recorded on the state (both which action and how it went), and an
    # out-of-range variation degrades to neutral rather than crashing or
    # becoming a secret favourite. See headless_main.cpp's
    # run_pet_care_variation_check().
    add_test(NAME pet_care_variation_check
             COMMAND kamiframe-headless --verify-pet-care-variation)

    # Investigates a real bug report ("it evolved from baby to teen, then
    # it also didn't go past teen to adult yet and I couldn't drag the
    # timeline there") against the actual session/seek surface a running
    # build and the SDL debug window use, not just Core's stage-transition
    # arithmetic in isolation (already covered by pet_stage_evolution_check
    # above). Proves an entirely uncared-for pet (no way to feed/play/rest/
    # bathe it existed before Task 6) dies of sustained neglect before ever
    # reaching Adult, that kf_pet_session_debug_seek() correctly leaves a
    # dead pet exactly where it died however far right it is dragged (dead
    # is terminal), and -- by running the SAME seek() call against a
    # genuinely cared-for pet, which DOES reach Adult -- that the timeline/
    # seek mechanism itself is not the defect. See headless_main.cpp's
    # run_pet_adult_reachability_check().
    add_test(NAME pet_adult_reachability_check
             COMMAND kamiframe-headless --verify-pet-adult-reachability)

    # Proves the debug stage-jump (Chris's request: a button that spawns the
    # creature at full care stats at each life stage, so looking at a
    # stage's art and care behaviour does not require nursing a pet through
    # every real stage first): lands on the requested stage, alive, not
    # sick, needs full, at the very start of it, for all five stages; an
    # explicit teen_form/adult_branch index is honoured exactly wherever the
    # stage has reached that branch point and clamps to 0 (same as "unset")
    # when out of range; the two indices stay at their not-decided-yet
    # default when the target stage has not reached their branch point yet;
    # and a genuinely dead, sick, neglected pet (killed by a real
    # kf_pet_advance() run) comes back alive and clean after a jump, without
    # kf_pet_advance()'s own death-by-neglect rule having been touched. See
    # headless_main.cpp's run_pet_debug_jump_check() and kf_pet_session.h's
    # kf_pet_session_debug_jump_to_stage().
    add_test(NAME pet_debug_jump_check
             COMMAND kamiframe-headless --verify-pet-debug-jump)

    # Proves the pet.* Lua binding (ADR 0016) actually reads and mutates
    # the SAME live kf_pet_session state Core computes, not a disconnected
    # copy: pet.hunger() read from Lua matches kf_pet_session_state() read
    # directly from C++, and pet.feed()/play()/rest() called from Lua
    # bring all three needs back to exactly max, checked against the live
    # C++ state independently of what the script itself reported. See
    # headless_main.cpp's run_lua_pet_check().
    add_test(NAME lua_pet_binding_check
             COMMAND kamiframe-headless --verify-lua-pet)

    # Proves the pet screen (ADR 0017) renders deterministically, the same
    # property lvgl_determinism_check proves for the (still separate, still
    # in place) proof screen. See headless_main.cpp's run_pet_screen_check()
    # and kf_pet_screen.cpp.
    #
    # KAMIFRAME_PET_SCREEN_GOLDEN_CHECKSUM: an FNV-1a hash of the framebuffer
    # after the pet screen has rendered a fresh pet's starting values, using
    # headless's synthetic per-frame clock so it is host-speed-independent
    # (same reasoning as ADR 0011). If this fails because the screen
    # deliberately changed, run `kamiframe-headless --verify-pet-screen`,
    # take the checksum it prints, and update the value here in the same
    # commit as the change that caused it.
    #
    # Updated four times now -- see docs/architecture/adr-0017-pet-screen.md
    # 's "Found after delivery" section for the first two (2026-08-04):
    # a focused-button style, then a lv_group_add_obj() focus-corruption
    # fix. Third and fourth updates (2026-08-05, ADR 0021):
    # kf_pet_screen.cpp now draws a placeholder stage/branch "blob" (a
    # plain LVGL circle, see kf_pet_screen.h's header comment) in the
    # screen's top-right corner -- new rendered pixels a fresh pet's
    # screen did not have before, an intentional content change, not a
    # regression. The fourth update fixed the blob's caption label
    # position: an initial lv_obj_align_to() call anchored it to the
    # blob's default ~100px LVGL size (before the first real resize had
    # run), landing it far from the blob -- caught by an actual Xvfb
    # screenshot, not assumed; see kf_pet_screen.cpp's own comment on the
    # fixed-offset replacement. Deterministic across repeated runs before
    # being locked in, same discipline as every earlier value here.
    set(KAMIFRAME_PET_SCREEN_GOLDEN_CHECKSUM "132458f0171a2c0b")

    add_test(NAME pet_screen_check
             COMMAND kamiframe-headless --verify-pet-screen
                     --expect-checksum ${KAMIFRAME_PET_SCREEN_GOLDEN_CHECKSUM})

    # Proves kf_creature_pose_for()'s precedence (dead beats sick beats a
    # held reaction beats neutral) and proves the reaction-hold window
    # actually gates the sticky last_reaction field, rather than reading it
    # straight through. See headless_main.cpp's run_creature_pose_check()
    # and hakoniwaos/include/kf/creature.h.
    add_test(NAME creature_pose_check
             COMMAND kamiframe-headless --verify-creature-pose)

    # Proves the wander (Task 3): the same seed reproduces the same walk,
    # the creature never leaves the field across many seeds and a long run,
    # and it actually moves rather than trivially passing the first two
    # checks by standing still. See headless_main.cpp's
    # run_creature_wander_check() and hakoniwaos/include/kf/creature.h's
    # kf_creature_init()/kf_creature_update()/kf_creature_bounds().
    add_test(NAME creature_wander_check
             COMMAND kamiframe-headless --verify-creature-wander)

    # Proves the pet screen's ownership switch (Task 4 of the pet-screen
    # plan) stays inside its dirty-rect budget -- at most 2 rectangles a
    # frame (erase the creature's previous position, draw its new one) and
    # a generous byte cap on top of that -- and that the budget is not met
    # vacuously by drawing nothing: the default asset pack has no creature
    # art yet, so this also pins down the placeholder-rectangle fallback
    # that keeps the screen visibly (if not correctly) drawing something
    # regardless. See headless_main.cpp's run_creature_screen_check() and
    # simulator/src/pet/kf_creature_screen.h.
    add_test(NAME creature_screen_check
             COMMAND kamiframe-headless --verify-creature-screen)

    # Task 6: the five care buttons the creature screen now reads directly
    # (kf_app_buttons_pressed(), the same debounced edge state kf_screen_
    # nav.cpp already uses for MENU/B) -- the only way left to move
    # pet->care_actions_taken in a running build besides the Lua binding,
    # now that the old LVGL Home holding Feed/Play is unreachable (Task 4).
    # Proves each button reaches its own kf_pet_session_* wrapper, that
    # each action's variation counter cycles 0->1->2->0 INDEPENDENTLY of
    # the other three (not one shared counter), that flush behaves per its
    # own no-variation/no-reaction contract, and that a button-triggered
    # action starts the same reaction-hold countdown a Lua-triggered one
    # already did. See headless_main.cpp's run_creature_screen_input_check()
    # and simulator/src/pet/kf_creature_screen.cpp's handle_care_buttons().
    add_test(NAME creature_screen_input_check
             COMMAND kamiframe-headless --verify-creature-screen-input)

    # Closes the coverage gap resolve_sprite() (kf_creature_screen.cpp) used
    # to record in its own comment: every OTHER test here mounts the
    # asset-less default pack, so kf_assets_get() always returns nullptr and
    # only the placeholder-colour branch ever ran -- kf_blit(),
    # kf_blit_mirrored(), and the "_w_"-not-found -> mirrored "_e_" fallback
    # were never exercised through the real drawing path. This check points
    # the runtime pack override at examples/creature_demo/assets.kfpack (the
    # demo roster from the art-naming task: egg_idle_{s,e,n}_01, no "_w_"),
    # drives kf_creature_screen_frame() through S/E/N/W with the facing
    # forced via kf_creature_screen_debug_set_direction(), and asserts real
    # sprite pixels are drawn (not the placeholder colour) and that the W
    # case is the E sprite's pixels mirrored column-for-column, not merely
    # "something non-background". Does not touch KF_ASSET_PACK or the
    # golden-checksum tests above -- see headless_main.cpp's
    # run_creature_screen_sprite_check() and this file's own
    # KF_CREATURE_DEMO_PACK_PATH comment above.
    add_test(NAME creature_screen_sprite_check
             COMMAND kamiframe-headless --verify-creature-screen-sprites)

    # The egg-and-guide task (project-owner feedback: "an egg that strolls
    # across the field is wrong"): the egg no longer wanders (kf_creature_
    # update() is simply never called for it -- see kf_creature_screen.cpp's
    # own comment on why that gate lives in the screen, not in Core), it
    # bobs gently in place instead (egg_bob_offset_y(), pure integer
    # triangle wave, no trig, no float), and the on-screen care-button guide
    # painted once by kf_creature_screen_enter() is never touched again by
    # the per-frame path. See headless_main.cpp's
    # run_creature_screen_egg_check() for all three claims measured
    # together against one fixture.
    add_test(NAME creature_screen_egg_check
             COMMAND kamiframe-headless --verify-creature-screen-egg)

    # The death scene (spec: "Death without a player holds on the last
    # creature's scene" -- docs/superpowers/specs/2026-08-09-core-care-loop-
    # design.md): once pet->dead, the screen special-cases a single shrine
    # sprite (or its placeholder, until the art pipeline ships
    # shrine_idle_s_01) centred in the field, and stops drawing anything
    # else -- no wander, no per-frame redraw once painted. See
    # headless_main.cpp's run_creature_screen_death_check().
    add_test(NAME creature_screen_death_check
             COMMAND kamiframe-headless --verify-creature-screen-death)

    # Closes a testing gap a review of the stage-jump debug lever found:
    # every check above either jumps stage without ever rendering a frame
    # (pet_debug_jump_check) or renders frames without ever jumping (every
    # other creature_screen_* check here). Two of that review's three
    # Important defects lived exactly at the seam between the two --
    # kf_creature_screen.cpp's per-frame code assumed a dead pet stays
    # dead, and assumed an egg has always been an egg, both of which
    # kf_pet_session_debug_jump_to_stage() makes routine. This drives both
    # seams on one fixture: jump to a stage after a death and render (the
    # shrine must not leak over the revived, living creature), then jump
    # back to Egg from a creature that had wandered and render (the egg
    # must re-centre, not draw off-centre or past this screen's own
    # y=[0,260) invariant). See headless_main.cpp's
    # run_creature_screen_debug_jump_check().
    add_test(NAME creature_screen_debug_jump_check
             COMMAND kamiframe-headless
                     --verify-creature-screen-debug-jump)

    # Proves decay is per-stage, not uniform (the demand-curve plan): a baby
    # empties far faster than an adult over the same elapsed time. See
    # headless_main.cpp's run_pet_demand_curve_check() and
    # docs/superpowers/plans/2026-08-09-demand-curve.md.
    add_test(NAME pet_demand_curve_check
             COMMAND kamiframe-headless --verify-demand-curve)

    # Proves the menu/screen navigation mechanism (ADR 0022): Home is
    # active at boot, kf_screen_nav_debug_advance() (the same effect as a
    # real MENU press) moves to Info and its rendering is deterministic,
    # and kf_screen_nav_debug_home() (a B press) returns to Home -- all
    # asserted against kf_screen_nav_debug_index() directly, not inferred
    # from pixels alone. Uses the debug/test entry points rather than
    # driving kf_app_frame()'s real button polling, because headless_
    # input.cpp's kf_headless_script() is one fixed, frame-indexed script
    # shared by every check that goes through kf_app_frame() (headless_
    # determinism among them) -- adding a MENU window to it would change
    # what those unrelated, already-locked golden checksums see too. See
    # kf_screen_nav.h's own header comment on why these entry points exist.
    #
    # KAMIFRAME_SCREEN_NAV_GOLDEN_CHECKSUM: an FNV-1a hash of the
    # framebuffer after Info has rendered a fresh pet's starting values,
    # same reasoning as KAMIFRAME_PET_SCREEN_GOLDEN_CHECKSUM above. If this
    # fails because Info's screen deliberately changed, run
    # `kamiframe-headless --verify-screen-nav`, take the checksum it
    # prints, and update the value here in the same commit.
    #
    # Updated with ADR 0023 (personality traits): Info grew a "Personality"
    # section showing base_trait (and, once past Egg, the dominant care
    # trait) -- an intentional content addition, not a regression.
    # Deterministic across repeated runs before being locked in, same
    # discipline as every earlier value here -- this check bypasses
    # kf_app_init() (see run_screen_nav_check()), so kf_rng is never
    # reseeded from the entropy HAL and a fresh pet's base_trait comes out
    # identical run to run, the same fixed-default-seed determinism every
    # other kf_rng consumer in a headless-bypass path already relies on.
    set(KAMIFRAME_SCREEN_NAV_GOLDEN_CHECKSUM "ac44bb9819809bea")

    add_test(NAME screen_nav_check
             COMMAND kamiframe-headless --verify-screen-nav
                     --expect-checksum ${KAMIFRAME_SCREEN_NAV_GOLDEN_CHECKSUM})

    # Proves the demo creature script (ADR 0018) -- the actual cartridge
    # sdl_main.cpp loads, not a proof script -- survives a full lifecycle
    # (fresh and full, decayed all the way down through every band to
    # empty, cared back up to full) without ever raising a Lua runtime
    # error. Not a checksum: this script has no rendering surface of its
    # own, it only logs, and kf.log has no capture hook (see
    # headless_main.cpp's run_lua_creature_check()) -- an exact-arithmetic-
    # style check on frame counts and the live C++ pet state instead, the
    # same category of check as lua_pet_binding_check above and for the
    # same reason: this is a logic/robustness question, not a rendering
    # one.
    add_test(NAME lua_creature_check
             COMMAND kamiframe-headless --verify-lua-creature)

    # Proves the asset pipeline (ADR 0033) end to end: kf_assets_init()
    # mounts and parses the checked-in pack
    # (examples/hello_sprite/assets.kfpack, ASSET_TYPE_SPRITE_INDEXED since
    # the animated-indexed-sprites plan's Task 3), kf_assets_get("test_sprite")
    # finds it with the right size/frame-count/colour-key, and -- the real
    # "matches what the packer wrote" proof -- every pixel, expanded by hand
    # through the pack FILE's own palette and index bytes at the offset its
    # directory names, matches the colour the loaded sprite yields. Read
    # independently, not through hakoniwaos/src/assets.cpp's own parser, so
    # this cannot pass by the parser merely agreeing with itself. See
    # headless_main.cpp's run_asset_check().
    add_test(NAME asset_pipeline_check
             COMMAND kamiframe-headless --verify-assets)

    # Proves ASSET_TYPE_SPRITE_INDEXED (the animated-indexed-sprites plan's
    # Task 1) decodes -- palette, frame count, contiguous frames -- and, the
    # point of the whole format, that expanding the indices through the
    # palette reproduces the RGB565 test_sprite byte for byte. Mounts the
    # indexed fixture (examples/hello_sprite/assets_indexed.kfpack) through
    # the RUNTIME override and restores the default, so it never disturbs
    # the golden-checksum tests above. See headless_main.cpp's
    # run_indexed_asset_check().
    add_test(NAME indexed_asset_check
             COMMAND kamiframe-headless --verify-indexed-assets)

    # Proves kf_blit_mirrored() (Task 3b): a fully on-screen mirrored blit
    # reverses the sprite's columns, colour-key skip still works mirrored,
    # and clipping is mirror-aware in both directions -- a sprite hanging
    # off the LEFT screen edge loses its trailing (high-index) source
    # columns rather than its leading ones, and vice versa on the right
    # edge, because that is what a reversed row actually pushes off-screen.
    # Also pins the cost-accounting decision that a mirrored blit, having no
    # memcpy fast path, is always charged to the keyed draw-counter bucket
    # even when the sprite carries no colour key. See headless_main.cpp's
    # run_blit_mirror_check() and hakoniwaos/include/kf/blit.h.
    add_test(NAME blit_mirror_check
             COMMAND kamiframe-headless --verify-blit-mirror)

    # The centrepiece losslessness proof (Task 2): draws the RGB565
    # test_sprite and the indexed one at the same screen position and
    # memcmp's the two resulting framebuffers, byte for byte -- a far
    # stronger check than a hand-written assertion, and the whole reason
    # 8bpp indexing was chosen for this art. Also pins frame addressing
    # (frame k reads k*w*h into the payload), the out-of-range clamp to
    # frame 0 rather than a wrap, mirrored equivalence, and that an indexed
    # blit is charged entirely to the keyed draw-counter bucket. See
    # headless_main.cpp's run_indexed_blit_check() and kf/blit.h's
    # kf_blit_frame()/kf_blit_frame_mirrored().
    add_test(NAME indexed_blit_check
             COMMAND kamiframe-headless --verify-indexed-blit)
endif()
