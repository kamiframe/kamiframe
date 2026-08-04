# ADR 0001: C++17, no exceptions, no RTTI, C-compatible HAL headers

**Status:** Accepted, 2026-08-04
**Reversal cost:** High. Touches every file.

## Decision

- **C++17** for everything, pinned identically in both builds.
- **Exceptions and RTTI off** on desktop as well as the device.
- **HAL headers written in a subset a C compiler accepts**, enforced by a
  build target that compiles them as C17.

## Why C++ rather than C

The deciding argument is not RAII or templates. It is `constexpr` and
`static_assert`. The constraint enforcement in ADR 0006 wants to be
compile-time wherever it can be, and C's version of that is preprocessor
macros. Secondary: `string_view`, `std::array` and range-for remove the
string and array handling that hurts people coming from higher-level
languages.

Considered and rejected: C17 (simpler, matches every ESP-IDF example and
Lua and LVGL themselves, but loses the compile-time machinery); C++20 and
above (ESP-IDF v6 actually defaults to `gnu++26`, so it is available, but
narrows the set of compilers a contributor can use); Rust (would prevent
real bug classes, but Lua and LVGL both become FFI projects and the Xtensa
toolchain is a fork); Zig (pre-1.0, almost no ESP32 prior art).

## Why exceptions and RTTI are off everywhere

ESP-IDF disables both by default: they cost binary size and unpredictable
latency on a microcontroller. If the desktop build left them on, months of
error handling would be written that simply does not compile for the device.
This is the same principle as capping the framebuffer at 240x320. Match the
device now, not later.

Set in `cmake/kamiframe_options.cmake`, which every target links.

## Why the HAL headers must compile as C

Not because anyone will write C, but because it is a mechanical, checkable
definition of "this interface is narrow." A header a C compiler accepts
cannot contain templates, overloads, default arguments, references, or
classes with behaviour. `hakoniwaos_hal_c_check` in the build compiles every
`kf/hal/*.h` as C17 and fails the build if the boundary widens.

Cost: `extern "C"`, plain structs, and `kf_` prefixes instead of namespaces.
Paid at the boundary only, never above it.

## Consequences

- A written subset rule is needed for core: no `std::function`, no hidden
  allocation, care with static initialisation order. Partly enforced by
  ADR 0008.
- Reading ESP-IDF examples means mentally translating C to C++. Accepted.
