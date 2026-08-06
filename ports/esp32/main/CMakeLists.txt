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
## REQUIRES'd below) and no third-party library of its own -- unlike the
## LVGL and Lua ports still ahead, which each pull in an external
## dependency and earn their own component the way hakoniwaos itself does.
## Referenced from simulator/src/pet/ by relative path, the same pattern
## the HAL backends above already use for ../hal/ -- one canonical copy of
## the file, not a fork.
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
        ../../../simulator/src/pet/kf_pet_session.cpp
    INCLUDE_DIRS "." "../hal" "../../../simulator/src/pet"
    REQUIRES
        hakoniwaos
        spi_flash
        esp_driver_gpio
        esp_driver_spi
        esp_lcd
        esp_timer
        nvs_flash
        esp_hw_support
        heap
        esp_system
    WHOLE_ARCHIVE
)

## See kf_pet_session.cpp's top-of-file comment: the desktop/headless
## backends get the scrubbable debug-timeline ring by default (a real
## static allocation over 200KB), and this build opts out of it
## explicitly, since a real device has neither the SRAM to spare nor any
## caller for it (nothing in app_main.cpp is a debug key binding).
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    KF_PET_SESSION_ENABLE_DEBUG_TOOLS=0
)
