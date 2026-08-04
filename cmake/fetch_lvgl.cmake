## SPDX-License-Identifier: Apache-2.0
## Copyright the Kamiframe contributors.
##
## Get LVGL. See docs/architecture/adr-0013-lvgl-for-menus.md.
##
## Same shape as fetch_sdl.cmake, on purpose: a pinned tag, fetched via
## FetchContent so `cmake -B build` still works on a clean machine with
## nothing installed but a compiler and CMake. No find_package() first here --
## unlike SDL3, a system-installed LVGL would almost certainly be built with
## someone else's lv_conf.h, and this project's curated one (five widgets,
## a PSRAM-backed pool allocator) is the entire point of pulling LVGL in.
## Always build our own from source, configured our way.
##
## To use a local checkout instead (offline, or while debugging LVGL):
##     cmake -B build -DFETCHCONTENT_SOURCE_DIR_LVGL=/path/to/lvgl

set(KAMIFRAME_LVGL_TAG "v9.2.2" CACHE STRING
    "LVGL git tag to build")

message(STATUS "LVGL: fetching ${KAMIFRAME_LVGL_TAG} (first build is slow)")

include(FetchContent)

# The curated config lives at simulator/src/lvgl/lv_conf.h, next to the port
# glue that uses it. LV_CONF_PATH is a plain #include of this exact file --
# see lv_conf_internal.h -- so this is the whole mechanism, no copying or
# generation step needed.
set(LV_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/src/lvgl/lv_conf.h")
set(LV_CONF_INCLUDE_SIMPLE ON CACHE BOOL "" FORCE)

# Only the library itself. The example gallery, the demo apps and ThorVG
# (vector graphics -- C++, and nothing this project's five widgets need) are
# all built by default otherwise, which is a lot of extra compile time for
# code nothing here calls.
set(LV_CONF_BUILD_DISABLE_EXAMPLES ON CACHE BOOL "" FORCE)
set(LV_CONF_BUILD_DISABLE_DEMOS ON CACHE BOOL "" FORCE)
set(LV_CONF_BUILD_DISABLE_THORVG_INTERNAL ON CACHE BOOL "" FORCE)

FetchContent_Declare(lvgl
    GIT_REPOSITORY https://github.com/lvgl/lvgl.git
    GIT_TAG        ${KAMIFRAME_LVGL_TAG}
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
)
FetchContent_MakeAvailable(lvgl)

# lv_conf.h pulls in kf/budget.h (see the comment in that file) so
# KF_ARENA_LVGL_BYTES can never drift from LV_MEM_SIZE. LVGL's own source is
# compiled as plain C with no notion of hakoniwaos/include, so it needs the
# path added explicitly.
if(TARGET lvgl)
    target_include_directories(lvgl PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../hakoniwaos/include")
endif()

# LVGL is a vendored C dependency, not code this project owns. Its own
# internal allocator (lv_malloc, the TLSF pool) references the word "malloc"
# in ways tools/check_no_heap.py's keyword scan would otherwise flag, even
# though it is wired to KF_ARENA_LVGL and never touches the system heap --
# see the "Accepted cost" section of ADR 0013. Warnings-as-errors is this
# project's own bar, not a vendored dependency's; LVGL builds clean on its
# own terms.
if(TARGET lvgl)
    target_compile_options(lvgl PRIVATE -w)
endif()
