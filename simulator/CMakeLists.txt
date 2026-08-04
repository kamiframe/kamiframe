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

# Shared by both backends: pools, logging, entropy, clocks, storage, power.
# Nothing here touches a display or an input device.
set(KF_HOST_COMMON_SRCS
    src/host/host_memory.cpp
    src/host/host_log.cpp
    src/host/host_time.cpp
    src/host/host_entropy.cpp
    src/host/host_storage.cpp
    src/host/host_power.cpp
)

add_library(kamiframe_host_common STATIC ${KF_HOST_COMMON_SRCS})
target_include_directories(kamiframe_host_common
    PUBLIC ${CMAKE_CURRENT_LIST_DIR}/src/host)
target_link_libraries(kamiframe_host_common
    PUBLIC hakoniwaos kamiframe_options)

# ---------------------------------------------------------------------------
# Pet session -- owns the live kf_pet_state instance and drives its two
# "ages" (offline fast-forward at boot, live decay per frame). See ADR
# 0016. Lives here, not in hakoniwaos/, for the same reason
# kamiframe_lvgl_port and kamiframe_lua_port do: this is simulator-only
# orchestration around a Core mechanism, not a claim that the ESP32 build
# has it wired up. Built before both the LVGL and Lua port glue below,
# because kamiframe_lvgl_port's pet screen (ADR 0017) and
# kamiframe_lua_port's pet.* binding (ADR 0016) both link against it.
# ---------------------------------------------------------------------------
add_library(kamiframe_pet_port STATIC
    src/pet/kf_pet_session.cpp
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
    src/lvgl/kf_lvgl_port.cpp
    src/lvgl/kf_lvgl_proof_screen.cpp
    src/lvgl/kf_pet_screen.cpp
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
    # Updated twice on 2026-08-04, same day as the initial value -- see
    # docs/architecture/adr-0017-pet-screen.md's "Found after delivery"
    # section for the full story. First update: added a focused-button
    # style (kf_pet_screen.cpp's g_focus_style, LV_STATE_FOCUSED), which
    # changes whichever button is focused at start's rendered pixels.
    # Second update (this one): removed the explicit lv_group_add_obj()
    # calls that were corrupting which button ends up focused in the first
    # place (lv_button auto-joins the default group at creation -- the
    # explicit calls were redundant re-adds that could kick focus onto the
    # wrong member), so Feed, not Play, is now correctly the one rendered
    # focused. Deterministic across 6 repeated runs before being locked in,
    # same discipline as every earlier value here.
    set(KAMIFRAME_PET_SCREEN_GOLDEN_CHECKSUM "5cc5b283817de3e6")

    add_test(NAME pet_screen_check
             COMMAND kamiframe-headless --verify-pet-screen
                     --expect-checksum ${KAMIFRAME_PET_SCREEN_GOLDEN_CHECKSUM})

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
endif()
