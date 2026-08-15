# ADR 0059: The 2in ST7789 lights up, becomes the default, and takes a returned panel's reputation with it

Status: Accepted
Date: 2026-08-13

## Context

The 2in Waveshare ST7789 has been the primary panel for this product since
the spec was written, and until today it had never displayed anything. The
ILI9341 2.8in module was the default profile for exactly one reason, stated
in `kf_panel_profile.h`: a default that produces a black screen on the only
board in the world running this firmware is a bad default, however
defensible on paper.

That reason has expired. Both panels display correctly.

## What was verified

A replacement ST7789 module was wired to the pinout in `kf_esp_pins.h` — the
same eight wires the ILI9341 uses, with GPIO6 moved from that module's SDO to
this one's BL — and flashed with `-DKF_PANEL=st7789`.

**First flash: a correct picture, as a photographic negative.** Geometry,
layout, text and sprites all right; every colour complemented.

**Second flash, with `invert` set true: correct.** That was the only change.
The init table — the Waveshare module's own sequence, taken from the upstream
Linux DRM driver written for this board — needed no correction at all, and
`big_endian_fb`, `x_gap`, `y_gap` and `use_builtin_init` were all already
right.

### Why the colour is confirmed rather than assumed

The Home background at the time of this verification was
`kf.color(232, 240, 216)`, a pale green-tinted white, and it looked like a
pale green-tinted white. That alone proves nothing: a red/blue channel swap
turns it into RGB(216, 240, 232), which on near-white is close to
indistinguishable by eye.

> That green cast was the first thing Chris disliked about the panel once it
> was working, and the background is now a warm cream — see
> `KF_CREATURE_PRESENTER_BG`. The proof below is unaffected, because it never
> rested on the background.

What separates them is a saturated colour elsewhere on the same screen. The
HUNGER and HAPPY bars are orange — high red, low blue — and the same swap
would render them teal. They are orange. Channel order is correct.

This is the same class of check as ADR 0024's byte-order finding, where white
and black were correct and only the saturated fills revealed the fault. Judge
colour on saturated pixels; near-white and near-black are invariant under
most of the ways colour goes wrong, which is what makes them useless as
evidence and dangerous as reassurance.

## Decision

**`KF_PANEL` defaults to `st7789`**, in `ports/esp32/main/CMakeLists.txt` and
in `kf_panel_profile.h`'s `KF_PANEL_PROFILE` fallback, which must agree. The
ILI9341 remains fully supported via `-DKF_PANEL=ili9341` — it is not
deprecated, and it is still the only profile the `KFDBG SCANLINE` and `VSYNC`
diagnostics can work against, being the only module here with a data-out
line.

`invert` becomes `true` on the ST7789 profile.

### The footgun this creates

`KF_PANEL` is a CMake cache variable, so it persists in a build directory
once set. Building for the other panel means passing it explicitly, and
flashing the wrong profile produces a black screen or wrong colours **with a
completely clean log** — every `esp_lcd` call returns `ESP_OK` against glass
showing nothing. This has already cost one session in the opposite
direction. The comments at both default sites say so.

## The part that matters more than the panel

**The first ST7789 was probably never faulty**, and this ADR is the reason
`kf_panel_profile.h` no longer cites its return as evidence against the
profile.

ADR 0024 condemned that module on a 0.7mV reading at its DC line. Two
sections earlier, the same ADR condemned GPIO9 for producing a 0.7mV reading
at its DC line. One session, one symptom, two culprits named — and only one
of them can have been the cause.

GPIO9 is FSPIHD, one of SPI2's four IOMUX function pins, and this build
drives SPI2 on the other three. If the driver claimed GPIO9 through IOMUX,
DC carried hold-line traffic no matter how sound the wire was. That accounts
for every observation in that section, including the one treated as decisive
— soldering directly to the pad, measuring a clean 3.3V, and still seeing
nothing.

ADR 0024 keeps its original text; a correction note points here. An ADR is a
record of what was believed at a moment, and rewriting it to be retroactively
correct destroys the only thing it is for.

### The generalisable lesson

When two independent suspects produce one identical measurement in one
session, naming both is not two findings. It is one finding and one guess.
The guess cost a returned part and five months of documentation asserting
that the primary panel was unproven.

## Still not measured on this panel

Verification was of *picture*, not *speed* or *longevity*:

- **No clock sweep has ever been run against the ST7789.** It inherits 40MHz
  from a measurement taken on the ILI9341. Safe — ST7789 modules commonly
  tolerate more — but it means the frame budget's ~32fps full-frame ceiling
  is, on the panel that is now the default, an inherited number.

  > **Resolved 2026-08-14: 80MHz.** The sweep held, and the real game then
  > rendered correctly there — the stronger evidence, since a seven-second
  > card cannot see an occasional dropped bit. A full-screen frame drops from
  > ~31ms to ~15ms against a 33ms budget.
  >
  > It also showed the clock is **per-panel, not global**: the ILI9341 came
  > out solid white at 80MHz, so one constant could only ever have been wrong
  > for one of the two. `spi_hz` is now a panel-profile field, which is where
  > every other per-module difference already lives.
  >
  > A brief screen dim on button presses was investigated and is **not** the
  > clock — the MENU button, which changes screen and plays no sound, does
  > not cause it. It is current draw, almost certainly the audio amp.
- **Tearing behaviour is unassessed.** ADR 0032's finding that neither
  breadboard panel exposes a TE signal is unchanged, and this module's
  eight-pin flex has no TE pin.
- **The BL pin drives the backlight, but PWM dimming is untested.** The
  screen is lit; nothing has yet tried to dim it. This is the first panel in
  the project where that is even possible.

## Alternatives considered

**Keep `ili9341` as the default.** Rejected: the only argument for it was
that the ST7789 had never worked, and that is no longer true. Leaving it
would mean the primary product panel needs a flag while the alternative does
not, which inverts the intent and quietly teaches every new builder the
wrong default.

**Flip the default only after a clock sweep.** Rejected: correctness and
speed are separable here. The panel renders correctly at the inherited
clock, and holding the default hostage to an optimisation measurement would
leave the misleading default in place for no gain in safety.

**Rewrite ADR 0024's faulty-module section.** Rejected on the grounds
`CLAUDE.md` gives for ADRs generally: an ADR stays true as history. A dated
correction preserves both what was believed and what turned out to be so;
a rewrite preserves neither.
