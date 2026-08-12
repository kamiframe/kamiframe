# ADR 0032: Screen tearing, what we tried, and what the kit's panel must expose

**Status:** Accepted
**Date:** 2026-08-09

## Context

Once the pet was rendering on real hardware, three separate flicker faults
were found and fixed (ADR 0029's driver work, plus the dirty-rect and
idempotent-update commits). What remained afterwards is not a bug: content
that genuinely changes — a focus highlight moving, a percentage label
updating, a screen switch — flickers in the rectangle that changed.

That is tearing. The panel scans its own memory out to the glass
continuously on its own clock, and nothing coordinates our writes with that
scan, so for one refresh the scanned region contains a mix of old and new
pixels. It is invisible on a static screen and obvious when the time
multiplier is turned up, because then the numbers really are changing every
frame. (That multiplier is `KFDBG MULT`, ADR 0031's addition to ADR 0030's
bridge from the day before -- this hands-on investigation is what first
drove that bridge live, over a physical UART, against real hardware. See
ADR 0030's "Not verified" section for why its own text still says "no
board reachable" afterwards: that line describes what each individual
coding task's own sandbox could reach, not whether the board was ever
touched by anyone.)

The standard fix is the panel's TE (tearing effect) signal, which pulses at
vertical blanking so the host knows when writing is safe. **The 2.8in
ILI9341 module in use does not expose TE.** Confirmed from the
manufacturer's schematic: the display's own 18-pin flex carries RESET, SCK,
D/C, CS, SDI, SDO, GND, LED and four resistive-touch lines. There is no pin
to wire and no pad to solder.

## What was tried

The ILI9341 supports `Get_scanline` (0x45), which reports the controller's
current scan position. The module does expose SDO, so unlike TE this was at
least reachable. It was wired to GPIO6 and probed.

**The panel answers, and the counter is real.** Measured at 1, 2 and 4MHz,
64 samples each: a counter running 0..161 that climbs monotonically and
wraps — 56 increases against a single decrease per run, the same shape at
every clock. Derived from sample spacing: ~64µs per count, so ~10ms per
cycle, about 96Hz refresh. A read costs 49–77µs.

Two things were learned on the way that are worth keeping:

- **The reply framing is `(raw[0] << 8) | raw[1]`, with no dummy byte**,
  contrary to the datasheet reading first implemented. The dummy-byte
  framing produces byte-boundary artefacts (0x000, 0x0FF, 0x100, 0x180,
  0x1FF) that look like noise and are not.
- **Reads at the 40MHz write clock are unreliable**, or at least were never
  shown reliable. Reading at 2MHz requires rebuilding the panel, which is
  far too slow to do per frame.

The wait was implemented anyway, behind a runtime flag, on the strength of
one re-decoded sample suggesting 40MHz might be usable. On hardware it made
no difference to the flicker. The flag is now default off.

## Decision

**Accept tearing on panels that cannot be synchronised, and make the ability
to synchronise a stated panel-selection criterion for the real board.**

Concretely, when choosing the flagship panel, in order of preference:

1. **An RGB parallel panel** driven by the ESP32-S3's LCD peripheral. The
   host owns the scan timing and streams from a framebuffer in PSRAM, so
   there is no separate panel memory to race and tearing does not exist.
   Costs ~20 pins (16 data + PCLK/DE/VS/HS), which forces the I2C GPIO
   expander for buttons that ADR 0024 already flagged, and needs a
   constant-current backlight driver.
2. **An SPI panel that exposes TE.** One trace to a GPIO, an interrupt
   instead of a poll, no bus time spent. Rare on breakout modules — a survey
   of ~20 Amazon listings found none, and Adafruit's own boards do not break
   it out either — but ordinary on bare panels with an FPC connector, which
   is what a custom PCB uses anyway.
3. **An SPI panel exposing SDO**, as a fallback, accepting that polling is
   coarse and may not work.

**Panels that expose none of these still work** and must keep working — the
panel-profile system (ADR 0029) already treats capability as data. They
simply tear on fast-moving content, which is acceptable for a pet that is
mostly still and unacceptable for full-screen animation.

## Consequences

The 2.8in ILI9341 currently in use is, unexpectedly, the *more* capable of
the two panels on hand: the 2in ST7789 module intended as the primary has no
SDO at all (its header is VCC GND DIN CLK CS DC RST BL), so it can do
neither TE nor polling. If full-screen animation matters, the reference
panel decision should be revisited rather than assumed.

Three 2in modules were examined and none exposed SDO or TE. This appears
structural rather than coincidental: 2in modules target compact, low-pin
uses, so vendors trim the pinout to the minimum needed to write to the
panel. Larger modules often expose SDO because they already carry touch or
SD hardware that needs a data-out line.

## Cost to change

The wait logic stays in the tree, defaulted off, reachable with one command
(`kf_debug.py vsync on`). A panel that answers reads more cleanly turns it
back on with no code change. A panel with TE replaces the poll with an
interrupt — a contained change to `push_rect()` plus one field in the panel
profile.

Going to RGB parallel is not contained: it changes the pin budget, the
display HAL's transfer model, and the board layout. That is a decision to
take deliberately at PCB design time, which is why it is written down here
rather than discovered later.

## Verified

- The scan counter's existence, range, monotonicity and timing, on hardware,
  at three clocks.
- The reply framing, empirically, by comparing both interpretations against
  the same raw bytes.
- That TE is absent from this module, from the manufacturer's schematic and
  both sides of the PCB.

## Not verified

Why the wait failed to help. Any of three links could be at fault: 40MHz
reads may be unreliable in a way the slow sweep did not expose, the
count-to-row mapping (`kf_vsync_count_to_scan_row()`, assuming one count is
two rows) may be wrong, or the wait may resume on the wrong side of the
scan. Nobody chased it further because the ceiling on the approach was
modest even in the best case: a ~50µs read against a ~64µs scan step gives
resolution of a couple of rows at best.
