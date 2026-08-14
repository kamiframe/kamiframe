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
#
# NOT changed to match the device. ports/esp32/main/CMakeLists.txt flashes
# examples/creature_demo/assets.kfpack (94 entries, real art, animated
# idles), not this hello_sprite pack -- see that file's own comment on the
# switch. This default stays put deliberately: the golden checksums
# headless_determinism/headless_fullscreen/asset_pipeline_check are pinned
# against IT, and moving it would break every one of them for no gain here.
# The practical upshot is that from that switch onward, a desktop build run
# with no override shows the placeholder rectangle (kPlaceholderColor,
# kf_creature_screen.cpp) where a real device shows an animated creature --
# not a bug, just this default and the device's flashed pack having
# diverged. To see what the device sees:
#   cmake -B build -DKF_ASSET_PACK=${CMAKE_SOURCE_DIR}/examples/creature_demo/assets.kfpack
# then rebuild -- and see the EXPECT TESTS TO FAIL note just above, because
# that command has the exact same effect this whole comment is about.
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
    src/pet/kf_creature_presenter.cpp
    src/pet/kf_home_screen_input.cpp
)
target_include_directories(kamiframe_pet_port
    PUBLIC ${CMAKE_CURRENT_LIST_DIR}/src/pet)
target_link_libraries(kamiframe_pet_port
    PUBLIC hakoniwaos kamiframe_host_common kamiframe_options)

# ---------------------------------------------------------------------------
# LVGL port glue -- menus and stat displays, see ADR 0013.
#
# ADR 0045 (Task 2 of the screens/clock/sleep plan): built only when
# KF_ENABLE_LVGL is ON (default OFF). Info -- the screen that used to be this
# library's own kf_pet_info_screen.cpp -- moved to a kf.screen() group over
# the retained scene (kf/scene.h), the same mechanism Home already used, so a
# default build no longer links LVGL or carves its 256 KB PSRAM arena.
# kf_screen_nav.cpp, kf_lua_home_screen.cpp and kf_error_banner.cpp moved OUT
# of this library that same task, into kamiframe_screen_port below: none of
# the three ever had an LVGL dependency of its own -- they only lived here
# because kf_screen_nav.cpp used to #include <lvgl.h> for Info's
# lv_screen_load()/lv_obj_invalidate() calls. What remains here
# (kf_pet_screen.cpp, kf_lvgl_proof_screen.cpp and the kf_lvgl_*.cpp port
# glue itself) is real LVGL code, kept building and passing its own two
# golden-checksum tests under -DKF_ENABLE_LVGL=ON so CLAUDE.md's "LVGL vs a
# custom sprite engine" evaluation stays available whenever Chris wants it --
# see docs/architecture/adr-0045-info-screen-in- lua.md.
# ---------------------------------------------------------------------------
if(KF_ENABLE_LVGL)
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
    )
    target_include_directories(kamiframe_lvgl_port
        PUBLIC ${CMAKE_CURRENT_LIST_DIR}/src/lvgl)
    target_link_libraries(kamiframe_lvgl_port
        PUBLIC hakoniwaos lvgl kamiframe_pet_port kamiframe_options)

    # Genuinely circular at the symbol level, not a layering mistake: LVGL
    # calls kf_lvgl_mem_pool_alloc() (defined in kamiframe_lvgl_port, wired
    # in via LV_MEM_POOL_ALLOC in lv_conf.h) once, at lv_init() time, and
    # everything else in kamiframe_lvgl_port calls into LVGL. CMake resolves
    # the cycle by repeating both archives on the final link line.
    target_link_libraries(lvgl PRIVATE kamiframe_lvgl_port)
endif()

# ---------------------------------------------------------------------------
# Lua port glue -- the SDK language, see ADR 0014. Its pet.* binding (ADR
# 0016) is what ties it to kamiframe_pet_port above.
#
# Built unconditionally, same as kamiframe_host_common and
# kamiframe_lvgl_port above: both desktop backends link it. Lives in
# ../sdk/lua, not here and not in hakoniwaos/: it IS wired into
# ports/esp32/ (ADR 0028 -- see sdk/lua/kf_lua_port.h), which is exactly why
# it does not belong in hakoniwaos/ core -- Lua is SDK/game glue on top of
# Core's HAL and arenas, present on both desktop and device, not part of
# Core itself. ports/esp32/main/CMakeLists.txt compiles the same two files
# from that same directory directly, by relative path, the same one-
# canonical-copy pattern it already uses for kf_pet_session.cpp.
# ---------------------------------------------------------------------------
include(fetch_lua)

