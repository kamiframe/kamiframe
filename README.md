# Kamiframe

**An open kit for giving a small virtual creature a body.**

Hardware, operating system, and SDK. All open, so you can build the device, write your own creature for it, or fork the whole thing into something that isn't a virtual pet at all.

---

> ### Status: Phase 1b. Firmware, board, and SDK all exist and run.
>
> HakoniwaOS boots on an ESP32-S3 devkit, drives a real display, and runs the pet care loop over a debug bridge from a Mac. The Lua SDK exists, and the demo creature runs both in the desktop simulator and on the board. Some pieces (sleep, art for the later life stages) are still in progress.
>
> If you want to watch someone with a web development background learn embedded hardware in public, mistakes included, you're in the right place. Star the repo or follow the devlog and you'll see it happen.

---

## The idea

Virtual pets are a genre that mostly stopped. There are collectors, there are emulators, there are a handful of indie devices, but there is no open platform. If you want to make a virtual pet today, you start from nothing: pick a microcontroller, write a display driver, invent a needs system, design an enclosure, and hope.

Kamiframe is meant to be the part you don't have to build. A reference device you can order the boards for and assemble yourself, an OS that handles the hardware and the boring parts of being alive, and an SDK where writing a creature means writing a creature, not writing a display driver.

Think of it as a small console. The system is open. The games are yours.

## What's in the box

| Piece | What it is |
|---|---|
| **Kamiframe One** | The reference hardware. ESP32-S3, a 2.4" color IPS display, buttons, a speaker, an IMU, a real-time clock, and a LiPo cell in a case you can print at home. |
| **HakoniwaOS** | The operating system. Display driver and HAL, sprite engine, sleep and wake, and the pet simulation framework: needs, decay, evolution, and catching up on the hours you were away. |
| **Kamiframe SDK** | Write creatures in Lua against a small, documented API. Package them into a cartridge file. No C required. |
| **Kamiframe Sim** | A desktop and browser build, so you can develop a creature without touching hardware. |

The pet simulation framework is the part I care most about. Every virtual pet project reinvents hunger, mood, aging, evolution branches, and what happens when the player closes the lid for three days. That should be a library, not a rite of passage.

### One codebase, not an emulator

The simulator is not a separate program that pretends to be the device. It's the real firmware, compiled against a desktop backend of the same hardware abstraction layer the ESP32 build uses. Same sprite engine, same Lua runtime, same simulation code, different bottom layer.

It also enforces the device's limits rather than letting your desktop paper over them: a 240×320 16-bit framebuffer, a Lua heap capped to what the PSRAM will actually give you, an asset budget sized to 12MB of the part's 16MB of flash (the rest is firmware, doubled for OTA), and warnings when a frame takes too long. Desktop speed lies, and a creature that only runs at 60fps on a laptop isn't finished.

## Design commitments

These are the things I'm trying not to compromise on.

- **You can build it yourself.** Not "technically open but good luck." The build guide is a first-class deliverable, with a pre-configured board order and a real bill of materials.
- **The case prints at home.** Designed for FDM from day one. No thin snap fits that only survive injection molding.
- **Commercial games are welcome.** If you want to write a creature and sell it closed-source, do it. You don't need my permission and you don't owe me anything. See [LICENSING.md](LICENSING.md).
- **The hardware is a suggestion.** The display is behind a HAL specifically so someone can build a mono or e-ink variant without forking the world.
- **Small is the point.** This is not a general purpose handheld. It's a thing that holds one creature and does that well.

## Roadmap

Rough, elastic, and paced for one person with a day job.

- **Phase 0** — Name, repo, first devlog post. Done.
- **Phase 1** — Everything on desktop first. HAL boundary, framebuffer and sprite engine against the desktop backend, Lua embedded, the pet simulation framework, and a demo creature running in the simulator under enforced device constraints. Plus an ESP-IDF hello-world in [Wokwi](https://wokwi.com), a browser ESP32-S3 simulator that costs nothing. Done — the creature runs in the simulator with saving and offline fast-forward working, and firmware booted in Wokwi, which triggered ordering parts.
- **Phase 1b (now)** — Parts arrived, real bring-up under way. The board boots, the display renders, and the care loop runs live over the debug bridge. Still ahead: deep sleep and RTC wake proven on the bench, and a breadboard pet surviving a week on battery.
- **Phase 2 (months 6–12)** — Custom PCB in KiCad, two or three revisions because that's normal, printable enclosure, SDK polish. *Exit: I carry a self-built unit daily for a month and it doesn't embarrass me.*
- **Phase 3 (months 12–15)** — v1.0. Firmware, SDK, simulator, KiCad and Gerbers, interactive BOM, enclosure files, demo creature, and the build guide. *Exit: a stranger builds one from the guide alone.*
- **Phase 4 (only if people ask)** — Small kit batches. Not planned. Let demand decide.

Nothing here has a deadline. The devlog is the discipline instead.

## Licensing, in one line each

- **Software** is Apache 2.0. Use it, fork it, ship it commercially.
- **Hardware** is CERN-OHL-W. Improve the board, share the improvements.
- **The demo creature's code** is Apache 2.0. **Its characters and artwork are not.** Draw your own.
- **Games you write** are yours entirely. Open or closed, free or paid, no strings.

Full explanation in [LICENSING.md](LICENSING.md). If anything there is unclear, open an issue and I'll rewrite it.

## Repositories

- **[kamiframe/kamiframe](.)** (this repo) — HakoniwaOS, SDK, simulator, docs, examples. Apache 2.0.
- **kamiframe/hardware** — Schematics, PCB, BOM, enclosure. CERN-OHL-W. Kept separate so the licenses never get tangled.

## About the name

Kamiframe is two words stuck together. *Kami* (カミ, 神) is the Japanese word for a spirit or a god, including the very small ones that live in ordinary objects. *Frame* is the English word, in the sense a machine has a frame: the chassis, the skeleton, the thing that holds something else up.

So, a frame for a spirit to live in.

**HakoniwaOS** is named after the *hakoniwa* (箱庭), a "box garden." It's a shallow tray of soil and sand with tiny bridges and houses set into it and small plants growing, arranged to imitate a whole landscape. It was a popular hobby in Japan in the 1800s. It's also, as it happens, the word Japanese games writers use for the genre of small enclosed worlds you tend over time, the Harvest Moon and Animal Crossing sort of thing. Both meanings fit a device that holds a little world you look after.

The creatures borrow from *tsukumogami* (付喪神), a class of Japanese yōkai: ordinary objects that wake up and gain a spirit after a hundred years of use. That's folklore. It belongs to everyone, I'm not claiming it, and I'm using it because it's the accurate description of what this device does, not because it sounds mysterious.

One thing worth saying plainly: I'm an American who likes Japanese design and Japanese folklore. This is not a Japanese company and it isn't pretending to be one. I picked these words because they mean the thing I'm making. If I've gotten a nuance wrong, tell me and I'll fix it.

## Following along

- **Devlog** — the honest version, including the parts that don't work.
- **Discussions** — questions, ideas, and "have you considered."
- **Issues** — there's a real codebase to file them against now.

Whether contributions are formally open yet is still an open question, not a settled one — there's plenty running now, but no contribution process has been decided. In the meantime, if you know embedded work and you can see me about to make a mistake, please say so. I'd rather hear it now than after the boards arrive.

## Prior art and thanks

This project exists downstream of a lot of people's work: Arduboy, Thumby, Pwnagotchi, the Playdate SDK's approach to a Lua game API, and every person who has ever posted a teardown of a Tamagotchi. The virtual pet genre is small and generous and I'm glad to be in it.
