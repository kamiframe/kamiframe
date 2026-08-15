## SPDX-License-Identifier: Apache-2.0
## Copyright the Kamiframe contributors.
##
## ESP-IDF requires a component named `main`. Phase 1b (ADR 0020): this now
## builds app_main.cpp, which calls the real kf_app_init()/kf_app_frame()
## loop against the real ESP32 HAL backends in ../hal/ -- not the ADR 0019
## hello-world's main.c, which is gone (see app_main.cpp's own header
## comment for what it kept from main.c and what it does not claim to run).
##
## REQUIRES hakoniwaos so this build compiles every file in
## hakoniwaos/sources.cmake for the real target, same as ADR 0019, plus every
## ESP-IDF component the HAL backends in ../hal/ actually call into --
## added one at a time as each backend file was written, not guessed at
## up front.
##
## WHOLE_ARCHIVE, found the hard way (a real `idf.py build` link failure,
## not guessed): hakoniwaos/src/app.cpp calls kf_display_init(),
## kf_time_init(), and every other kf_hal_* function as plain extern "C"
## symbols -- it does not know or care that this component supplies their
## definitions. That is the whole point of the HAL boundary (kf/hal/*.h),
## but it means the actual link-time dependency runs BACKWARDS from what
## `main REQUIRES hakoniwaos` declares: hakoniwaos.a needs symbols out of
## main.a, not the other way around. ESP-IDF's automatic archive-repeat
## logic (used to resolve genuine circular component dependencies) walks
## the declared REQUIRES graph, and no component declares a dependency
## running from hakoniwaos back to main -- correctly, since hakoniwaos must
## stay backend-agnostic -- so it never repeats libmain.a in the final link
## line. The result: libmain.a is scanned exactly once, before anything has
## referenced its HAL symbols yet (app_main.cpp's own call into
## kf_app_init() hasn't been processed), so every object in it that only
## defines kf_hal_* functions gets silently skipped, and the link fails with
## a wall of "undefined reference to kf_display_init" and friends despite
## every one of those functions genuinely existing and having compiled
## clean. WHOLE_ARCHIVE forces the entire component into the final binary
## regardless of reference-resolution order, which is exactly the fix ESP-IDF
## documents this flag for.
##
## kf_pet_session.cpp, added here rather than given its own EXTRA_COMPONENT_
## DIRS entry: it is one file with one dependency (hakoniwaos, already
## REQUIRES'd below) and no third-party library of its own -- unlike LVGL
## (ADR 0027) and Lua (ADR 0028), which each pull in an external dependency
## and earn their own component the way hakoniwaos itself does. Referenced
## from simulator/src/pet/ by relative path, the same pattern the HAL
## backends above already use for ../hal/ -- one canonical copy of the file,
## not a fork.
##
## kf_creature_screen.cpp (Task 4 of the pet-screen plan), added right next
## to kf_pet_session.cpp for the identical reason: one file, one dependency
## on kf_pet_session.h (already this component's own neighbour) plus
## hakoniwaos's kf/creature.h and kf/blit.h, no LVGL and no third-party
## library of its own -- it is app_main.cpp's kf_screen_nav_init() that
## reaches it, via kf_screen_nav.cpp (below, in this same component since
## ADR 0045), not this file directly, the same indirection sdl_main.cpp
## uses on desktop.
##
## kf_creature_presenter.cpp/kf_home_screen_input.cpp (Task 5 of the Lua
## game-layer plan): the wander/pose/animation orchestration and the five
## hardware care buttons, extracted OUT of kf_creature_screen.cpp so that
## KF_HOME_SCREEN=lua (kf_screen_nav.cpp, this same component since
## ADR 0045) gets the identical wander result and the identical buttons the
## cpp screen always had -- same one-file-no-third-party-dependency
## reasoning as kf_pet_session.cpp above.
##
## kf_lua_alloc.cpp/kf_lua_port.cpp (ADR 0028): same relative-path pattern as
## kf_pet_session.cpp above -- these two files have no third-party library of
## their own, only Lua itself does, which is why Lua earns the "lua" REQUIRES
## below (../components/lua, registered via this port's CMakeLists.txt
## EXTRA_COMPONENT_DIRS) instead of being folded in here.
##
## kamiframe_lvgl_port REQUIRES, only under -DKF_ENABLE_LVGL=ON (ADR 0045,
## Task 2 of the screens/clock/sleep plan): the
## component does not even exist otherwise -- ../CMakeLists.txt only adds
## its directory to EXTRA_COMPONENT_DIRS when this is set, see that file's
## own comment. Historically (ADR 0027) app_main.cpp called kf_lvgl_port_
## init()/kf_lvgl_port_pump(), both defined in that component -- REQUIRES
## brought in its public headers (so app_main.cpp's bare #include
## "kf_lvgl_port.h" resolves, the same INCLUDE_DIRS-propagation
## ../../hakoniwaos already relies on for kf/*.h) and, per the WHOLE_ARCHIVE
## comment above, positioned kamiframe_lvgl_port.a after this component in
## the final link -- the same position hakoniwaos.a already occupies, for
## the same single-pass-resolution reason. kf_screen_nav_init()/_frame()
## moved OUT of that component with ADR 0045 (into kf_screen_nav.cpp below,
## compiled straight into this component like kf_pet_session.cpp already
## is) precisely so screen navigation stops needing this REQUIRES at all.
##
## kf_dbg_bridge.cpp/kf_dbg_codec.cpp (ADR 0030, the KFDBG serial debug
## bridge): plain files in this directory, not a new component -- same
## reasoning as kf_pet_session.cpp above, one canonical copy with no
## third-party dependency of its own. esp_driver_uart REQUIRES is new here:
## nothing else in this component called driver/uart.h before this.
##
## kf_screen_nav.cpp/kf_lua_home_screen.cpp/kf_error_banner.cpp (ADR 0045):
## moved here from the kamiframe_lvgl_port component's own SRCS -- none of
## the three has ever had an LVGL dependency of its own; they only lived
## there because kf_screen_nav.cpp used to #include <lvgl.h> for Info's
## LVGL screen-load calls. Same one-canonical-copy-by-relative-path pattern
## as kf_pet_session.cpp above, and needed UNCONDITIONALLY (not gated on
## KF_ENABLE_LVGL): every build needs screen navigation, whether or not
## LVGL is linked in at all.
##
## kf_frame_loop.cpp (ADR 0058): the one per-frame sequence app_main.cpp's
## own loop and sdl_main.cpp's now both call instead of each hand-
## maintaining their own copy -- see that file's own header comment for why
## (a real, hardware-diagnosed bug) and for exactly what stayed out of it.
## Same one-canonical-copy-by-relative-path pattern as kf_pet_session.cpp
## above; it needs kf_pet_session.h and kf_screen_nav.h (already this
## component's own neighbours) plus kf_lua_port.h/kf_lua_scene.h (already
## reachable via the "../../../sdk/lua" INCLUDE_DIRS entry below, the same
## way kf_screen_nav.cpp already reaches them).
##
## KF_MAIN_REQUIRES built as a list, not inline in REQUIRES below, purely so
## kamiframe_lvgl_port can be conditional (ADR 0045) -- REQUIRES itself
## takes a plain list, and appending to one before the call is simpler and
## less error-prone than any post-registration mechanism.
set(KF_MAIN_REQUIRES
    hakoniwaos
    lua
    spi_flash
    esp_partition
    esp_driver_gpio
    esp_driver_spi
    esp_driver_i2c
    esp_driver_i2s
    esp_driver_uart
    # esp_driver_ledc: the display backlight's PWM dimmer. Its own component
    # in ESP-IDF v6 -- <driver/ledc.h> is not reachable via esp_driver_gpio
    # the way it was when the drivers lived in one blob, so leaving this out
    # fails at the #include, not at link time.
    esp_driver_ledc
    esp_lcd
    esp_timer
    nvs_flash
    esp_hw_support
    heap
    esp_system
)
##
## $ENV{KF_ENABLE_LVGL}, not the plain ${KF_ENABLE_LVGL} CACHE variable --
## found the hard way (a real `idf.py build` REQUIRES-graph mismatch, not
## guessed): ESP-IDF's own component_get_requirements.cmake (~/esp/esp-idf/
## tools/cmake/scripts/) runs a SEPARATE `cmake -P` process to scan every
## candidate component's CMakeLists.txt for its REQUIRES, ahead of the real
## configure -- confirmed by instrumenting this exact line with message()
## and watching it run TWICE with two different states: once with
## KF_ENABLE_LVGL genuinely empty (the scan process, which has no
## CMakeCache.txt or -D of its own -- it inherits none of the parent
## cmake process's variables, only whatever ESP-IDF explicitly threads
## through via generated property files), and once with it correctly ON
## (the real, later configure). The scan process's answer is what actually
## gets baked into the cross-component "who may include whose headers"
## graph ESP-IDF enforces -- the later, correct answer is too late to
## matter. ../CMakeLists.txt exports this same value into the process
## ENVIRONMENT (set(ENV{KF_ENABLE_LVGL} ...)) specifically because
## environment variables, unlike CMake cache/normal variables, DO cross
## process boundaries the way this scan's separate `cmake -P` invocation
## needs -- see that file's own comment on the export.
if("$ENV{KF_ENABLE_LVGL}")
    list(APPEND KF_MAIN_REQUIRES kamiframe_lvgl_port)