add_library(kamiframe_lua_port STATIC
    ../sdk/lua/kf_lua_alloc.cpp
    ../sdk/lua/kf_lua_port.cpp
    ../sdk/lua/kf_lua_scene.cpp
)
target_include_directories(kamiframe_lua_port
    PUBLIC ${CMAKE_CURRENT_LIST_DIR}/../sdk/lua)
target_link_libraries(kamiframe_lua_port
    PUBLIC hakoniwaos lua_core kamiframe_pet_port kamiframe_options)

# ---------------------------------------------------------------------------
# Screen navigation -- kf_screen_nav.cpp (ADR 0022/0044), plus the two small
# files that only ever exist to give it something to switch TO under
# KF_HOME_SCREEN=lua (kf_lua_home_screen.cpp, kf_error_banner.cpp). Built
# unconditionally, same as kamiframe_lua_port above: every desktop backend
# needs screen navigation regardless of KF_ENABLE_LVGL, now that Info no
# longer needs LVGL to exist (ADR 0045).
#
# A library of its own, not folded into kamiframe_pet_port: this file needs
# BOTH kamiframe_pet_port (Home's cpp implementation, kf_creature_screen.h)
# AND kamiframe_lua_port (Home's lua implementation, and Info's -- kf_lua_
# port.h/kf_lua_scene.h), and kamiframe_lua_port already depends ON
# kamiframe_pet_port (its pet.* binding needs kf_pet_session.h) -- putting
# these three files inside kamiframe_pet_port would make kamiframe_pet_port
# depend on kamiframe_lua_port too, a straight cycle. This library sits
# above both instead, the same position kamiframe_lvgl_port occupied before
# ADR 0045 moved these files out of it for the identical reason.
#
# kf_frame_loop.cpp (ADR 0058) joined this library for the same dependency
# reason: it needs both kamiframe_pet_port (kf_pet_session.h) and
# kamiframe_lua_port (kf_lua_port.h/kf_lua_scene.h), the exact set this
# library already has. It is the one shared per-frame sequence both
# kamiframe-sim (below) and ports/esp32/main/app_main.cpp drive -- see that
# file's own header comment. ports/esp32/main/CMakeLists.txt compiles the
# identical source by relative path, the same one-canonical-copy pattern
# kf_screen_nav.cpp above and kf_pet_session.cpp already use.
# ---------------------------------------------------------------------------
add_library(kamiframe_screen_port STATIC
    src/pet/kf_screen_nav.cpp
    src/pet/kf_lua_home_screen.cpp
    src/pet/kf_lua_settings_screen.cpp
    src/pet/kf_error_banner.cpp
    src/pet/kf_frame_loop.cpp
    # kf_debug_actions.cpp: the one table of portable debug actions both the
    # SDL debug window and the KFDBG bridge dispatch through. Lives in this
    # library rather than kamiframe_pet_port because its SCREEN action calls
    # kf_screen_nav_debug_advance(), declared right here in kf_screen_nav.h
    # -- the same one-file-one-dependency reasoning kf_frame_loop.cpp above
    # already follows.
    src/pet/kf_debug_actions.cpp
)
target_link_libraries(kamiframe_screen_port
    PUBLIC hakoniwaos kamiframe_pet_port kamiframe_lua_port kamiframe_options)

# kf_frame_loop.cpp's #ifdef KF_ENABLE_LVGL block calls kf_lvgl_port_pump(),
# declared in kamiframe_lvgl_port's own kf_lvgl_port.h -- which pulls in
# <lvgl.h> for lv_group_t, so it needs that target's real INCLUDE_DIRS, not
# just its symbol at final link time. Only under KF_ENABLE_LVGL, same as
# kamiframe_lvgl_port's own definition above (the target does not exist
# otherwise). Not circular: kamiframe_lvgl_port depends on kamiframe_pet_port,
# never on kamiframe_screen_port. This mirrors ports/esp32/main/CMakeLists.txt's
# own conditional REQUIRES kamiframe_lvgl_port, added for the identical reason
# (app_main.cpp's own #include "kf_lvgl_port.h") before this file existed.
if(KF_ENABLE_LVGL)
    target_link_libraries(kamiframe_screen_port PUBLIC kamiframe_lvgl_port)
