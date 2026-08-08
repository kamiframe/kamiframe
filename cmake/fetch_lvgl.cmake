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
##
## Included from two places now (ADR 0027): simulator/CMakeLists.txt (desktop)
## and ports/esp32/kamiframe_lvgl_port/CMakeLists.txt (device) -- the whole
## point being the SAME pinned tag and the SAME curated lv_conf.h reach both,
## so there is exactly one LVGL configuration to reason about, not two that
## can quietly drift apart. That is why every path below is anchored on
## CMAKE_CURRENT_LIST_DIR (this file's own directory, cmake/, fixed
## regardless of who includes it) rather than CMAKE_CURRENT_SOURCE_DIR (the
## INCLUDING file's directory, which used to work only by coincidence: it
## happened to equal simulator/ because simulator/CMakeLists.txt was the only
## caller).

set(KAMIFRAME_LVGL_TAG "v9.2.2" CACHE STRING
    "LVGL git tag to build")

message(STATUS "LVGL: fetching ${KAMIFRAME_LVGL_TAG} (first build is slow)")

include(FetchContent)

# The curated config lives at simulator/src/lvgl/lv_conf.h, next to the port
# glue that uses it. LV_CONF_PATH is a plain #include of this exact file --
# see lv_conf_internal.h -- so this is the whole mechanism, no copying or
# generation step needed.
set(LV_CONF_PATH "${CMAKE_CURRENT_LIST_DIR}/../simulator/src/lvgl/lv_conf.h")
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

# ARM-only SIMD assembly kernels, dropped from the `lvgl` target's SOURCES
# unconditionally -- neither Xtensa (ESP32) nor this project's desktop
# targets (x86-64/ARM64, never Cortex-M) can ever use Helium (an M-profile
# MVE extension) or NEON assembly, so these two files can only ever be dead
# weight, on any build this project produces, forever.
#
# Found on the ESP32 side (ADR 0027), not guessed: `lv_blend_helium.S`
# unconditionally #includes lv_conf_internal.h (a plain C header, not
# assembly-safe -- it was never written expecting to run through `as`) two
# lines above the `#if defined(__ARM_FEATURE_MVE) ...` guard that correctly
# excludes the file's actual SIMD body on any non-ARM target; that guard
# never gets a chance to matter, because parsing lv_conf_internal.h's own
# C declarations as assembly mnemonics fails immediately, before the guard
# is even reached. `lv_blend_neon.S` has the same shape and the same
# unconditional include, so it is pruned for the identical reason, not
# because it was separately confirmed to fail. Desktop's own build has
# never hit this: the root CMakeLists.txt's project() call never enables
# the ASM language (LANGUAGES C CXX only), so CMake has always silently
# never compiled either .S file there -- ESP-IDF's project() enables ASM
# project-wide (its own startup/vector code needs it), which is what
# actually exercises this LVGL v9.2.2 bug for the first time. Pruned here,
# not per-build-system, because the underlying fact (neither target is
# ever ARM) is the same fact either way -- see ADR 0027's "Found after
# delivery" section.
if(TARGET lvgl)
    get_target_property(_kf_lvgl_srcs lvgl SOURCES)
    if(_kf_lvgl_srcs)
        list(FILTER _kf_lvgl_srcs EXCLUDE REGEX
             "blend/(helium|neon)/lv_blend_(helium|neon)\\.[Ss]$")
        set_property(TARGET lvgl PROPERTY SOURCES ${_kf_lvgl_srcs})
    endif()
    unset(_kf_lvgl_srcs)
endif()

