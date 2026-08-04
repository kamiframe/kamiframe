# Licensing

Kamiframe uses three different licenses for three different kinds of thing. This page explains it in plain language. The actual legal text lives in the `LICENSE` files; if this page and a `LICENSE` file ever disagree, the `LICENSE` file wins.

## The short version

| What | License | What it means for you |
|---|---|---|
| Software (OS, SDK, simulator, tools, examples) | **Apache License 2.0** | Do essentially anything, including selling it. Keep the notices. |
| Hardware (schematics, PCB, enclosure, BOM) | **CERN-OHL-W v2** | Build it, sell it, modify it. If you modify the board itself, publish your changes. |
| The demo creature's **code** | **Apache License 2.0** | Same as the rest of the software. |
| The demo creature's **characters and artwork** | **Copyright, all rights reserved** | Look at it, learn from it, don't ship it. |
| Games and creatures **you** write | **Whatever you want** | They're yours. Open, closed, free, paid. No obligations to me. |

---

## Software: Apache 2.0

Everything in this repository is Apache 2.0 unless a file or folder says otherwise. That covers HakoniwaOS, the SDK, the simulator, the packaging CLI, and the example creatures' source code.

**You can:** use it commercially, modify it, distribute it, sublicense it, use it in closed-source products, and build a business on it.

**You have to:** include the license and copyright notice, and state what you changed if you distribute a modified version.

**You don't have to:** open-source your own work, ask permission, or pay anyone.

Apache 2.0 rather than MIT because it includes an explicit patent grant. That matters more than people think on a hardware-adjacent project, and it costs contributors nothing.

## Hardware: CERN-OHL-W v2

The `kamiframe/hardware` repository, which holds the KiCad project, Gerbers, bill of materials, and enclosure models, is licensed under the **CERN Open Hardware Licence Version 2, Weakly Reciprocal**.

**You can:** make the boards, use them, sell them, sell assembled units, modify the design, and use the design in a product with other parts that aren't open.

**You have to:** if you distribute a modified version of *this* hardware, publish your modified design files under the same license.

**The "weakly" part matters.** Strongly reciprocal open hardware licenses can pull anything you connect to the board into scope. Weakly reciprocal doesn't. You can build a Kamiframe One into a larger product with proprietary components and only the Kamiframe-derived board files stay under CERN-OHL-W. That was a deliberate choice: I want people to be able to use this in things I haven't thought of.

Kept in a separate repository so the two licenses can never blur into each other. Mixing Apache-licensed code and CERN-licensed hardware in one tree is how open hardware projects end up with a licensing FAQ nobody can answer.

## The demo creature: split license

The device ships with one creature so it does something out of the box. That creature is split down the middle.

**Its code is Apache 2.0.** Read it, copy it, use the mechanics, build on the structure. It's meant to be a worked example of how to write for this platform, and an example nobody's allowed to use isn't an example.

**Its characters, artwork, animations, names, and designs are copyrighted and not licensed for reuse.** Don't ship them in your project, don't sell merchandise with them, don't use them as your game's mascot.

This is the same structure id Software used with Doom: the engine went open, the monsters didn't. It works because it gives away the thing that helps everyone and keeps the thing that's personal. If you want a creature, draw a creature. That's the fun part anyway.

If you're not sure whether something you want to do crosses the line, open an issue and ask. I'd rather answer than have you guess.

## Games and creatures you write

Yours. Completely.

Write a creature, keep the source closed, sell it for money, never mention this project, all fine. You are not obligated to open-source anything, contribute anything back, share revenue, or ask permission. Using the SDK does not put any license on your work.

This is deliberate and it's not going to change. A platform where commercial software is a grey area isn't a platform, it's a hobby project with a marketing page. If someone makes a living selling creatures for this thing, that's the best possible outcome.

The one thing you can't do is imply I made your creature or that it's officially endorsed, because that's about trademark and honesty, not copyright.

## Trademark

"Kamiframe" and "HakoniwaOS" are the project's names. I'd like to keep them meaning this project.

**Fine, no permission needed:**
- "Runs on Kamiframe" / "for Kamiframe" / "a HakoniwaOS device"
- "Built from the Kamiframe hardware files"
- Writing about, reviewing, or teaching the project
- Selling boards or kits you built from the published files, described accurately

**Please don't:**
- Name your fork "Kamiframe [something]" in a way that suggests it's the official one
- Use the name or logo in a way that implies I made, endorsed, or support your product
- Register the name or a confusingly similar one

No trademark is registered yet. That's a Phase 4 problem, if it's ever a problem at all. If you're doing something commercial and want certainty, ask.

## Contributing

There's no contribution process yet because there's nothing to contribute to. When there is, contributions to the software will be under Apache 2.0 and contributions to hardware under CERN-OHL-W, by the ordinary inbound-equals-outbound rule. No CLA. I'm not going to ask anyone to sign a document so I can relicense their work later.

## Questions

If any of this is unclear, that's a bug in this document. Open an issue and I'll rewrite the confusing part.

None of this is legal advice, and I am not a lawyer.
