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

# Shared by both backends: pools, logging, entropy, clocks. Nothing here
# touches a display or an input device.
set(KF_HOST_COMMON_SRCS
    src/host/host_memory.cpp
    src/host/host_log.cpp
    src/host/host_time.cpp
    src/host/host_entropy.cpp
)

add_library(kamiframe_host_common STATIC ${KF_HOST_COMMON_SRCS})
target_include_directories(kamiframe_host_common
    PUBLIC ${CMAKE_CURRENT_LIST_DIR}/src/host)
target_link_libraries(kamiframe_host_common
    PUBLIC hakoniwaos kamiframe_options)

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
        PRIVATE hakoniwaos kamiframe_host_common kamiframe_options
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
        PRIVATE hakoniwaos kamiframe_host_common kamiframe_options)
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
    set(KAMIFRAME_GOLDEN_CHECKSUM        "964b2c7426d41723")
    set(KAMIFRAME_GOLDEN_CHECKSUM_STRESS "6b9e3d13c8b9af4f")

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
endif()
