## SPDX-License-Identifier: Apache-2.0
## Copyright the Kamiframe contributors.
##
## The ESP-IDF component LVGL "earns" on this port -- ../main/CMakeLists.txt's
## own comment on why kf_pet_session.cpp did NOT get this treatment says so
## directly: a plain file with no third-party dependency of its own gets
## added straight into main's SRCS; a module that pulls in an external
## library gets a real component, the way hakoniwaos itself does. See
## docs/architecture/adr-0027-lvgl-on-esp32.md.
##
## Two things live here, kept deliberately separate:
##
##   1. LVGL itself, fetched via the SAME cmake/fetch_lvgl.cmake the desktop
##      build already uses -- same pinned tag, same curated lv_conf.h (see
##      that file's own header comment on why every path in it is anchored
##      on its OWN directory now, not the including file's). One LVGL
##      configuration for both targets, not two that can drift apart.
##
##   2. The portable LVGL port-glue sources (kf_lvgl_port.cpp and friends) --
##      NOT copied here. They are compiled straight out of
##      simulator/src/lvgl/ by relative path, the same "one canonical file,
##      not a fork" rule ../main/CMakeLists.txt already applies to
##      kf_pet_session.cpp. kf_lvgl_pointer.cpp (the desktop mouse cursor)
##      is deliberately NOT in the list below -- see kf_lvgl_port.cpp's own
##      #ifndef ESP_PLATFORM guard around the one place that would have
##      needed it.

## fetch_lvgl.cmake (FetchContent_Declare/MakeAvailable) is included AFTER
## idf_component_register() below, not before -- found the hard way, not
## guessed: ESP-IDF runs a lightweight "requirements scan" pass over every
## candidate component's CMakeLists.txt (tools/cmake/scripts/component_get_
## requirements.cmake) to discover REQUIRES/PRIV_REQUIRES before the real
## configure runs. That pass defines idf_component_register() as a MACRO
## that captures its REQUIRES arguments and immediately return()s -- so
## everything in this file AFTER that call is skipped during the scan, but
## everything BEFORE it still runs, in that pass's own restricted CMake
## execution context. FetchContent_Declare's internal define_property()
## call isn't valid there (`CMake Error ... define_property command is not
## scriptable`), so calling it above idf_component_register() breaks
## `idf.py set-target` outright, before a single line of C++ compiles. See
## docs/architecture/adr-0027-lvgl-on-esp32.md's "Found after delivery"
## section.

