# Architecture decision records

Short notes explaining why HakoniwaOS is built the way it is. Each one names
the decision, what else was considered, and what it would cost to change.

They exist for two readers: a contributor asking "why is it like this," and
future us in month eight asking the same question with less patience.

| # | Decision | Status |
|---|---|---|
| [0001](adr-0001-language.md) | C++17, no exceptions, no RTTI, C-compatible HAL headers | Accepted |
| [0002](adr-0002-build-system.md) | One source tree, two build systems, via `ESP_PLATFORM` | Accepted |
| [0003](adr-0003-sdl3.md) | SDL3 for the desktop backend | Accepted |
| [0004](adr-0004-hal-surface.md) | What belongs in the HAL and what does not | Accepted |
| [0005](adr-0005-hal-dispatch.md) | Compile-time backend selection, runtime capabilities | Accepted |
| [0006](adr-0006-constraint-enforcement.md) | Where the device's limits are enforced | Accepted |
| [0007](adr-0007-frame-loop-ownership.md) | The backend owns the loop, not the core | Accepted |
| [0008](adr-0008-memory-model.md) | Fixed arenas, no heap in core | Accepted |
| [0009](adr-0009-transfer-cost.md) | Modelling display transfer cost on desktop | Accepted |
| [0010](adr-0010-bitmap-text.md) | Bitmap text and the constraint HUD | Accepted |
| [0011](adr-0011-dirty-rect-list.md) | A dirty-rectangle list, not a single box | Accepted |
| [0012](adr-0012-storage-and-power.md) | Save state and deep sleep | Accepted |
| [0013](adr-0013-lvgl-for-menus.md) | LVGL for menus, the custom engine for the pet | Accepted |
| [0014](adr-0014-lua-embedding.md) | Embedding Lua: version, sandboxing, the arena-backed allocator | Accepted |
| [0015](adr-0015-pet-simulation-framework.md) | The pet simulation framework: needs, decay, offline fast-forward | Accepted |
| [0016](adr-0016-lua-pet-binding.md) | The `pet.*` Lua binding, and a pet session to bind it to | Accepted |
| [0017](adr-0017-pet-screen.md) | The pet screen | Accepted |
| [0018](adr-0018-demo-creature-script.md) | The demo creature script | Accepted |
| [0019](adr-0019-esp-idf-hello-world.md) | An ESP-IDF hello-world, and what it actually proves | Accepted |
| [0020](adr-0020-esp32-hal-backends.md) | Real ESP32 HAL backends, compile-verified but not hardware-verified | Accepted |
| [0021](adr-0021-life-stages-and-evolution.md) | Life stages and evolution | Accepted |
| [0022](adr-0022-menu-screen-navigation.md) | Menu/screen navigation | Accepted |
| [0023](adr-0023-personality-traits.md) | Personality traits | Accepted |
| [0024](adr-0024-bringup-pinout-and-diagnostic.md) | The bring-up pinout, and a diagnostic separate from the firmware | Accepted |
| [0025](adr-0025-esp32-pet-session.md) | The pet session, running for real on ESP32 | Accepted |
| [0026](adr-0026-ds3231-rtc-driver.md) | A real DS3231 RTC driver, closing the wall-clock gap | Accepted |
| [0027](adr-0027-lvgl-on-esp32.md) | LVGL on ESP32: the pet screen, on the real panel | Accepted |
| [0028](adr-0028-esp32-lua-port.md) | Lua on ESP-IDF, and the demo creature driving the pet for real | Accepted |
| [0029](adr-0029-panel-profiles.md) | Panel profiles: one display driver, many panels | Accepted |
| [0030](adr-0030-serial-debug-bridge.md) | The KFDBG serial debug bridge: see and drive the device over USB, no camera | Accepted |
| [0031](adr-0031-kfdbg-time-control.md) | Time control over KFDBG, and splitting the pet session's debug flag in two | Accepted |

Open evaluations, deliberately not decided:

- **The creature class name.** Deferred by the naming decision record.

LVGL and Lua were both open evaluations here once; ADR 0013 and ADR 0014
each decided one, so neither is listed above anymore.

See also `../frame-budget.md`, which works through what the display bandwidth
means for full-screen animation and what the options are for 60fps, and
`../sdk-style-guide.md`, which names the design principle behind `pet.*`/
`kf.*`'s function-per-action shape for whatever Lua bindings get added next.

The full option space behind these, including what was rejected, is in
`08-phase1-slice1-decisions.md` in the planning folder.

**This index had drifted out of date before this edit** -- it stopped at
ADR 0009 while the directory already held files through ADR 0025. Rows
0010-0025 above are being added back in the same pass that adds 0026, not
because anything about ADR 0026 required it, just because a broken index is
worse than a slightly oversized diff to fix it.