# MSVC C2099 fix -- the ACTUAL root cause, confirmed by capturing the real
# cl.exe command line for lv_bar.c via a /verbosity:detailed CI build (see
# "Attempt 5" in docs/architecture/adr-0013-lvgl-for-menus.md). This is not
# a guess: LVGL's own CMakeLists.txt does, unconditionally under MSVC --
#
#     target_compile_definitions(lvgl
#         INTERFACE LV_ATTRIBUTE_EXTERN_DATA=__declspec(dllimport)
#         PRIVATE   LV_ATTRIBUTE_EXTERN_DATA=__declspec(dllexport))
#
# -- intending PRIVATE (dllexport) for lvgl's own sources and INTERFACE
# (dllimport) for whatever links against it, which is correct for a target
# that is only ever a dependency, never a dependent. But simulator/
# CMakeLists.txt's kamiframe_lvgl_port <-> lvgl link is genuinely circular
# (see the comment there): kamiframe_lvgl_port links lvgl PUBLIC, and lvgl
# links kamiframe_lvgl_port PRIVATE right back, to resolve LVGL calling
# kf_lvgl_mem_pool_alloc() at lv_init() time. That second link makes lvgl
# consume kamiframe_lvgl_port's INTERFACE usage requirements for its OWN
# compilation -- and kamiframe_lvgl_port's PUBLIC link to lvgl means those
# requirements include lvgl's own INTERFACE LV_ATTRIBUTE_EXTERN_DATA
# definition, reflected straight back at it. So lvgl ends up compiling
# itself with BOTH definitions on the same command line: its own direct
# PRIVATE dllexport, and a dllimport reflected back through the cycle.
# MSVC's cl.exe applies repeated /D flags left-to-right, later wins -- and
# the reflected one lands last, so every LV_ATTRIBUTE_EXTERN_DATA
# declaration in LVGL's own sources -- including `lv_obj_class` in
# lv_obj.h, referenced as `.base_class = &lv_obj_class` in lv_bar.c,
# lv_button.c, lv_image.c and lv_label.c -- compiles as
# __declspec(dllimport). An imported symbol's address is resolved through
# the Import Address Table at load time, not at compile time, so MSVC
# correctly refuses to treat `&lv_obj_class` as a constant initializer:
# error C2099.
#
# The fix doesn't touch the circular link (it's load-bearing -- see
# simulator/CMakeLists.txt) or LVGL's own CMakeLists.txt (vendored, not
# ours to maintain). It strips LV_ATTRIBUTE_EXTERN_DATA from both of
# lvgl's own compile-definition properties right here, before anything
# downstream links against it -- so there is nothing left for the circular
# link to reflect back, on any platform. This project never builds LVGL as
# a DLL, so the dllimport/dllexport distinction this macro exists for does
# not apply here at all; falling back to LVGL's own default (a plain,
# empty LV_ATTRIBUTE_EXTERN_DATA -- see src/lv_conf_internal.h) is exactly
# what every non-MSVC compiler already does, and is correct on MSVC too.
if(TARGET lvgl)
    get_target_property(_kf_lvgl_defs lvgl COMPILE_DEFINITIONS)
    if(_kf_lvgl_defs)
        list(FILTER _kf_lvgl_defs EXCLUDE REGEX "^LV_ATTRIBUTE_EXTERN_DATA=")
        set_property(TARGET lvgl PROPERTY COMPILE_DEFINITIONS ${_kf_lvgl_defs})
    endif()

    get_target_property(_kf_lvgl_iface_defs lvgl INTERFACE_COMPILE_DEFINITIONS)
    if(_kf_lvgl_iface_defs)
        list(FILTER _kf_lvgl_iface_defs EXCLUDE REGEX "^LV_ATTRIBUTE_EXTERN_DATA=")
        set_property(TARGET lvgl PROPERTY INTERFACE_COMPILE_DEFINITIONS ${_kf_lvgl_iface_defs})
    endif()
    unset(_kf_lvgl_defs)
    unset(_kf_lvgl_iface_defs)
endif()

# Superseded by the fix above -- left disabled, not deleted, so the earlier
# reasoning stays visible in git history instead of vanishing with the
# file. This used to make the four widgets' bit-field initializers
# explicit, on the theory that MSVC couldn't constant-fold a packed
# bit-field word left partially implicit. That theory was wrong: it
# correlated with the C2099 line number (MSVC always reports C2099 at the
# aggregate's own declaration line, never the specific member), but a
# confirmed-applied version of this exact patch (see "Attempt 3" in the
# ADR) produced an identical failure -- which the LV_ATTRIBUTE_EXTERN_DATA
# fix above actually explains: `.base_class = &lv_obj_class` was always
# the real non-constant member, and no bit-field rewrite could ever have
# touched that.
# include("${CMAKE_CURRENT_LIST_DIR}/patches/lvgl_msvc_c2099_fix.cmake")
# kf_lvgl_apply_msvc_fix("${lvgl_SOURCE_DIR}")

# lv_conf.h pulls in kf/budget.h (see the comment in that file) so
# KF_ARENA_LVGL_BYTES can never drift from LV_MEM_SIZE. LVGL's own source is
# compiled as plain C with no notion of hakoniwaos/include, so it needs the
# path added explicitly. CMAKE_CURRENT_LIST_DIR (this file's own directory),
# not CMAKE_CURRENT_SOURCE_DIR -- see this file's header comment.
if(TARGET lvgl)
    target_include_directories(lvgl PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/../hakoniwaos/include")
endif()

# Belt-and-suspenders alongside the top-level CMAKE_C_STANDARD 17 /
# C_STANDARD_REQUIRED ON in the root CMakeLists.txt: confirmed (by
# inspecting the actual compile command for lv_bar.c via
# CMAKE_EXPORT_COMPILE_COMMANDS) that those global settings already reach
# the lvgl target correctly under GCC (`-std=gnu17`). Set explicitly here
# too because MSVC only accepts designated-initializer/bit-field aggregate
# patterns like the one cmake/patches/lvgl_msvc_c2099_fix.cmake works around
# under its newer, more standards-conformant /std:c11 or /std:c17 front end
# -- its default (pre-standard-flag) C mode is the older, less-invested-in
# part of the compiler, and Microsoft's own docs on this exact C2099 error
# suggest exactly this category of gap. Making the requirement explicit,
# target-local, removes any dependency on how a specific toolchain resolves
# the inherited global variable, at zero cost since it only reinforces what
# CMAKE_C_STANDARD already requests.
if(TARGET lvgl)
    set_target_properties(lvgl PROPERTIES
        C_STANDARD          17
        C_STANDARD_REQUIRED ON)
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
