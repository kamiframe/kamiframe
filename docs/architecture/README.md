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
| [0032](adr-0032-panel-tearing-and-selection.md) | Screen tearing, what we tried, and what the kit's panel must expose | Accepted |
| [0033](adr-0033-asset-pipeline.md) | The asset pipeline: sprites packed into flash, memory-mapped, not copied | Accepted |
| [0034](adr-0034-kfdbg-care-and-stage-jump.md) | KFDBG parity for care actions and life-stage jump | Accepted |
| [0035](adr-0035-kfdbg-mutate-gate.md) | Splitting KFDBG by observe vs. mutate | Accepted |
| [0036](adr-0036-frame-counter-window.md) | The frame budget counters count the frame the port actually drew | Accepted |
| [0039](adr-0039-panel-read-line-and-backlight.md) | The panel profile owns the read line, and something turns the backlight on | Accepted |
| [0040](adr-0040-retained-scene.md) | A retained scene and a coalescing differ, in Core | Accepted |
| [0041](adr-0041-lua-drawing-binding.md) | The Lua binding over the retained scene | Accepted |
| [0042](adr-0042-lua-home-screen-parity.md) | The Lua home screen, and the parity check that judges it | Accepted |
| [0043](adr-0043-lua-home-default.md) | `kf_scene_force_repaint()`, and `KF_HOME_SCREEN` defaults to `lua` | Accepted |
| [0044](adr-0044-lua-screen-groups.md) | `kf.screen()` — named object groups over one shared scene, and a registry that learns names | Accepted |
| [0045](adr-0045-info-screen-in-lua.md) | Info leaves LVGL for a `kf.screen()` group, and LVGL leaves the default build | Accepted |
| [0046](adr-0046-core-civil-clock.md) | `kf/clock.h` — civil time in Core, integers only, no offset | Accepted |
| [0047](adr-0047-lua-time-api-and-settings-screen.md) | The Lua time API, and the Settings screen | Accepted |

(0037 and 0038 do not exist — skipped numbers, not a gap in this list.)

Open evaluations, deliberately not decided:

- **The creature class name.** Deferred by the naming decision record.

LVGL and Lua were both open evaluations here once; ADR 0013 and ADR 0014
each decided one, so neither is listed above anymore. LVGL's own decision
has itself moved since — see ADR 0013's "Superseded in part" section and
`CLAUDE.md`'s "Already decided" section.

See also `../frame-budget.md`, which works through what the display bandwidth
means for full-screen animation and what the options are for 60fps, and
`../sdk-style-guide.md`, which names the design principle behind `pet.*`/
`kf.*`'s function-per-action shape for whatever Lua bindings get added next.

The full option space behind these, including what was rejected, was
described as living in `08-phase1-slice1-decisions.md` in the planning
folder — **flagged, not resolved: that file does not exist anywhere in
this repo, and is not among the planning docs `CLAUDE.md` names
(`02-min-spec-sheet.md`, `04-roadmap-diy-release.md`,
`07-naming-decision-and-setup.md`).** It may exist under a different name
in the external planning folder (`D:\Game Development Repos\Virtual Pet
Experiments`, per `CLAUDE.md`), may have been renamed, or may never have
existed under this name — worth Chris confirming rather than guessing here.

**This index has drifted out of date before, more than once.** It previously
stopped at ADR 0009 while the directory held files through ADR 0025, and
was caught and fixed then. This pass catches the same drift again — it had
stopped at ADR 0035 while the directory held files through ADR 0046, eight
ADRs missing (0036, 0039-0046). Whoever next lands an ADR: add its row here
in the same commit, not "later" — a broken index is worse than a slightly
oversized diff to fix it, and this is now the second time that lesson
needed relearning.