endif()

# ---------------------------------------------------------------------------
# KF_HOME_SCREEN (Task 5 of the Lua game-layer plan, the Lua game-layer
# plan): which Home implementation a build's interactive app wires up --
# examples/creature_demo/creature.lua declaring it directly through
# kf/scene.h's retained scene ("lua", the default since Task 6 -- ADR 0043)
# or kf_creature_screen.cpp ("cpp", unchanged since Task 4, kept as the
# parity reference and fallback for one release, per the plan). Both are
# ALWAYS compiled, regardless of this setting: the headless parity check
# (kamiframe-headless --verify-screen-parity) drives both halves of the
# comparison from ONE binary, at runtime, independent of which one this flag
# wires up for the INTERACTIVE app. Same CACHE STRING + configure-time
# FATAL_ERROR pattern as ports/esp32/main/CMakeLists.txt's own KF_PANEL, for
# the identical reason: a typo here should read as a command-line mistake,
# not a mysterious preprocessor failure three files away.
#
# kamiframe_screen_port needs KF_HOME_SCREEN_LUA, not kamiframe_lvgl_port
# (which ADR 0045 moved this choice out of entirely): kf_screen_nav.cpp,
# which owns the choice of which Home implementation is wired up, lives
# there now.
set(KF_HOME_SCREEN "lua" CACHE STRING "Home screen implementation: cpp or lua")
if(NOT KF_HOME_SCREEN STREQUAL "cpp" AND NOT KF_HOME_SCREEN STREQUAL "lua")
    message(FATAL_ERROR
        "KF_HOME_SCREEN must be 'cpp' or 'lua', got '${KF_HOME_SCREEN}'")
endif()

if(KF_HOME_SCREEN STREQUAL "lua")
    target_compile_definitions(kamiframe_screen_port PUBLIC KF_HOME_SCREEN_LUA=1)
    # kf_lua_port.cpp's own compile-time default for kf.home_screen_active()
    # -- see that file's own comment on kf_lua_port_set_home_screen_active().
    target_compile_definitions(kamiframe_lua_port PUBLIC KF_HOME_SCREEN_LUA=1)
endif()

