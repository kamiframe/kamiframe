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
## kf_lua_alloc.cpp/kf_lua_port.cpp (ADR 0028): same relative-path pattern as
## kf_pet_session.cpp above -- these two files have no third-party library of
## their own, only Lua itself does, which is why Lua earns the "lua" REQUIRES
## below (../components/lua, registered via this port's CMakeLists.txt
## EXTRA_COMPONENT_DIRS) instead of being folded in here.
##
## kamiframe_lvgl_port REQUIRES: app_main.cpp now calls kf_lvgl_port_init()/
## kf_lvgl_port_pump() and kf_screen_nav_init()/kf_screen_nav_frame()
## (ADR 0027), both defined in that component, not this one -- REQUIRES
## brings in both its public headers (so app_main.cpp's bare #include
## "kf_lvgl_port.h" / "kf_screen_nav.h" resolve, the same
## INCLUDE_DIRS-propagation ../../hakoniwaos already relies on for kf/*.h)
## and, per the WHOLE_ARCHIVE comment above, positions kamiframe_lvgl_port.a
## after this component in the final link -- the same position hakoniwaos.a
## already occupies, for the same single-pass-resolution reason: this
## component's own object files reference symbols that component defines,
## not the other way around, so this component staying WHOLE_ARCHIVE and
## that one staying an ordinary REQUIRES entry is enough; no WHOLE_ARCHIVE
## needed on kamiframe_lvgl_port itself. See docs/architecture/
## adr-0027-lvgl-on-esp32.md's "Verified" section for how that was actually
## confirmed, not just reasoned about.
##
## kf_dbg_bridge.cpp/kf_dbg_codec.cpp (ADR 0030, the KFDBG serial debug
## bridge): plain files in this directory, not a new component -- same
## reasoning as kf_pet_session.cpp above, one canonical copy with no
## third-party dependency of its own. esp_driver_uart REQUIRES is new here:
## nothing else in this component called driver/uart.h before this.
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
        ../../../simulator/src/pet/kf_pet_session.cpp
        ../../../simulator/src/lua/kf_lua_alloc.cpp
        ../../../simulator/src/lua/kf_lua_port.cpp
        kf_dbg_bridge.cpp
        kf_dbg_codec.cpp
    INCLUDE_DIRS "." "../hal" "../../../simulator/src/pet" "../../../simulator/src/lua"
    REQUIRES
        hakoniwaos
        lua
        kamiframe_lvgl_port
        spi_flash
        esp_partition
        esp_driver_gpio
        esp_driver_spi
        esp_driver_i2c
        esp_driver_uart
        esp_lcd
        esp_timer
        nvs_flash
        esp_hw_support
        heap
        esp_system
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
esptool_py_flash_to_partition(flash "assets"
    "${CMAKE_CURRENT_LIST_DIR}/../../../examples/hello_sprite/assets.kfpack")

## See kf_pet_session.h's "DEBUG ONLY" section and kf_pet_session.cpp's
## top-of-file comment: the pet session's debug tools now split across TWO
## flags, not one, because they cost very differently --
##
##   KF_PET_SESSION_ENABLE_DEBUG_CONTROLS (advance/reset/age_seconds) is a
##   handful of instructions on top of code the gameplay path already
##   links in. Left ON (its default) here, deliberately: it is how
##   kf_dbg_bridge.cpp's KFDBG ADVANCE/RESET/MULT commands below reach the
##   pet session at all, letting a developer fast-forward a real device's
##   multi-day decay curve over the serial bridge instead of watching it
##   do nothing for days (see ADR 0030's "Time control" section).
##
##   KF_PET_SESSION_ENABLE_DEBUG_TOOLS gates ONLY the scrubbable-timeline
##   snapshot ring and kf_pet_session_debug_seek() -- a real static
##   allocation over 200KB that desktop/headless get by default. This
##   build opts out of it explicitly, since a real device has neither the
##   SRAM to spare nor any caller for it (nothing in app_main.cpp scrubs a
##   timeline).
##
## LUA_32BITS: must match ../components/lua/CMakeLists.txt's identical
## define on `lua_core` exactly -- see that file's own comment for why
## (an xtensa-esp-elf/C++ LLONG_MAX gap, and the ABI reason both sides of
## the kf_lua_port.cpp <-> lua_core boundary have to agree on LUA_INTEGER's
## real width, not just its typedef'd name).
##
## KF_DBG_BRIDGE_ENABLE / KF_DBG_INPUT_INJECT_ENABLE (ADR 0030) are NOT set
## here -- both default to 1 in kf_dbg_bridge.h, which is correct for this
## pre-release developer firmware. A SHIPPING build removes the bridge (and
## therefore remote button injection) entirely by adding, right here:
##     target_compile_definitions(${COMPONENT_LIB} PRIVATE KF_DBG_BRIDGE_ENABLE=0)
## which compiles kf_dbg_bridge_init()/_frame()/_shutdown() down to empty
## functions and kf_dbg_input_mask() down to an unconditional `return 0` --
## see kf_dbg_bridge.h's own comment on that flag. Setting only
## KF_DBG_INPUT_INJECT_ENABLE=0 keeps PING/SHOT/STATE (read-only
## introspection) while refusing remote control of the buttons, for a
## build that wants the former without the latter. A build that removes
## the bridge entirely this way also loses ADVANCE/RESET/MULT below, since
## they are KFDBG commands like any other -- KF_PET_SESSION_ENABLE_DEBUG_
## CONTROLS staying on costs nothing by itself in that configuration, it
## simply has no caller left.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    KF_PET_SESSION_ENABLE_DEBUG_CONTROLS=1
    KF_PET_SESSION_ENABLE_DEBUG_TOOLS=0
    LUA_32BITS
)
