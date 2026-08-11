# Kamiframe

Open source virtual pet platform. Open hardware + **HakoniwaOS** + a Lua SDK, console-style, with commercial third-party games explicitly allowed.

---

## How to talk to Chris

**He is a web developer. He has no embedded, systems, C or C++ background.** Do not ask him to arbitrate technical decisions he has no basis for evaluating.

**The split is not "big vs small." It is "needs expertise" vs "needs Chris's preferences."**

**Decide these yourself, don't ask:** language and standard, build system layout, library choices, file and module structure, data formats, dependencies, API shape. Anything with a defensible technical answer. Make the call, give **one plain sentence of reasoning**, move on. Example: *"Using C++ here because the ESP32 tooling expects it and fighting that later costs more than it saves now."*

**Ask him about:** time, money, scope. How the product should look, feel or behave. What to prioritise. Anything irreversible or expensive to undo, described in real consequences rather than technology names. Give a recommendation plus what each option actually means for him.

**When explaining:**
- Translate into web terms where the analogy is honest. He knows JavaScript, npm, browsers, build tooling. A HAL is an adapter layer. CMake is package.json plus a build script. A framebuffer is a canvas you draw into before it reaches the screen.
- Define jargon inline the first time, briefly, without condescension.
- Lead with what it means for him, then the detail only if the detail changes something.
- Tell him he can ask you to unpack anything, and mean it.

**Do not front-load a list of technical questions before starting.** He is a planner, but a plan means "what will exist, in what order, and what will it let me do." Not "which C standard." Give the plan in outcomes, start building, explain choices in context as they come up.

Flag mistakes early and directly. He'd rather hear it now.

---

## Architecture non-negotiables

**1. There is no emulator.** The simulator is the real firmware compiled against a desktop backend of the same HAL the ESP32 build uses. Same sprite engine, same Lua runtime, same simulation code, different bottom layer. Two codebases that mimic each other is a failure state, not a design.

