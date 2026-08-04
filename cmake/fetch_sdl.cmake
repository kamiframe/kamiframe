## SPDX-License-Identifier: Apache-2.0
## Copyright the Kamiframe contributors.
##
## Get SDL3.
##
## Order: a system SDL3 if one is installed, otherwise fetch and build a pinned
## version from source.
##
## FetchContent is the default on purpose. It means `cmake -B build` works on a
## clean machine with nothing installed but a compiler and CMake, which is the
## difference between someone trying this project and someone bouncing off a
## README full of package-manager instructions. The cost is a slow first build.
##
## To use a local checkout instead (offline, or while debugging SDL):
##     cmake -B build -DFETCHCONTENT_SOURCE_DIR_SDL3=/path/to/SDL

set(KAMIFRAME_SDL3_TAG "release-3.4.8" CACHE STRING
    "SDL3 git tag to build when no system SDL3 is found")

find_package(SDL3 3.2 QUIET CONFIG)

if(SDL3_FOUND)
    message(STATUS "SDL3: using system installation")
else()
    message(STATUS "SDL3: fetching ${KAMIFRAME_SDL3_TAG} (first build is slow)")

    include(FetchContent)

    # Static, and only the parts a virtual pet simulator needs. Keeps the
    # build shorter and the binary smaller.
    set(SDL_SHARED     OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC     ON  CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES   OFF CACHE BOOL "" FORCE)
    set(SDL_INSTALL    OFF CACHE BOOL "" FORCE)

    # ------------------------------------------------------------------
    # Linux, WSL2 and CI: switch off X11 extensions this project cannot use.
    #
    # SDL turns these on by default and then FAILS THE CONFIGURE STEP if the
    # matching -dev package is absent, which turns a first build into a game
    # of installing one package, re-running, and finding the next one.
    #
    # None of them do anything for a 240x320 window that draws one texture:
    #   XSCRNSAVER  inhibits the screensaver during fullscreen playback
    #   XTEST       synthesises fake input events, for automation tools
    #   XDBE        double buffering, unused since we render through a texture
    #
    # Anyone who wants them back can pass -DSDL_X11_XTEST=ON.
    # Audio backends (ALSA, PulseAudio, JACK, PipeWire) are left alone: SDL
    # only warns when those are missing, and audio is coming later.
    # ------------------------------------------------------------------
    if(UNIX AND NOT APPLE)
        set(SDL_X11_XSCRNSAVER OFF CACHE BOOL "" FORCE)
        set(SDL_X11_XTEST      OFF CACHE BOOL "" FORCE)
        set(SDL_X11_XDBE       OFF CACHE BOOL "" FORCE)
    endif()

    FetchContent_Declare(SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG        ${KAMIFRAME_SDL3_TAG}
        GIT_SHALLOW    TRUE
        GIT_PROGRESS   TRUE
    )
    FetchContent_MakeAvailable(SDL3)
endif()