endif()

idf_component_register(
    SRCS
        app_main.cpp
        ../hal/esp_display.cpp
        ../hal/esp_input.cpp
        ../hal/esp_time.cpp
        ../hal/esp_memory.cpp
        ../hal/esp_entropy.cpp
        ../hal/esp_log.cpp
        ../hal/esp_storage.cpp
        ../hal/esp_power.cpp
        ../hal/esp_assets.cpp
        ../hal/esp_audio.cpp
        ../../../simulator/src/pet/kf_pet_session.cpp
        ../../../simulator/src/pet/kf_creature_screen.cpp
        ../../../simulator/src/pet/kf_creature_presenter.cpp
        ../../../simulator/src/pet/kf_home_screen_input.cpp
        ../../../simulator/src/pet/kf_soft3d.cpp
        ../../../simulator/src/pet/kf_screen_nav.cpp
        ../../../simulator/src/pet/kf_lua_home_screen.cpp
        ../../../simulator/src/pet/kf_lua_settings_screen.cpp
        ../../../simulator/src/pet/kf_error_banner.cpp
        ../../../simulator/src/pet/kf_frame_loop.cpp
        ../../../simulator/src/pet/kf_debug_actions.cpp
        ../../../sdk/lua/kf_lua_alloc.cpp
        ../../../sdk/lua/kf_lua_port.cpp
        ../../../sdk/lua/kf_lua_scene.cpp
        kf_dbg_bridge.cpp
        kf_dbg_codec.cpp
    INCLUDE_DIRS "." "../hal" "../../../simulator/src/pet" "../../../sdk/lua" "../../../sdk/lua/generated"
    REQUIRES ${KF_MAIN_REQUIRES}
    WHOLE_ARCHIVE
)