set(KAMIFRAME_LVGL_PORT_SRC_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../../../simulator/src/lvgl")

## ADR 0045 (Task 2 of docs/superpowers/plans/2026-08-13-screens-clock-
## sleep.md): kf_pet_info_screen.cpp is deleted (Info moved to a
## kf.screen() group over the retained scene) and kf_screen_nav.cpp/
## kf_lua_home_screen.cpp/kf_error_banner.cpp moved OUT of this component
## entirely, into ../main/CMakeLists.txt's own SRCS (compiled straight from
## simulator/src/pet/, the same one-canonical-copy-by-relative-path pattern
## kf_pet_session.cpp already used there) -- none of the three ever had an
## LVGL dependency of its own; they only lived here because kf_screen_
## nav.cpp used to #include <lvgl.h>. This component -- and therefore this
## whole directory's CMakeLists.txt -- now only exists at all when built
## with -DKF_ENABLE_LVGL=ON (../CMakeLists.txt only adds this directory to
## EXTRA_COMPONENT_DIRS in that case; see that file's own comment).
idf_component_register(
    SRCS
        "${KAMIFRAME_LVGL_PORT_SRC_DIR}/kf_lvgl_pool.cpp"
        "${KAMIFRAME_LVGL_PORT_SRC_DIR}/kf_lvgl_tick.cpp"
        "${KAMIFRAME_LVGL_PORT_SRC_DIR}/kf_lvgl_display.cpp"
        "${KAMIFRAME_LVGL_PORT_SRC_DIR}/kf_lvgl_input.cpp"
        "${KAMIFRAME_LVGL_PORT_SRC_DIR}/kf_lvgl_port.cpp"
        "${KAMIFRAME_LVGL_PORT_SRC_DIR}/kf_pet_screen.cpp"
    INCLUDE_DIRS "${KAMIFRAME_LVGL_PORT_SRC_DIR}"
    REQUIRES hakoniwaos
)

## No REQUIRES/PRIV_REQUIRES on `main` above, and that absence is considered,
## not missed: kf_pet_screen.cpp #include "../pet/kf_pet_session.h" by
## relative path, which the compiler resolves against its own real location
## (simulator/src/lvgl/) with no include-dir help needed -- but that
## header's DEFINITIONS live in kf_pet_session.cpp, compiled straight into
## ../main (see that file's own comment on why). Requiring main back from
## here would be circular (main already REQUIRES this component, under
## -DKF_ENABLE_LVGL=ON, to reach kf_lvgl_port_init()); nothing in this
## component needs to compile against main's headers, only to have main's
## symbols present at final link time, which ESP-IDF's ordinary component
## link order already guarantees.

# Now safe to include fetch_lvgl.cmake at all: idf_component_register()
# above already return()'d out of the requirements-scan pass before reaching
# this line -- see this file's header comment. But LVGL v9.2.2's OWN
# CMakeLists.txt (env_support/cmake/esp.cmake vs .../custom.cmake) branches
# on ESP_PLATFORM too, and found the hard way (a real `idf.py set-target`
# configure failure, not guessed): its ESP_PLATFORM branch calls
# idf_component_register() a SECOND time, itself, from inside the
# add_subdirectory() FetchContent_MakeAvailable(lvgl) below triggers --
# which inherits THIS component's own "current component" context (a
# directory-scoped property, and this is a child scope of it) rather than
# establishing a distinct one, so it tries to register another component
# under the exact same name and CMake fails with "add_library cannot
# create target ... because another target with the same name already
# exists." LVGL was never meant to be fetched this way underneath an
# already-registered component; ESP-IDF's own supported path for that is
# either the component registry (idf_component.yml) or listing its source
# directory directly in EXTRA_COMPONENT_DIRS, and adopting either would
# mean giving up the ONE thing this fetch is for -- reusing cmake/fetch_
# lvgl.cmake as-is, so both builds share one pinned tag and one curated
# lv_conf.h. Locally shadowing ESP_PLATFORM to OFF for exactly this
# include() instead routes LVGL down custom.cmake's plain add_library(lvgl
# ...) path -- the SAME path the desktop build already takes, since it
# never has ESP_PLATFORM set at all -- which is exactly what fetch_lvgl.
# cmake was written against; nothing about the fix below is ESP32-specific,
# it just stops LVGL from noticing it's being built for ESP32 for the
# narrow duration of its own fetch. A plain (non-CACHE) set() shadows only
# this directory and its children (FetchContent's own add_subdirectory,
# nothing else, and nothing sibling or upstream) -- restored right after so
# nothing downstream of this file ever sees the shadowed value. See
# docs/architecture/adr-0027-lvgl-on-esp32.md's "Found after delivery"
# section for how this was actually diagnosed.
set(KAMIFRAME_LVGL_PORT_SAVED_ESP_PLATFORM "${ESP_PLATFORM}")
set(ESP_PLATFORM OFF)
include("${CMAKE_CURRENT_LIST_DIR}/../../../cmake/fetch_lvgl.cmake")
set(ESP_PLATFORM "${KAMIFRAME_LVGL_PORT_SAVED_ESP_PLATFORM}")
unset(KAMIFRAME_LVGL_PORT_SAVED_ESP_PLATFORM)

# The circular link lv_conf.h's LV_MEM_POOL_ALLOC hook requires: LVGL calls
# kf_lvgl_mem_pool_alloc() (defined in kf_lvgl_pool.cpp, in ${COMPONENT_LIB})
# exactly once, at lv_init() time; everything else in ${COMPONENT_LIB} calls
# into LVGL. Identical shape and identical reasoning to simulator/CMakeLists.
# txt's own kamiframe_lvgl_port <-> lvgl link -- see the comment there. This
# is also what makes kf_lvgl_pool.h (LV_MEM_POOL_INCLUDE) reachable to LVGL's
# own C compilation: the PRIVATE link below pulls in ${COMPONENT_LIB}'s
# public INCLUDE_DIRS (this directory) for lvgl's own sources, the same
# mechanism, not a second one invented for ESP-IDF.
target_link_libraries(${COMPONENT_LIB} PUBLIC lvgl)
target_link_libraries(lvgl PRIVATE ${COMPONENT_LIB})

# fetch_lvgl.cmake's own last block already suppresses warnings on the
# `lvgl` target (`target_compile_options(lvgl PRIVATE -w)`) -- LVGL is
# vendored C, not held to this project's own -Werror bar, on either build.
# Nothing to repeat here.
