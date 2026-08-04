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

Open evaluations, deliberately not decided:

- **The creature class name.** Deferred by the naming decision record.

See also `../frame-budget.md`, which works through what the display bandwidth
means for full-screen animation and what the options are for 60fps.

The full option space behind these, including what was rejected, is in
`08-phase1-slice1-decisions.md` in the planning folder.
