## SPDX-License-Identifier: Apache-2.0
## Copyright the Kamiframe contributors.
##
## Get Lua. See docs/architecture/adr-0014-lua-embedding.md.
##
## Lua ships a Makefile, not a CMakeLists.txt, so unlike fetch_sdl.cmake and
## fetch_lvgl.cmake FetchContent_MakeAvailable() here only populates the
## source -- there is no subdirectory for it to add, and it does not error
## for that, which is the documented, supported way to vendor a
## Makefile-only C library through FetchContent.
##
## The static library below is built from an explicit file list, the same
## discipline hakoniwaos/sources.cmake already uses and for the same reason:
## CMake does not notice a globbed directory gaining a file, so a glob would
## silently leave new Lua source out of the build on the next upstream bump.
## The list below is Lua's own upstream `makefile`'s CORE_O + AUX_O + LIB_O,
## minus ltests.c (Lua's own internal test harness, guarded behind macros
## this project never defines, and never shipped by any real embedder) and
## minus lua.c / luac.c (the standalone interpreter and compiler mains --
## this project embeds Lua, it does not ship either program).

set(KAMIFRAME_LUA_TAG "v5.5.0" CACHE STRING
    "Lua git tag to build")

message(STATUS "Lua: fetching ${KAMIFRAME_LUA_TAG} (first build is slow)")

include(FetchContent)

FetchContent_Declare(lua_upstream
    GIT_REPOSITORY https://github.com/lua/lua.git
    GIT_TAG        ${KAMIFRAME_LUA_TAG}
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
)
FetchContent_MakeAvailable(lua_upstream)

set(KAMIFRAME_LUA_SRCS
    # Core (CORE_O in Lua's own makefile, minus ltests.c).
    lapi.c lcode.c lctype.c ldebug.c ldo.c ldump.c lfunc.c lgc.c llex.c
    lmem.c lobject.c lopcodes.c lparser.c lstate.c lstring.c ltable.c
    ltm.c lundump.c lvm.c lzio.c
    # Auxiliary library (AUX_O). luaL_openselectedlibs, the sandboxing
    # mechanism kf_lua_port.cpp actually uses, lives here.
    lauxlib.c
    # Standard libraries (LIB_O). Compiling all of them in is not the same
    # as any script being able to reach all of them: kf_lua_port.cpp calls
    # luaL_openselectedlibs with an explicit bitmask, and io/os/debug/
    # package are deliberately never in it -- see ADR 0014. Their luaopen_*
    # functions still end up in the binary (linit.c's registration table
    # references them unconditionally), unreachable dead code from any
    # running script's point of view, not a live sandbox hole.
    lbaselib.c ldblib.c liolib.c lmathlib.c loslib.c ltablib.c lstrlib.c
    lutf8lib.c loadlib.c lcorolib.c linit.c
)
list(TRANSFORM KAMIFRAME_LUA_SRCS PREPEND "${lua_upstream_SOURCE_DIR}/")

add_library(lua_core STATIC ${KAMIFRAME_LUA_SRCS})
target_include_directories(lua_core PUBLIC "${lua_upstream_SOURCE_DIR}")

# Single-precision lua_Number, matching the S3's single-precision FPU --
# doubles are emulated in software there and slow, the same reasoning
# already applied to the simulation's own maths (see the "Fixed-point vs
# float" note in docs' decision log). luaconf.h reads LUA_FLOAT_TYPE and
# otherwise defaults to double; this is Lua's own documented override
# mechanism (a command-line -D, not a source edit), so there is nothing to
# maintain against upstream drift. See ADR 0014.
target_compile_definitions(lua_core PRIVATE
    LUA_FLOAT_TYPE=LUA_FLOAT_FLOAT)

# Deliberately NOT defining LUA_BUILD_AS_DLL: Lua's own luaconf.h only
# decorates LUA_API with __declspec(dllimport/dllexport) when that macro is
# set, and falls back to plain `extern` otherwise -- exactly the
# undecorated, no-DLL-indirection behaviour this project's static-only
# build needs. Worth saying explicitly after the LVGL slice's MSVC C2099
# saga (ADR 0013): that bug was LVGL's own CMakeLists applying the
# decoration unconditionally under MSVC regardless of static-vs-shared, and
# a circular link reflecting it back onto LVGL's own compilation. Lua's
# upstream default here already does the safe thing without kamiframe
# needing to fix anything, but it is exactly the same category of trap, so
# it gets called out rather than assumed.

# Vendored C, not ours to hold to this project's own -Werror bar -- same
# reasoning as fetch_lvgl.cmake's identical line.
target_compile_options(lua_core PRIVATE -w)