## Puts the checked-in test asset pack (ADR 0033) into the "assets" data
## partition (../partitions.csv) as part of `idf.py flash`, the same
## mechanism ESP-IDF's own examples use for a raw (non-filesystem) data
## partition -- see e.g. examples/storage/nvs/nvs_bootloader in the ESP-IDF
## tree for another call to this exact function. esp_assets.cpp's
## esp_partition_mmap() call above only maps whatever is already in flash;
## this is what actually puts the pack file's bytes there. Only reached
## once ESP-IDF's own project.cmake (included by ../CMakeLists.txt) has
## defined esptool_py_flash_to_partition(), which is why this call lives
## here rather than earlier in the file.
##
## The CREATURE pack, not hello_sprite's. Switched 2026-08-11, once
## esp_partition_mmap() had been confirmed on real silicon against the small
## 1,156-byte pack -- the deliberate order the bring-up plan asks for, since a
## mapping fault and a rendering fault look identical from the outside and the
## small pack rules the first one out cheaply. With that proven, this is the
## pack the game actually wants: 94 entries, 556,488 bytes, six creatures with
## animated idles, well inside the 12MB partition (../partitions.csv).
##
## Flashing takes noticeably longer than the test pack did. That is 556KB
## crossing a serial link, not a fault.
esptool_py_flash_to_partition(flash "assets"
    "${CMAKE_CURRENT_LIST_DIR}/../../../examples/creature_demo/assets.kfpack")

