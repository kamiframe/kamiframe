# ADR 0002: One source tree, two build systems

**Status:** Accepted, 2026-08-04
**Reversal cost:** Medium. Mechanical but tedious.

## Problem

ESP-IDF uses CMake, but not normal CMake. Each library must be a "component":
a directory whose *name* is the component name, containing a `CMakeLists.txt`
that calls `idf_component_register(...)`. The project file includes
`$ENV{IDF_PATH}/tools/cmake/project.cmake` before `project()`, which rewrites
a great deal of standard behaviour. You cannot `add_subdirectory()` your way
in.

Meanwhile the desktop build wants to be ordinary CMake that works on a clean
machine.

## Decision

Each module has **one** `CMakeLists.txt` that branches on `ESP_PLATFORM`
(which ESP-IDF defines), with the source list in a **separate `sources.cmake`
above the branch**, so it exists exactly once.

```cmake
include(${CMAKE_CURRENT_LIST_DIR}/sources.cmake)   # sets HAKONIWAOS_SRCS
if(ESP_PLATFORM)
  idf_component_register(SRCS ${HAKONIWAOS_SRCS} INCLUDE_DIRS include ...)
else()
  add_library(hakoniwaos STATIC ${HAKONIWAOS_SRCS})
  ...
endif()
```

The desktop build starts at the repository root. The ESP-IDF build starts at
`ports/esp32/` and sets `EXTRA_COMPONENT_DIRS` back at the same module
directories.

Considered and rejected: two build trees with duplicated source lists
(guaranteed drift, the named risk); `file(GLOB ...)` (CMake does not re-run
when a directory gains a file, so new code silently leaves the build);
ESP-IDF's `IDF_TARGET=linux` as the only build system (drags all of ESP-IDF
onto the desktop, awkward on Windows, designed for headless component tests
rather than an SDL window; still worth revisiting for unit tests later);
generating both from a third source (adds a step nobody else has).

## Constraints this imposes, permanently

1. **Module directory names are ESP-IDF component names.** Lowercase, no
   spaces, and unique against every component inside ESP-IDF itself. This is
   one of the places the `hakoniwaos` naming rule is load-bearing rather than
   cosmetic: a directory named `driver`, `log`, `console` or `main` would
   collide.
2. **Components cannot nest.** The module list stays flat.
3. **`REQUIRES` and `target_link_libraries` are kept in sync by hand.** No
   mechanism exists. Keep the dependency graph small enough that it does not
   matter.

## Dependencies

SDL3 comes from `FetchContent` with a pinned tag, falling back to
`find_package` if a system SDL3 exists. Reason: `cmake -B build` works on a
clean machine with only a compiler and CMake. Every alternative is a README
step contributors get wrong. Cost is a slow first build, then cached.
