# ADR 0029: Panel profiles — one display driver, many panels

**Status:** Accepted
**Date:** 2026-08-08

## Context

`esp_display.cpp` was written against the ST7789 and only the ST7789. It
called `esp_lcd_new_panel_st7789()`, ran `esp_lcd_panel_init()`
unconditionally, and declared a little-endian framebuffer.

Bring-up (ADR 0024) made that untenable in two independent ways.

The reference 2in ST7789 module arrived faulty — its DC line measured 0.7mV
at the panel while every other line swung a clean 3.3V, and soldering
directly to the pad did not revive it — and was returned. The panel that
does work, and on which every measurement in ADR 0024 was taken, is a 2.8in
HiLetgo ILI9341. Flashing the firmware as written to the board that actually
exists would have produced a dark screen, and all three of the hardcoded
decisions above would have been wrong for it.

Separately, and more importantly, Kamiframe is meant to be buildable with
whatever 240x320 SPI panel someone can source. That is a product decision
about who can build the kit, not an engineering preference, and it means
"which controller" cannot be a property of the driver.

## Decision

Everything that differs between panel modules becomes data in
`ports/esp32/hal/kf_panel_profile.h`, one `kf_panel_profile` per module:

| Field | Why it varies |
|---|---|
| `init` / `init_count` | Controllers do not share a command set, and the same controller on different glass needs different power and gamma values. Per-module, not per-controller. |
| `use_builtin_init` | ESP-IDF's ST7789 init sends four commands. The ILI9341 must skip it: RAMCTRL (0xB0) is a different register there, so sending it writes a wrong value to something real. |
| `big_endian_fb` | See below. |
| `invert` | ST7789 silicon defaults to inversion off, but many IPS modules around it are wired to need INVON. |
| `x_gap` / `y_gap` | Many modules address a window offset inside a larger controller frame buffer. |

`esp_display.cpp` knows how to drive a 240x320 SPI panel and no longer knows
which one. Adding a third module is a table, not a driver edit.

### Byte order is the field that earns the abstraction

RGB565 goes on the wire high byte first. The ESP32-S3 is little-endian. And
`esp_lcd` byte-reverses commands and parameters but **not** colour data. So a
`uint16_t` framebuffer transmits backwards.

The ST7789 hides this: RAMCTRL carries a little-endian bit that `esp_lcd`
sets from `data_endian`. But that bit is only ever sent by
`esp_lcd_panel_init()` — which the ILI9341 path must skip — and the ILI9341
has no equivalent register at all. It is always big-endian.

Measured on hardware, full-screen fills where no geometry is involved:
0xF800 red showed blue, 0x07E0 green showed pink, 0x001F blue showed green,
while white and black were correct. That is a plain byte swap and nothing
else. White and black are invariant under both a byte swap and a BGR fault,
which is exactly what separates the two — and why the bring-up test card
carries white and black patches rather than three colour bands.

This is the field a naive abstraction would have missed, because it is
invisible until it is a whole evening.

## Alternatives considered

**Swap the hardcoded ST7789 for a hardcoded ILI9341.** Cheapest, and wrong
for the same reason the original was: it makes the next panel another driver
rewrite, and the product goal is many panels.

**Make `kf_color` big-endian at construction time**, via `KF_RGB565`. Tempting
and genuinely fast — zero per-frame cost — and it survives a first look,
because nothing in the codebase decodes a colour back into components
(verified by grep). Rejected because it is a global, invisible change to a
documented contract (`kf/types.h`: "RGB565, native-endian") to satisfy one
panel. It would also silently invalidate every pre-baked sprite array in
flash, and force LVGL's own colour-swap setting to stay in lockstep with a
constant three layers away. A panel quirk should not reach that far.

**Swap in `kf_display_present()`, into a full staging frame.** Simple, but a
second framebuffer is 150KB of internal RAM on a chip with 512KB, to save a
handful of `draw_bitmap` calls.

**Chosen: swap in `kf_display_present()`, strip at a time.** `kf_color` keeps
its native-endian contract, every producer of a pixel — blitter, font
renderer, sprite data, LVGL — stays unaware, and the cost is a 19KB DMA strip
plus one pass over 76,800 pixels per frame. Against ~31ms of wire time at the
measured 40MHz that is small, but it is not free, and it is a real reason to
prefer a panel that does not need it.

## Which panel this build defaults to

The ILI9341, which is the supported alternative rather than the reference
panel. That is deliberate: a default which produces a black screen on the
only board in the world running this firmware is a bad default, however
defensible on paper.

**This should flip to the ST7789 once a replacement arrives and passes.** The
ST7789 profile is written, and is reasoned rather than proven.

## Verified

- `idf.py build` clean for esp32s3 against ESP-IDF v6.0.2, zero warnings
  under `-Wall -Wextra -Werror`, both alone and alongside ADR 0027 (LVGL) and
  ADR 0028 (Lua).
- The ILI9341 profile's values are not invented: every one of them —
  MADCTL 0x88, COLMOD 0x55, inversion off, big-endian framebuffer, no gap —
  is what `ports/esp32-bringup` proved on the bench and photographed on
  2026-08-08 (ADR 0024).

## Not verified

**This driver had never driven a panel, as of this ADR.** The values were
proven; the code that sends them was not, at the time. **Superseded:** ADR
0032 (one day later, 2026-08-09) opens "Once the pet was rendering on real
hardware" — this driver has since driven the ILI9341 panel for real. Only
this blanket first sentence is superseded; the rest of this paragraph's
point (the diagnostic that proved the values is a separate program from the
driver that sends them, and matching tables sent by different code is a real
gap class) is still a fair thing to have flagged at the time.

The ST7789 profile is unverified in every respect — no hardware exists to
test it against. Expect to correct something in it on the first real run.

## Cost to change

Adding a panel: one table plus one `#define`. Changing the default: one
line. Removing the abstraction if it proves unnecessary would mean folding
one profile's fields back into `esp_display.cpp` — an hour, and only worth
doing if the many-panels goal is abandoned.

The one genuinely expensive reversal would be moving the byte swap out of
`present()` and into colour construction, since that changes a contract
every layer depends on. That trade is documented above rather than left to
be rediscovered.
