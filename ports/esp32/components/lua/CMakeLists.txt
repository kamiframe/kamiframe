## SPDX-License-Identifier: Apache-2.0
## Copyright the Kamiframe contributors.
##
## Lua for the ESP-IDF build. See docs/architecture/adr-0028-esp32-lua-port.md
## and ADR 0014 (the original embedding decision: version, float precision,
## sandboxing).
##
## Does NOT duplicate cmake/fetch_lua.cmake's file list by hand -- it
## INCLUDEs that file directly (same "one canonical file, not a fork"
## discipline this port already uses for kf_pet_session.cpp; see ../../main/
## CMakeLists.txt's own comment) and reuses the `KAMIFRAME_LUA_SRCS` variable
## it builds, rather than retyping Lua's CORE_O+AUX_O+LIB_O list a second
## time. include() runs in this file's own scope, not a fresh one, so that
## variable -- and fetch_lua.cmake's FetchContent_Declare/MakeAvailable call,
## fetching the exact same pinned Lua 5.5.0 tag desktop builds -- are both
## still visible below.
##
## What is NOT reused: fetch_lua.cmake's own `add_library(lua_core ...)`.
## idf_component_register() below compiles KAMIFRAME_LUA_SRCS again, into
## THIS component's own archive -- see "idf_component_register(), not
## lua_core" below for why that second compile is the correct outcome here,
## not an accident. The orphan `lua_core` target fetch_lua.cmake still
## creates is excluded from the build entirely (EXCLUDE_FROM_ALL) so its
## sources are not compiled twice for nothing.
##
## CMAKE_BUILD_EARLY_EXPANSION, found the hard way (a real `idf.py build`
## configure failure, not guessed): ESP-IDF evaluates every component's
## CMakeLists.txt TWICE -- once in a lightweight "requirements expansion"
## pass that only wants to know this component's REQUIRES/PRIV_REQUIRES,
## and again for real. FetchContent_Declare's modern implementation calls
## define_property(), which CMake refuses to run in the restricted "script
## mode" that first pass uses ("define_property command is not scriptable"),
## so fetch_lua.cmake's include() below must not run during that pass at
## all -- ESP-IDF sets CMAKE_BUILD_EARLY_EXPANSION true specifically so a
## component can tell the two passes apart and skip exactly this kind of
## real work on the first one. idf_component_register() itself is always
## safe unguarded (it is what the early pass is trying to read), so it is
## the only line outside the guard below.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    include(${CMAKE_CURRENT_LIST_DIR}/../../../../cmake/fetch_lua.cmake)
    set_target_properties(lua_core PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()

## idf_component_register(), not lua_core: found the hard way (a real
## `idf.py build` LINK failure, not guessed). An earlier version of this
## file registered an otherwise-empty component and exposed lua_core to
## `main` via `target_link_libraries(${COMPONENT_LIB} INTERFACE lua_core)`
## -- lua_core being a plain CMake target, not an ESP-IDF component, is
## invisible to ESP-IDF's own component-ordering machinery (the thing that
## decides the final link line's archive order, and repeats archives to
## resolve real circular REQUIRES -- see ../../main/CMakeLists.txt's
## WHOLE_ARCHIVE comment for the general category). CMake's own usage-
## requirement propagation instead tacked liblua_core.a onto the very END
## of the link command, after even ESP-IDF's `-Wl,--wrap=longjmp` and the
## `libcxx.a` that defines `__wrap_longjmp` for it -- so `ld`'s single left-
## to-right archive scan had already finished with `libcxx.a` by the time
## `ldo.c.obj` (Lua's C error/coroutine core, which calls `longjmp`) asked
## for that symbol, and the link failed on "undefined reference to
## __wrap_longjmp": a real symbol, defined in a real archive, simply
## scanned before anything needed it.
##
## Compiling KAMIFRAME_LUA_SRCS straight into THIS component's own SRCS
## instead sidesteps the whole class of problem: `main REQUIRES lua` (see
## ../../main/CMakeLists.txt) is then an ordinary ESP-IDF component
## dependency, positioned in the link line by the same machinery that
## already places `hakoniwaos` and every HAL-owning component correctly.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    idf_component_register(
        SRCS ${KAMIFRAME_LUA_SRCS}
        INCLUDE_DIRS "${lua_upstream_SOURCE_DIR}"
    )

    ## Exactly fetch_lua.cmake's own lines, repeated here because they are
    ## target_compile_definitions/target_compile_options calls -- CMake
    ## commands, not data -- and so cannot be "reused" the way the
    ## KAMIFRAME_LUA_SRCS variable above was; ${COMPONENT_LIB} (this
    ## component's real archive) is a different target than lua_core (now
    ## excluded from the build) and needs its own copy of both.
    ##
    ## LUA_32BITS, on top of LUA_FLOAT_TYPE: desktop's g++ defines LLONG_MAX
    ## from a plain <limits.h> include just fine, so lua_core there keeps
    ## LUA_INTEGER as `long long` (LUA_INT_DEFAULT, luaconf.h) undisturbed --
    ## verified working, ADR 0014. This toolchain's C++ mode does not:
    ## xtensa-esp-elf's own bundled limits.h fallback (reached because
    ## ESP-IDF's picolibc limits.h defers to it via #include_next) only
    ## defines LLONG_MAX inside an `#if defined __STDC_VERSION__` guard, a
    ## C-only macro never set for a C++ translation unit -- confirmed
    ## directly (a one-line #ifdef LLONG_MAX probe compiles under `-x c`,
    ## fails under `-x c++`, same flags, same toolchain). luaconf.h's own
    ## #error names exactly this class of compiler as needing
    ## `-DLUA_32BITS`, and that option is a genuine fit here, not just a
    ## workaround: it makes LUA_INTEGER a 32-bit type, matching this CPU's
    ## native word size, the same reasoning that already justified
    ## LUA_FLOAT_TYPE=LUA_FLOAT_FLOAT for lua_Number.
    ##
    ## Must match ../../main/CMakeLists.txt's identical LUA_32BITS define
    ## exactly -- see that file's own comment for why: LUA_INTEGER crosses
    ## the API boundary in lua_pushinteger()/luaL_checkinteger() calls
    ## kf_lua_port.cpp makes directly, so both sides of that boundary must
    ## agree on its real width or the call is genuinely ABI-mismatched, not
    ## just differently-typed source.
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        LUA_FLOAT_TYPE=LUA_FLOAT_FLOAT
        LUA_32BITS
    )

    ## Vendored C, not ours to hold to this project's own -Werror bar --
    ## same reasoning as fetch_lua.cmake's identical line for lua_core.
    target_compile_options(${COMPONENT_LIB} PRIVATE -w)
else()
    idf_component_register()
endif()