**2. The desktop build enforces the device's real constraints from commit one.** 240×320 RGB565 framebuffer, Lua heap capped to realistic PSRAM, assets budgeted to 12MB of the part's 16MB flash (`kf/budget.h`'s `KF_FLASH_ASSET_BUDGET_BYTES`; the rest is firmware, doubled for OTA), warnings when a frame runs long. Desktop speed lies. Constraint enforcement lives somewhere it can't be accidentally bypassed.

**3. Hardware is real now — bring-up is in progress, not hypothetical.** The trigger this rule used to gate on has already fired: the demo pet ran in the simulator under enforced constraints with save + offline fast-forward working, and an ESP-IDF hello-world booted in Wokwi. Parts were ordered, the board is on the bench, and the care loop runs on it over the debug bridge. Work can now assume hardware on the desk — but the discipline that got us here still applies going forward: don't build ahead of what is proven on the bench, and keep the desktop build as the fast, enforced-constraints loop for anything that doesn't specifically need silicon.

## Naming rules

- **HakoniwaOS is always one token.** Never bare "Hakoniwa" — in prose, folder names (`hakoniwaos/`), URL paths, package names, module identifiers or CLI output. The bare word collides with an existing embedded-sim project and a Rust crate.
- Sanctioned short form in speech is **"Hako."**
- The platform is **Kamiframe**. Individual demo creatures get names ending in **-maru**. The creature *class* name is deliberately undecided; don't invent one.
- Never generate Japanese phrases, taglines or slogans. Single nouns only.

## Target hardware (not the starting point)

ESP32-S3-WROOM-1 N16R8 (16MB flash, 8MB octal PSRAM) · 2.0–2.8" IPS TFT 240×320 ST7789 over SPI+DMA, target 30–60fps · RTC with backup (the pet must age while powered off) · 6-axis IMU · ambient light sensor · buzzer + I2S speaker · haptic motor · 500–1000mAh LiPo, USB-C, <50µA deep sleep · WiFi/BLE/ESP-NOW, IR, microSD, Qwiic I2C.

## Repo layout

Monorepo, Apache 2.0:

```
/hakoniwaos    the OS: HAL, display driver, sprite engine, sleep/wake,
               pet simulation framework (needs, decay, evolution,
               offline fast-forward)
/sdk           Lua game API, packaging CLI
/simulator     desktop HAL backend (SDL) + WASM later
/ports         device backends for the HAL (esp32, esp32-bringup)
/tools         asset pipeline, packing, debug bridge, and dev scripts
/examples      sample creatures
/docs          docs site source
```

Hardware files live in a **separate** `kamiframe/hardware` repo under CERN-OHL-W, so the licenses never blur. Not created yet.

Demo creature code is Apache; its characters and artwork are **not** licensed for reuse. See `LICENSING.md`.

## Don't decide yet

- The creature class name.

## Already decided

- **LVGL vs a custom sprite engine.** Decided: ADR 0013. The retained scene
  engine now drives the whole home screen, and `KF_ENABLE_LVGL` defaults
  OFF on both build systems (Info moved off LVGL onto the scene engine too —
  ADR 0045). LVGL is kept buildable behind that flag as the ADR 0013
  evaluation path, not deleted, but it is off by default.

## If you are the operator, these two things are your job

The operator is whoever is coordinating and dispatching subagents — usually an
Opus-tier session. Subagents cannot do either of these, because both need
context that spans more than one task.

**1. Keep the plan documents true, and fix them the moment they are not.**

Task briefs are *generated from* the plan. So a stale line in a plan is not a
documentation problem — it is re-served to every implementer that follows, and
they copy it faithfully. This has manufactured six defects on this project:
three comments that contradicted their own code, a real `ValueError` in a code
listing, a function called four times against an assert that fires on the
second call, and a requirement to build and persist a config field that a
previous task had deliberately decided not to implement.

So:

- **Update the plan when a decision is made, before dispatching the task it
  affects** — not afterwards, and not by appending amendments to a generated
  brief.
- When a review finds a bad comment or pattern in the source, **grep the plan
  document too.** A defect there costs one defect per remaining task.
- When a task changes a design, **sweep the sections of every later task** that
  described the old one. The task that made the change usually fixes its own
  section and flags the rest; finishing that sweep is the operator's job.
- Code listings in plans get copied verbatim, bugs included. Prefer describing
  a requirement over pasting code nobody has run.

**2. Do not take a subagent's assertions at face value — especially "this is
impossible" or "this cannot happen".**

Subagents report confidently and are sometimes wrong, and a wrong claim that
goes unchallenged becomes a decision. The expensive ones on this project:

- Two agents ran concurrently, each measured the same shared counter, and each
  attributed the other's spend to itself — producing a cost figure wrong by
  **14x** that then shaped a budget decision.
- A comment asserted a pet could never come back from dead; a debug lever in the
  same commit range made it routine, and a shrine painted over a living
  creature.
- A comment claimed a panel had no backlight pin to drive. The first run on the
  new panel was a black screen with a clean log.

So: when a subagent says something is impossible, unreachable, already handled,
or not worth checking — **verify it cheaply yourself before building on it.**
One grep or one command is usually enough. The same applies to your own claims:
if you told Chris a number, and a later measurement contradicts it, say so
plainly and give the corrected one.

Related, and cheap to check: **a test that passes is not evidence it tests
anything.** Three tests on this project quietly stopped testing what they were
written for, including one that passed with the entire drawing path deleted.
Ask implementers to prove non-vacuity by breaking the thing under test and
watching the assertion fail.

## Model selection — tell Chris when to switch

Before starting a substantive task, if a different model would suit it better, say so in **one line**, then proceed. Fire at transitions, roughly once per session. Never interrupt mid-task unless something has genuinely changed. `/model` switches mid-session.

| Work | Model |
|---|---|
| Architecture with long shadows (HAL boundary, build structure, Lua API surface, save format); debugging that has already resisted two attempts; **PCB schematic/netlist review**; licensing or compliance reasoning | Opus tier |
| Implementation against an agreed plan; SDL backend; Lua bindings; sprite/framebuffer work; pet simulation framework; tests, refactors, docs, ordinary review | Sonnet tier — the default here |
| File moves, renames, boilerplate, commit messages, formatting, mechanical find-and-replace | Haiku tier |

Pattern: plan on Opus, implement on Sonnet.

Bigger cost levers than model tier, mention when relevant: **context volume** (fresh session per slice beats one long session) and **unnecessary subagents** (research justifies fan-out, implementation almost never does).

## Planning docs

Specs, roadmaps and the naming decision record live in `D:\Game Development Repos\Virtual Pet Experiments`. All current as of 2026-08-03.

- `02-min-spec-sheet.md` is the target hardware spec, and doubles as the constraint budget the desktop build enforces.
- `04-roadmap-diy-release.md` is the roadmap: Phase 1 is desktop-only, Phase 1b is bring-up after the hardware trigger is met.
- `07-naming-decision-and-setup.md` is the naming decision record and Phase 0 setup.

Superseded versions live in `_archive/` with dated folder names. Don't read from `_archive/` unless you are specifically looking at history.