## See kf_pet_session.h's "DEBUG ONLY" section and kf_pet_session.cpp's
## top-of-file comment: the pet session's debug tools now split across TWO
## flags, not one, because they cost very differently --
##
##   KF_PET_SESSION_ENABLE_DEBUG_CONTROLS (advance/reset/age_seconds/
##   jump_to_stage/clock_target/set_clock) is a handful of instructions on
##   top of code the gameplay path already links in. Left ON (its default)
##   here, deliberately: it is how kf_dbg_bridge.cpp's KFDBG ADVANCE/RESET/
##   MULT commands below reach the pet session at all, letting a developer
##   fast-forward a real device's multi-day decay curve over the serial
##   bridge instead of watching it do nothing for days (see ADR 0030's
##   "Time control" section). jump_to_stage now has a KFDBG command too
##   (KFDBG JUMP -- see kf_dbg_bridge.cpp's handle_jump()), so this flag
##   staying on is no longer merely harmless, it is load-bearing for that
##   command as well, not just the other three. clock_target/set_clock are
##   the newest pair (ADR 0054): KFDBG CLOCK reaches them the same way,
##   verified by the same "does this build even have a caller for it"
##   question this comment already asks about jump_to_stage.
##
##   KF_PET_SESSION_ENABLE_DEBUG_TOOLS gates the scrubbable-timeline
##   snapshot ring and kf_pet_session_debug_seek() -- a real static
##   allocation over 200KB that desktop/headless get by default -- plus
##   kf_pet_session_state_mutable_for_test() (Task 5), which costs nothing
##   itself but rides the same flag as a test-only reach-into-Core no real
##   device should link in either. This build opts out of all of it
##   explicitly, since a real device has neither the SRAM to spare nor any
##   caller for any of the three (nothing in app_main.cpp scrubs a timeline
##   or reaches into pet state directly).
##
## LUA_32BITS: must match ../components/lua/CMakeLists.txt's identical
## define on `lua_core` exactly -- see that file's own comment for why
## (an xtensa-esp-elf/C++ LLONG_MAX gap, and the ABI reason both sides of
## the kf_lua_port.cpp <-> lua_core boundary have to agree on LUA_INTEGER's
## real width, not just its typedef'd name).
##
## KF_DBG_BRIDGE_ENABLE / KF_DBG_MUTATE_ENABLE / KF_DBG_INPUT_INJECT_ENABLE
## (ADR 0030/0035) are NOT set here -- all three default to 1 in
## kf_dbg_bridge.h, which is correct for this pre-release developer
## firmware. Three flags, one nested inside the next, because KFDBG's
## commands split into three risk tiers, not two:
##
##   - PING/SHOT/STATE/SCANLINE/VSYNC/RTC are read-only introspection --
##     nothing a serial connection could learn that watching the screen
##     couldn't. RTC (ADR 0054) joined this tier reading the DS3231 chip
##     directly, not the pet -- still nothing that changes.
##   - FEED/PLAY/REST/BATH/FLUSH/JUMP/ADVANCE/RESET/MULT/CLOCK/BTN/BTNHOLD
##     all change the pet or the simulation -- for a pet whose whole premise
##     is that time and care are real, a serial cable reaching any of these
##     is the cheat this split exists to gate. CLOCK (ADR 0054) joined this
##     tier for the same reason ADVANCE/RESET/MULT are in it: it moves the
##     pet's own notion of what time it is.
##   - BTN/BTNHOLD specifically can drive ANY UI flow reachable by button
##     presses, not just the pet's state -- a strictly larger surface than
##     the other ten mutating commands put together.
##
## A SHIPPING build removes the bridge (and therefore everything below)
## entirely by adding, right here:
##     target_compile_definitions(${COMPONENT_LIB} PRIVATE KF_DBG_BRIDGE_ENABLE=0)
## which compiles kf_dbg_bridge_init()/_frame()/_shutdown() down to empty
## functions and kf_dbg_input_mask() down to an unconditional `return 0` --
## see kf_dbg_bridge.h's own comment on that flag. A build that wants
## screenshots and state reads in the field without handing a serial
## connection any way to touch the pet keeps the bridge on and adds:
##     target_compile_definitions(${COMPONENT_LIB} PRIVATE KF_DBG_MUTATE_ENABLE=0)
## which leaves PING/SHOT/STATE/SCANLINE/VSYNC/RTC working and makes every
## mutating command reply `err` instead of running (see kf_dbg_bridge.cpp's
## require_mutate_enabled()). Setting only KF_DBG_INPUT_INJECT_ENABLE=0
## narrows the mutate set further, refusing button injection specifically
## while leaving named care/time-control commands available -- meaningless
## on top of KF_DBG_MUTATE_ENABLE=0, since nothing nested inside an off
## flag matters. A build that removes the bridge entirely also loses
## ADVANCE/RESET/MULT/CLOCK/JUMP below, since they are KFDBG commands like
## any other -- KF_PET_SESSION_ENABLE_DEBUG_CONTROLS staying on costs
## nothing by itself in that configuration, it simply has no caller left.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    KF_PET_SESSION_ENABLE_DEBUG_CONTROLS=1
    KF_PET_SESSION_ENABLE_DEBUG_TOOLS=0
    LUA_32BITS
)