# ---------------------------------------------------------------------------
# SDL3 simulator
# ---------------------------------------------------------------------------
if(KAMIFRAME_BUILD_SIMULATOR)
    include(fetch_sdl)

    add_executable(kamiframe-sim
        src/sdl/sdl_audio.cpp
        src/sdl/sdl_debug_window.cpp
        src/sdl/sdl_display.cpp
        src/sdl/sdl_input.cpp
        src/sdl/sdl_main.cpp
    )
    target_link_libraries(kamiframe-sim
        PRIVATE hakoniwaos kamiframe_host_common kamiframe_screen_port
                kamiframe_lua_port kamiframe_pet_port kamiframe_options
                SDL3::SDL3)
    if(KF_ENABLE_LVGL)
        target_link_libraries(kamiframe-sim PRIVATE kamiframe_lvgl_port)
    endif()
    set_target_properties(kamiframe-sim PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
endif()

# ---------------------------------------------------------------------------
# Headless runner
# ---------------------------------------------------------------------------
if(KAMIFRAME_BUILD_HEADLESS)
    add_executable(kamiframe-headless
        src/headless/headless_audio.cpp
        src/headless/headless_display.cpp
        src/headless/headless_input.cpp
        src/headless/headless_main.cpp
    )
    target_include_directories(kamiframe-headless
        PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src/headless)
    target_link_libraries(kamiframe-headless
        PRIVATE hakoniwaos kamiframe_host_common kamiframe_screen_port
                kamiframe_lua_port kamiframe_pet_port kamiframe_options)
    if(KF_ENABLE_LVGL)
        target_link_libraries(kamiframe-headless PRIVATE kamiframe_lvgl_port)
    endif()
    set_target_properties(kamiframe-headless PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

    # An absolute, source-tree-relative path to the demo creature pack, baked
    # in at configure time -- the same reasoning KF_HOST_DEFAULT_ASSET_PACK_
    # PATH above gets, and for the same reason: a relative path would break
    # the moment ctest's working directory is not the repo root, which is not
    # a promise it makes. PRIVATE and on this target alone, not KF_ASSET_PACK
    # or kamiframe_host_common's shared define -- this is not a new default
    # pack. Read via a runtime kf_host_assets_set_pack_path() override each
    # reader restores before returning, exactly the mechanism host_assets.h's
    # own header comment describes and the same one kamiframe-sim's `--pack`
    # flag already uses. Every other test keeps mounting the untouched
    # default. Originally run_creature_screen_sprite_check()'s alone;
    # run_scene_check() (Task 2) and run_lua_draw_check() (Task 3 of
    # the Lua game-layer plan) both read it too
    # now, for egg_idle_s specifically.
    target_compile_definitions(kamiframe-headless PRIVATE
        KF_CREATURE_DEMO_PACK_PATH="${CMAKE_SOURCE_DIR}/examples/creature_demo/assets.kfpack"
    )

    # Task 3 of the Lua game layer plan: an absolute, source-tree-relative
    # path to the minimal-pet example, so run_lua_draw_check() can read the
    # actual checked-in file rather than a copy pasted into this .cpp --
    # "if it does not work verbatim ... one of the two is wrong" only means
    # something if the check runs the real file. Same baked-in-at-configure
    # reasoning as KF_CREATURE_DEMO_PACK_PATH just above.
    target_compile_definitions(kamiframe-headless PRIVATE
        KF_HELLO_PET_SCRIPT_PATH="${CMAKE_SOURCE_DIR}/examples/hello_pet/pet.lua"
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
    # ADR 0045: only registered under -DKF_ENABLE_LVGL=ON (default OFF) --
    # run_lvgl_check() and kamiframe_lvgl_port itself do not exist in the
    # default build's binary at all, so registering this test unconditionally
    # would be a link failure, not a skip.
    #
    # KAMIFRAME_LVGL_GOLDEN_CHECKSUM: an FNV-1a hash of the framebuffer after
    # the proof screen has rendered, using headless's synthetic per-frame
    # clock so it is host-speed-independent (the same reasoning as ADR 0011).
    # If this fails because the proof screen deliberately changed, run
    # `kamiframe-headless --verify-lvgl`, take the checksum it prints, and
    # update the value here in the same commit.
    if(KF_ENABLE_LVGL)
        set(KAMIFRAME_LVGL_GOLDEN_CHECKSUM "70c00d13cdfb6d97")

        add_test(NAME lvgl_determinism_check
                 COMMAND kamiframe-headless --verify-lvgl
                         --expect-checksum ${KAMIFRAME_LVGL_GOLDEN_CHECKSUM})
    endif()

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

    # Proves docs/architecture/adr-0056-pet-save-checkpoints.md: the pet is
    # actually persisted at the real checkpoints a battery-powered device
    # has -- after a care action, at the awake -> asleep transition, and by
    # a dirty-gated periodic safety net -- not only at kf_pet_session_
    # shutdown(), which never runs on ESP32 (ports/esp32/main/app_main.cpp's
    # own comment calls it "unreachable in practice"). Every case
    # deliberately never calls shutdown() before checking what landed on
    # disk, so a checkpoint that silently did nothing would not be masked
    # by it. Includes the load-bearing case: a pet checkpointed by a care
    # action alone, with no explicit save and no shutdown, survives an
    # abrupt "power loss" and reloads correctly aged -- the thing that has
    # never worked on device before this task. See headless_main.cpp's
    # run_pet_save_checkpoints_check().
    add_test(NAME pet_save_checkpoints_check
             COMMAND kamiframe-headless --verify-pet-save-checkpoints)

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
    # the evolution-tree reconciliation plan.
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

    # Proves sleep (the core care-loop design spec's "Sleep, settled", the
    # screens/clock/sleep plan's Task 6, ADR 0048): state->asleep tracks
    # the 22:00-07:00 night window live as the clock crosses it, an egg
    # never sleeps, the neglect clock pauses while asleep but the needs
    # keep decaying, the waking-fraction (15/24) threshold compression
    # pinned to its exact value, and the v9 save format including refusing
    # a v8 save. pet_offline_ageing_check (--verify-pet) separately proves
    # requirement 1 (last_advanced tracks live play) and requirement 8
    # (offline
    # analytic fast-forward across a night); hokorimaru_check proves
    # requirement 7 unchanged. See headless_main.cpp's run_pet_sleep_check().
    add_test(NAME pet_sleep_check
             COMMAND kamiframe-headless --verify-sleep)

    # The wiring behind the debug window's Drowsy/Bedtime/Morning buttons:
    # kf_pet_session_debug_set_clock() has to move the HAL wall clock AND
    # Core's last_advanced together, because they are separate clocks that
    # nothing else keeps in step (wall time enters Core only at load; after
    # that the session ticks from the MONOTONIC clock). Moving only one
    # leaves Lua showing 22:00 while Core believes it is mid-afternoon, and
    # the creature stays awake -- indistinguishable from a broken night
    # window. pet_sleep_check already covers Core's own night arithmetic;
    # this covers the wiring, which is the part that can silently do
    # nothing. See headless_main.cpp's run_clock_jump_check().
    add_test(NAME clock_jump_check
             COMMAND kamiframe-headless --verify-clock-jump)

    # Proves the care-variation preference table (the care-variations plan)
    # is exhaustively well-shaped: all six traits x four actions x three
    # variations, every trait has exactly one liked/neutral/disliked
    # variation of every action, no two traits agree on everything, and
    # out-of-range input degrades to neutral rather than reading past the
    # table. See headless_main.cpp's run_pet_preferences_check().
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
    # ADR 0045: only registered under -DKF_ENABLE_LVGL=ON, same reasoning as
    # lvgl_determinism_check above -- kf_pet_screen.cpp (kamiframe_lvgl_port)
    # does not exist in the default build's binary. kf_pet_screen.cpp is NOT
    # deleted by that task: it survives as this test's subject specifically
    # so the file and this golden checksum stay available together. Deleting
    # it and retiring this test is a real, defensible follow-up and it is
    # Chris's call, not ADR 0045's -- CLAUDE.md names LVGL a deliberate
    # later evaluation.
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
    if(KF_ENABLE_LVGL)
        set(KAMIFRAME_PET_SCREEN_GOLDEN_CHECKSUM "132458f0171a2c0b")

        add_test(NAME pet_screen_check
                 COMMAND kamiframe-headless --verify-pet-screen
                         --expect-checksum ${KAMIFRAME_PET_SCREEN_GOLDEN_CHECKSUM})
    endif()

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
    # creature's scene" -- the core care-loop design spec): once
    # pet->dead, the screen special-cases a single shrine sprite (or its
    # placeholder) centred in the field, and stops drawing anything else
    # -- no wander, no per-frame redraw once painted. This test mounts no
    # pack override, so it runs against the checked-in default
    # (examples/hello_sprite/assets.kfpack), which does not carry
    # shrine_idle_s -- the placeholder branch is genuinely what this test
    # exercises, not a stand-in for art that does not exist yet: the art
    # pipeline has shipped shrine_idle_s since, it just lives in examples/
    # creature_demo/assets.kfpack (entry 34), a pack this test does not
    # mount. See headless_main.cpp's run_creature_screen_death_check().
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

    # Task 9 of the hardware bring-up plan: the stats band the owner asked
    # for after playing the game on real hardware ("I also can't see the
    # pet's stats anywhere now like how hungry/tired etc"). Proves the band
    # is really painted, costs nothing extra against the creature's own
    # <=2u dirty-rect budget while no need is changing, and DOES redraw --
    # exactly the bar that changed -- the moment a need's quantised bar
    # width actually moves. See headless_main.cpp's
    # run_creature_screen_stats_check() and this task's own report.
    add_test(NAME creature_screen_stats_check
             COMMAND kamiframe-headless --verify-creature-screen-stats)

    # Proves decay is per-stage, not uniform (the demand-curve plan): a baby
    # empties far faster than an adult over the same elapsed time. See
    # headless_main.cpp's run_pet_demand_curve_check() and
    # the demand-curve plan.
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
    #
    # Updated again with ADR 0045 (Task 2 of the screens/clock/sleep
    # plan): Info is now the same blitter that draws the creature,
    # not LVGL -- different font, different pixel
    # layout, uppercase text throughout (the bitmap font has no lowercase;
    # LVGL's did). Both a before-and-after PNG and the reasoning for every
    # re-baselined golden constant this project has ever moved live in
    # docs/architecture/adr-0045-info-screen-in-lua.md -- this is the ONLY
    # circumstance in which this project re-baselines one.
    set(KAMIFRAME_SCREEN_NAV_GOLDEN_CHECKSUM "ca97615c1cd56b73")

    add_test(NAME screen_nav_check
             COMMAND kamiframe-headless --verify-screen-nav
                     --expect-checksum ${KAMIFRAME_SCREEN_NAV_GOLDEN_CHECKSUM})

    # Proves the demo creature script (ADR 0018) -- the actual cartridge
    # sdl_main.cpp loads, not a proof script -- survives a full lifecycle
    # (fresh and full, decayed all the way down through every band to
    # empty, cared back up to full) without ever raising a Lua runtime
    # error. Not a checksum: run_lua_creature_check() (headless_main.cpp)
    # calls kf_lua_port_init()/kf_lua_port_frame() directly and never
    # brings up kf_screen_nav_init(), so no screen is declared or
    # committed here -- this deliberately isolates the script's on_frame()
    # logic from its Home-screen rendering (which it DOES own under
    # KF_HOME_SCREEN=lua in the real app; see run_screen_nav_check()
    # above for the checksum that covers that). kf.log has no capture hook
    # either, so what this check has to work with is an exact-arithmetic-
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

    # Task 6 of the animated-indexed-sprites plan, the payoff the other five
    # were groundwork for: the animation cursor (kf_anim, hakoniwaos/include/
    # kf/creature.h) advances on its own ~10fps clock independent of the
    # display's, carries its accumulator's remainder rather than drifting,
    # wraps back to 0 rather than reading past a shorter animation's end, and
    # -- the load-bearing claim -- costs no extra dirty rectangles, because
    # kf_creature_screen_frame() already erases and redraws the creature
    # every frame regardless of whether it moved. Also proves the cursor
    # against the real 3-frame fixture (examples/hello_sprite/
    # assets_indexed.kfpack's test_sprite_anim), not just a synthetic frame
    # count, so this fails if the wrap logic and the fixture's real
    # frame_count ever disagree. See headless_main.cpp's
    # run_creature_anim_check().
    add_test(NAME creature_anim_check
             COMMAND kamiframe-headless --verify-creature-anim)

    # Task 1 of the hardware bring-up plan: proves keyed_pixels and
    # dirty_rect_count are not structurally zero on the KF_DEMO_NONE path a
    # real device runs -- before this task, kf_draw_counters_reset() sat at
    # the TOP of kf_app_frame(), inside a window nothing draws in on that
    # path (the creature is drawn by a port AFTER kf_app_frame() returns),
    # so this number would have read 0 forever regardless of how the
    # indexed blit actually performed. See headless_main.cpp's
    # run_frame_counters_check() and docs/architecture/
    # adr-0036-frame-counter-window.md.
    add_test(NAME frame_counters_check
             COMMAND kamiframe-headless --verify-frame-counters)

    # Minor 6/7 of the pre-merge review (.superpowers/sdd/pre-merge-fixes-
    # report.md): the only check exercising the real, animated creature_demo
    # pack's dirty-rect cost (frame_counters_check, above) bounds it loosely
    # at [1, KF_MAX_DIRTY_RECTS], and the check that drives all three stat
    # bars (creature_screen_stats_check, above) does it against the
    # placeholder-only default pack and only ever moves one need at a time.
    # Neither combines creature erase+draw, mess, and all three bars
    # changing in the same frame -- measured at 5 of the available 8 (the
    # analytical ceiling is 6; this check's own comment explains the gap
    # between that and what it actually measures). See headless_main.cpp's
    # run_creature_screen_budget_combination_check().
    add_test(NAME creature_screen_budget_combination_check
             COMMAND kamiframe-headless
                     --verify-creature-screen-budget-combo)

    # Task 2 of the Lua game layer plan (the Lua game-layer plan): the
    # retained scene differ (kf/scene.h), proved on its own with no Lua
    # involved -- see headless_main.cpp's run_scene_check() for the
    # four things it checks, in order.
    add_test(NAME scene_check
             COMMAND kamiframe-headless --verify-scene)

    # Task 3 of the Lua game layer plan (the Lua game-layer plan): the Lua
    # binding over kf/scene.h -- proved by diffing a scene DECLARED FROM
    # LUA against the same picture drawn by hand with
    # kf_fill_rect()/kf_blit_frame() directly, exactly scene_check's own
    # proof but with a script in the loop, plus two anti- vacuity checks
    # (the 65th object, a removed object) and the minimal- pet example run
    # verbatim from disk. See headless_main.cpp's run_lua_draw_check() for
    # the seven things it checks, in order.
    add_test(NAME lua_draw_check
             COMMAND kamiframe-headless --verify-lua-draw)

    # Task 5 of the Lua game layer plan (the Lua game-layer plan): THE
    # correctness argument for the whole plan. Drives a fresh pet through an
    # identical 250-frame timeline -- egg bob, wander, mess appearing and
    # being flushed, a button-triggered care reaction, death and the shrine,
    # revival clearing it -- once through kf_creature_ screen.cpp (unchanged
    # since Task 4) and once through examples/ creature_demo/creature.lua
    # (this task's own home-screen declarations), and asserts the two painted
    # byte-identical framebuffers on every one of those 250 frames, not just
    # the last. See headless_main.cpp's run_lua_vs_cpp_screen_check() for the
    # FNV-1a rolling hash and the exact event timeline. This build's own
    # KF_HOME_SCREEN setting is irrelevant here: both implementations are
    # always compiled in, and this check drives each directly at runtime.
    add_test(NAME screen_parity_check
             COMMAND kamiframe-headless --verify-screen-parity)

    # Task 1 of the screens/clock/sleep plan, ADR 0044: kf.screen() -- named
    # groups of scene objects over the one shared retained scene, and the
    # register-by-name navigation table that replaced the old fixed
    # two-entry array. Declares two screens from an inline script, switches
    # between them through screen:show(), then reproduces the identical
    # transitions through kf_screen_nav_debug_advance()/_debug_home() (the
    # same edges a real MENU/B press produces) and asserts identical
    # framebuffers -- the anti-two-owners property the whole task exists
    # to guarantee. See headless_main.cpp's run_screen_group_check().
    add_test(NAME screen_group_check
             COMMAND kamiframe-headless --verify-screen-groups)

    # Task 3 of the screens/clock/sleep plan: kf/clock.h, integer-only civil
    # time in Core, proved on its own with nothing else in the picture --
    # no pet, no Lua, no Settings screen. See headless_main.cpp's
    # run_clock_check() for the round trip, the hand-computed civil values,
    # and the 22:00-07:00 night-window arithmetic it checks, in that order.
    add_test(NAME clock_check
             COMMAND kamiframe-headless --verify-clock)

    # Task 4 of the screens/clock/sleep plan: the Lua time API (kf.time/
    # hour/minute/clock_set/set_clock) and the Settings screen's four-field
    # editor, driven with kf_app_debug_set_buttons() the way a real MENU/
    # LEFT/RIGHT/UP/DOWN/A/B sequence would be. Proves kf.time()'s "--:--
    # --" unset-clock string and its exact 12-hour format, the real Home +
    # Info + Settings live-object count (closing risk 5), that saving moves
    # the wall clock by exactly the expected amount and nothing else, and
    # that cancelling with B from a modified state does not move it at all.
    # See headless_main.cpp's run_settings_screen_check().
    add_test(NAME settings_screen_check
             COMMAND kamiframe-headless --verify-settings-screen)

    # Regression for a real hardware bug (owner's own report running the
    # ESP32 build): Home's creature/poop/shrine objects kept showing on top
    # of Info and Settings, frozen but never actually hidden, because
    # creature.lua's Home-drawing block lived inside the SHARED on_frame(),
    # guarded by kf.home_screen_active() -- a BUILD-TIME flag, not a "is
    # Home showing right now" query -- and the main loop calls on_frame()
    # unconditionally every real frame regardless of the active screen.
    # Fixed by giving Home its own on_home_frame() entry point, the same
    # shape Info and Settings already had. Drives the SAME two per-frame
    # calls the real main loop makes (kf_screen_nav_frame() then the
    # unconditional kf_lua_port_frame(0)) -- exactly what neither
    # screen_parity_check nor screen_nav_check ever replicates, and exactly
    # what this bug needed to actually surface. See headless_main.cpp's
    # run_screen_isolation_check().
    add_test(NAME screen_isolation_check
             COMMAND kamiframe-headless --verify-screen-isolation)

    # Task 7 of the screens/clock/sleep plan: sleep on screen -- pet.asleep()/
    # pet.wake() bound to Lua, kf_pet_wake()'s Core mechanics, the wander
    # freezing while asleep (screen_parity_check hashes one frame and cannot
    # catch a creature that keeps walking asleep -- this drives 300+ real
    # frames and checks position stays put), the *_sleeping_* pose, the
    # tuck-in interaction (kf.button()) and its futon_idle_s scenery, the ZZZ
    # blink/wiggle actually animating, and the worst-case dirty-rect count
    # while sleep's own scenery is on screen. See headless_main.cpp's
    # run_sleep_screen_check().
    add_test(NAME sleep_screen_check
             COMMAND kamiframe-headless --verify-sleep-screen)

    # Task 8 of the screens/clock/sleep plan: the attention signal --
    # kf_pet_wants()'s purity/hysteresis/priority order/dead-and-asleep
    # gating direct against Core, pet.wants() as a Lua-bound uppercase
    # string (or nil), and creature.lua's three presentation layers (held
    # pose/position, the 1 Hz "!", the inverted care-guide entry) driven
    # through the real Home screen with pixel-level assertions, plus the
    # worst-case dirty-rect count while a want is active. See
    # headless_main.cpp's run_attention_signal_check().
    add_test(NAME attention_signal_check
             COMMAND kamiframe-headless --verify-attention-signal)

    # Owner follow-up after Task 4/8 landed (Chris, 2026-08-11, after
    # testing): a small always-on digital clock in Home's upper-left
    # corner, showing kf.time() verbatim. Proves the REAL clock text
    # object shows exactly the string kf.time() returns (pixel-identical
    # to a direct reference render, not merely "something is there"),
    # repaints when that string changes, and does not dirty a rectangle
    # on a frame where it did not. See headless_main.cpp's
    # run_home_clock_check().
    add_test(NAME home_clock_check
             COMMAND kamiframe-headless --verify-home-clock)

    # Sound foundation task: kf/hal/audio.h's contract against the headless
    # recording backend (invalid hz/ms rejected and recorded as nothing,
    # valid calls recorded exactly), the kf.tone()/kf.beep() Lua binding
    # (right arguments reach the HAL, in the right order, invalid ones
    # report false rather than raising), and creature.lua's attention-signal
    # chirp driven through the real Home screen -- exactly one chirp per
    # want streak, a fresh chirp for a fresh streak, and no chirp while
    # asleep. See headless_main.cpp's run_audio_check().
    add_test(NAME audio_check
             COMMAND kamiframe-headless --verify-audio)

    # ADR 0058: the one new assertion the frame-loop extraction adds --
    # kf_frame_loop_run()'s debug time multiplier moves the wall clock and
    # the pet's own clock by the SAME amount in the SAME call. This is the
    # exact invariant whose violation (only the pet's delta was scaled,
    # ports/esp32/main/app_main.cpp, fixed in d1ab8a7) cost a real
    # diagnosis one power cycle after the fact. See headless_main.cpp's
    # run_frame_loop_multiplier_check().
    add_test(NAME frame_loop_multiplier_check
             COMMAND kamiframe-headless --verify-frame-loop-multiplier)
endif()

# ---------------------------------------------------------------------------
# lua_embed_check -- Task 1 of the Lua game-layer plan. Every script embedded
# into a header by tools/kf_embed_lua.py (today:
# examples/creature_demo/creature.lua ->
# sdk/lua/generated/kf_lua_demo_creature_script.h) is CHECKED IN rather than
# generated at configure time, so a build stays self-contained and offline
# and a reviewer can see the exact bytes that ship (see the plan's "Decisions
# already taken"). This test is what keeps that checked-in copy honest: it
# regenerates every mapped header into a temp directory and diffs, failing
# with the fix command if the .lua source and its header have drifted apart
# -- e.g. someone edited creature.lua and forgot to regenerate, or
# hand-edited the generated header directly.
#
# No kamiframe-headless build dependency -- this is pure Python, run
# directly against the checked-out source tree, so it is registered
# unconditionally rather than nested inside KAMIFRAME_BUILD_HEADLESS above.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME lua_embed_check
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_CURRENT_LIST_DIR}/../tools/kf_embed_lua.py --check)
