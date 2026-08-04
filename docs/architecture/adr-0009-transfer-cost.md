# ADR 0009: Model display transfer cost on desktop

**Status:** Accepted, 2026-08-04
**Reversal cost:** Medium.

## The lie desktop tells that matters most

Not CPU speed. That putting pixels on the screen is free.

On desktop, `present()` is a texture upload and a GPU blit: effectively
instant. On the device it is 153,600 bytes down an SPI wire:

```
240 * 320 * 2 bytes * 8 = 1,228,800 bits
1,228,800 / 40,000,000  = 30.7 ms
```

That is most of a 33.3ms frame before the CPU has drawn anything, and it is
very likely the largest single item in the frame budget. Desktop reports it as
zero.

Without modelling it, the natural outcome is a 60fps game designed over
several months that the hardware cannot display, discovered at bring-up.

## Decision

The desktop display backends report the **device's** link speed in
`kf_display_caps.link_bytes_per_second`, not the host's. Core estimates
transfer time from the dirty rectangle every frame and includes it in the
budget:

```
last   654 us =   192 cpu +   462 transfer(est)   dirty   1%
a FULL frame would cost 30720 us of transfer alone
```

Reporting zero would silently switch off the one number desktop cannot measure
and most needs to show.

## Effect, immediately

The demo repaints only where the sprite was and where it now is, so the mean
dirty area is about 1.8% and the estimated transfer cost is about 462us
instead of 30,720us. That behaviour was not designed in advance; it is what
the number made obvious the moment it was visible. A CI test now holds the
mean dirty area under a ceiling, because a change that starts redrawing the
whole screen would still look correct on desktop and would still halve the
frame rate on hardware.

## Honest limits

`KF_DISPLAY_SPI_HZ` is currently an **assumption**, not a measurement. ST7789
over SPI on the S3 is commonly clocked at 40 to 80MHz; 40 is the conservative
figure. It sits in `budget.h` with a comment saying so, and it is the first
number to correct at bring-up.

The estimate is also pessimistic on purpose: it ignores the DMA overlap the
device will get from double buffering. Being wrong in the direction of "you
have less headroom than you think" is the useful direction.

## Extended 2026-08-04: draw cost is estimated the same way

Host wall-clock time is equally useless as a prediction in the other
direction: a desktop draws pixels roughly a hundred times faster than a 240MHz
microcontroller. So the blitter counts the pixels it writes, split into opaque
(memcpy-shaped) and colour-keyed (per-pixel test), and the frame loop converts
the counts to an estimated device time using `KF_DRAW_*_PX_PER_US` in
budget.h. The count is a property of the game, not the machine.

The report also prints both `serial_us` (draw + transfer, what it costs today)
and `overlapped_us` (max of the two, what DMA plus double buffering would
give), so the available headroom is visible rather than discovered.

`kamiframe-headless --stress` runs a full-screen scrolling tilemap under 12
sprites: draw 1,259us, transfer 30,720us, i.e. **drawing is 4% of the frame**.
The conclusion that this hardware is display-bandwidth-bound rather than
compute-bound is robust even if the draw-rate assumption is out by 4x. Full
analysis and the options for 60fps are in `docs/frame-budget.md`.

## Later

Once double buffering exists, the desktop backend can actually sleep the
modelled transfer time, so the simulator paces like the device rather than
merely reporting on it.