## KF_PANEL (ADR 0039): a normal CMake cache variable, not a #define someone
## has to remember to edit, selecting which table in
## ../hal/kf_panel_profile.h this build drives. Passed on the command line:
##
##     idf.py -DKF_PANEL=st7789 build      # explicit default, same as a
##                                            plain `idf.py build`
##     idf.py -DKF_PANEL=ili9341 build     # the 2.8in alternative
##
## Defaults to st7789 as of 2026-08-13 (ADR 0059), when that panel was first
## verified on hardware. It is the primary panel for the product, and both
## profiles now display correctly, so the "do not default to a black screen
## on the one board in the world running this firmware" reasoning that used
## to pin this to ili9341 no longer selects between them -- see
## kf_panel_profile.h's own KF_PANEL_PROFILE fallback, which moved with it.
##
## THIS IS A CACHE VARIABLE, so it sticks in a build directory once set.
## Switching panels means passing the other value explicitly, and flashing
## the wrong profile gives a black screen or wrong colours with a completely
## clean log. Keep this default and kf_panel_profile.h's in step: they are
## two spellings of one decision, and the header's #ifndef never fires in
## this build because the line below always defines it.
##
## Validated here, at CMake configure time, rather than left for the
## compiler to discover: a typo (e.g. KF_PANEL=st7889) that reached
## kf_panel_profile.h as KF_PANEL_PROFILE=kf_panel_st7889 would fail with
## "use of undeclared identifier kf_panel_st7889" deep inside a header,
## which reads like a code bug, not a command-line mistake. Catching it here
## turns that into one clear FATAL_ERROR naming the actual typo instead.
set(KF_PANEL "st7789" CACHE STRING "Panel profile: st7789 or ili9341")
if(NOT KF_PANEL STREQUAL "ili9341" AND NOT KF_PANEL STREQUAL "st7789")
    message(FATAL_ERROR
        "KF_PANEL must be 'ili9341' or 'st7789', got '${KF_PANEL}' -- see "
        "../hal/kf_panel_profile.h for the profiles this can select.")
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    KF_PANEL_PROFILE=kf_panel_${KF_PANEL}
)

