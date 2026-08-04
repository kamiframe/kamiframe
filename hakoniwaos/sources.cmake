## SPDX-License-Identifier: Apache-2.0
## Copyright the Kamiframe contributors.
##
## The source list, defined ONCE, above the build-system fork in
## CMakeLists.txt. Both the host build and the ESP-IDF build read it from
## here, which is the mechanism that stops the two from drifting apart.
##
## Paths are relative to this file's directory so it works from either build.
##
## Sources are listed explicitly, never globbed: CMake does not re-run when a
## globbed directory gains a file, so a glob silently leaves new code out of
## the build.

set(HAKONIWAOS_SRCS
    src/arena.cpp
    src/blit.cpp
    src/framebuffer.cpp
    src/rng.cpp
    src/app.cpp
    src/demo.cpp        # placeholder content, deleted when the app loader lands
)