## KF_SPI_HZ: the display clock, for A/B testing a faster panel on real glass
## without editing the panel profile and without an experimental value
## reaching a commit by accident.
##
##     idf.py -DKF_SPI_HZ=80000000 build flash monitor
##
## PRIVATE to this component is correct and not an oversight. The clock is
## read only by ../hal/esp_display.cpp, compiled here. Core's transfer
## estimate does NOT read any macro -- estimate_transfer_us() takes
## link_bytes_per_second as a runtime argument, fed from the display caps
## that esp_display.cpp fills in from the same clock. So the wire speed, the
## caps and the budget model all move together. Widening this to hakoniwaos
## would change nothing and would imply the opposite.
##
## Empty by default, which leaves the ACTIVE PANEL PROFILE's own measured
## clock in place (kf_panel_profile.h's spi_hz -- 40MHz on the ILI9341, 80MHz
## on the ST7789). Promote a value into that profile only after it has
## survived the real game for a long run, not a test card: marginal SPI drops
## occasional bits rather than failing outright, and the symptom is a stray
## speckle or a slightly wrong colour minutes apart.
## KF_DEMO3D: flash a build that shows the software 3D renderer instead of
## the pet, to see what it actually looks like on glass.
##
##     idf.py -DKF_DEMO3D=1 build flash monitor    # 2.5D: 3D pet in a window
##     idf.py -DKF_DEMO3D=2 build flash monitor    # full-screen 3D
##     idf.py build flash monitor                  # back to the pet
##
## Every 3D number this project has is a desktop model (kamiframe-headless
## --stress3d). This is the only way to get one off real silicon, and to see
## whether a lit low-poly creature reads as 3D at 240x320 on a 2in panel --
## which no measurement can answer.
##
## Does not touch the pet's save: the demo build never runs the pet session,
## so reflashing without the flag brings the creature back where it was.
set(KF_DEMO3D "0" CACHE STRING "0=pet, 1=2.5D demo, 2=full-screen 3D demo")
if(NOT KF_DEMO3D MATCHES "^[012]$")
    message(FATAL_ERROR
        "KF_DEMO3D must be 0, 1 or 2, got '${KF_DEMO3D}'.")
endif()
if(NOT KF_DEMO3D STREQUAL "0")
    message(STATUS "KF_DEMO3D=${KF_DEMO3D}: this build shows the 3D demo, "
                    "NOT the pet")
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    KF_DEMO3D_MODE=${KF_DEMO3D}
)

set(KF_SPI_HZ "" CACHE STRING "Display SPI clock in Hz; empty = the active panel profile's measured spi_hz")
if(NOT KF_SPI_HZ STREQUAL "")
    if(NOT KF_SPI_HZ MATCHES "^[0-9]+$")
        message(FATAL_ERROR
            "KF_SPI_HZ must be a plain decimal number of Hz, got '${KF_SPI_HZ}' "
            "-- e.g. -DKF_SPI_HZ=80000000, not 80MHz or 80000000u.")
    endif()
    message(STATUS "KF_SPI_HZ override: ${KF_SPI_HZ} Hz "
                    "(kf/budget.h's default is not in effect)")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        KF_SPI_HZ_OVERRIDE=${KF_SPI_HZ}
    )
endif()

## KF_HOME_SCREEN (Task 5 of the Lua game-layer plan): set at the top of
## ../CMakeLists.txt, not here -- see that file's own comment for why. This
## one define covers both of this component's own readers: kf_lua_port.cpp
## reads KF_HOME_SCREEN_LUA for kf.home_screen_active()'s compile-time
## default, and kf_screen_nav.cpp (moved into this component's SRCS with
## ADR 0045) reads it for its own build-time choice of which Home
## implementation to wire up -- one define, ${COMPONENT_LIB}-wide, now that
## both files live in the same component.
if(KF_HOME_SCREEN STREQUAL "lua")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE KF_HOME_SCREEN_LUA=1)
endif()
