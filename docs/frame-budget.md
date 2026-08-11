# The frame budget, and whether full-screen animation works

**Short answer: yes, at 31fps today.** A scrolling tile background with
sprites on top, every pixel redrawn every frame, runs at about **31fps**.
Drawing is not the problem. The wire to the screen is — and the obvious free
lever, a faster SPI clock, was tried at bring-up and failed on the wiring
tested (see "Run the bus faster" below). 60fps is still reachable, but
through a different lever: a parallel panel, a smaller panel, or reducing
what a frame sends.

Run it yourself:

```
build\kamiframe-sim --stress
```

Scrolling tilemap, twelve moving sprites, no dirty-rectangle cleverness at all.
The console prints what it would cost on hardware.

---

## The measurement

From `kamiframe-headless --frames 300 --stress`, at the current assumed 40MHz
SPI clock:

```
device estimate: draw  1259 us + transfer 30720 us
   serial (today)        31979 us  ->   31.3 fps
   overlapped (DMA+2buf) 30720 us  ->   32.6 fps
pixels drawn:  76800 opaque +  12288 keyed   dirty 100%
```

Drawing the entire screen costs about **1.3ms**. Sending it costs about
**30.7ms**. Drawing is 4% of the frame.

That is the single most important fact about this hardware, and it is the
opposite of what a web or desktop background suggests. You are not
compute-bound. You are display-bandwidth-bound.

---

## Why the wire is the bottleneck

A 240x320 RGB565 frame is 153,600 bytes. The panel is connected by a serial
line that moves one bit per clock:

```
153,600 bytes x 8 = 1,228,800 bits
1,228,800 / 40,000,000 = 30.7 ms
```

Nothing you do in software changes that. It is a property of the cable.

Measured with the bus clock varied, everything else identical:

| Link | Full frame | Ceiling (overlapped) |
|---|---|---|
| SPI 40MHz (current assumption) | 30.7 ms | **32.6 fps** |
| SPI 60MHz | 20.5 ms | **48.8 fps** |
| SPI 80MHz | 15.4 ms | **65.1 fps** |
| 8-bit parallel, 20MHz pixel clock | 7.7 ms | ~130 fps (something else becomes the limit first) |

Those first three are real runs, not arithmetic on a napkin. The parallel row
is calculated, because it needs different hardware.

---

## So what is the dirty rectangle actually for?

**It is an optimisation, not a requirement, and it is never in your way.**

`kf_display_present(framebuffer, dirty_rects, dirty_rect_count)` takes a
short list of rectangles (up to `KF_MAX_DIRTY_RECTS`, see ADR 0011)
describing what changed, not just one. If your game changes everything, you
pass the whole screen as a single rectangle and get whatever the hardware
can do. Nothing refuses. The bouncing-blob demo passes one small rectangle
because in that demo only a small rectangle changed; a HUD and a moving
sprite together pass two, kept independent, rather than one box spanning
both.

Where it earns its place is that most virtual-pet screens are not scrolling
tilemaps. A pet idling in a room, a menu, a stats page, a sleeping animation:
these change a small fraction of the screen. On those, dirty rectangles are
the difference between 32fps and effectively unlimited, and between a battery
that lasts a day and one that lasts a week, because the display link is also
one of the bigger power draws.

The right way to think about it: **the dirty rectangle is how a quiet screen
gets cheap. It is not what makes a busy screen possible.**

---

## How to get 60fps with a full screen of animation

In rough order of cost to you.

### 1. Overlap drawing and sending (free, and already planned)

Right now the model assumes the CPU draws the whole frame and then waits for
all of it to go out: 1.3 + 30.7 = 32ms. With DMA and a second buffer, the
previous frame goes out over the wire while the CPU draws the next one, so a
frame costs `max(draw, transfer)` instead of `draw + transfer`.

On this hardware that saves only the 1.3ms of drawing, because transfer
dominates so completely. It matters more once drawing gets heavier: alpha
blending, more sprites, effects. Worth having, not a rescue.

Cost is memory. A second full framebuffer is another 153,600 bytes of internal
SRAM, and internal SRAM is the scarce pool. The cheaper version is **band
rendering**: draw a horizontal strip into a small buffer, start its DMA, draw
the next strip while the first is in flight. Same overlap, a fraction of the
RAM. `budget.h` has `KF_DISPLAY_DOUBLE_BUFFERED` sitting at 0, and the report
prints both numbers so the headroom is always visible.

### 2. Run the bus faster — tried, and it did not pan out

40MHz was a conservative assumption, and the theoretical case for 80MHz
looked good: the ST7789 datasheet is around 62MHz for serial writes, and
80MHz is widely used in practice on the ESP32-S3. Going from 40 to 80 would
double the ceiling — **32fps to 65fps** — for a single configuration line and
no code changes, if the wiring cooperated.

**It didn't.** Measured at bring-up (`docs/hardware-bringup.md`'s Stage 2b
clock sweep, 2026-08-08, on the breadboard ILI9341): 40MHz rendered the test
card correctly; 80MHz came out **solid white** — wholesale data corruption on
this wiring, not a subtle glitch. `KF_DISPLAY_SPI_HZ` stays 40MHz, now as a
measured ceiling rather than a guess. Worth re-trying if the wiring changes
(shorter leads, a PCB instead of jumpers) or once the primary 2in ST7789
panel is on the bench — this result is from the 2.8in ILI9341 — but treat it
as a re-test, not a free win still on the table.

### 3. Choose a parallel panel instead of SPI (a hardware decision)

The ESP32-S3 has a dedicated LCD peripheral that speaks the 8080-style
parallel interface, 8 or 16 bits wide, instead of one bit at a time. Eight
bits per clock at 20MHz is 20MB/s, which puts a full frame at 7.7ms and takes
the display off the critical path entirely.

The catch is sourcing. Most small 240x320 ST7789 modules sold to hobbyists are
SPI-only; parallel versions exist but are less common, use more GPIO pins, and
complicate the build guide for anyone reproducing your hardware. Since the
whole point is that people can build one themselves, "easy to buy" carries
real weight against "faster."

**This is a genuine product decision rather than a technical one, and it is
yours.** It does not need deciding now. It needs deciding before the PCB.

### 4. Reduce how much data a frame is (an architecture decision, later)

- **A smaller panel.** 240x240 is a very common size and is 25% less data,
  taking 40MHz SPI from 32fps to 43fps. It also changes what the pet looks
  like, so it is a design call.
- **12-bit colour.** The ST7789 accepts RGB444 as well as RGB565: 1.5 bytes
  per pixel instead of 2, so 25% less data, at the cost of visible banding on
  gradients. Retro-appropriate, arguably.
- **A palettised framebuffer.** Store 8 bits per pixel in RAM (a palette index
  rather than a colour) and expand to RGB565 during the transfer. Halves the
  framebuffer to 76,800 bytes, which makes double buffering comfortable, and
  gives you palette-swap effects for free: day/night tinting, damage flashes,
  a sick pet going grey, all by changing 256 numbers instead of redrawing
  anything.

  It does **not** reduce wire time, since the panel still receives 16-bit
  pixels. And it is entangled with the deferred sprite-engine decision: LVGL
  works in RGB565 natively, so choosing a palette would effectively rule LVGL
  out. Flagging it, not deciding it.

### 5. Hardware scrolling (a free trick worth knowing about)

The ST7789 has a vertical scroll feature built into the panel controller: you
tell it to shift its own memory, and only the newly revealed row needs
sending. A vertically scrolling background costs almost nothing.

It only scrolls one axis, but if the panel is mounted rotated 90 degrees, that
axis becomes horizontal on screen. A classic side-scroller trick, and worth
remembering when a specific game needs more than the bus can give.

---

## What the numbers assume, stated plainly

Two of the figures in `budget.h` are estimates, not measurements, and both are
flagged in the file:

**`KF_DISPLAY_SPI_HZ = 40000000`.** Conservative. Likely improvable to 60 or
80 on real hardware. First thing to check at bring-up.

**`KF_DRAW_OPAQUE_PX_PER_US = 100`, `KF_DRAW_KEYED_PX_PER_US = 25`.** How fast
the device fills pixels. Genuine guesses, and could reasonably be out by a
factor of two either way.

That second uncertainty does not change any conclusion here, which is worth
saying explicitly. Drawing is 4% of the frame. If the estimate is wrong by 4x
in the pessimistic direction, drawing becomes 16% of the frame and transfer
still dominates. The conclusion "you are wire-bound" is robust to being quite
badly wrong about the CPU.

Both get corrected in Phase 1b, and the simulator's numbers become accurate
rather than merely useful.

---

## Why the simulator counts pixels instead of timing itself

The budget report prints two different CPU numbers:

```
device estimate: draw 1259 us
host cpu          209 us (your PC, not the device)
```

The second one is your machine. It is meaningless as a prediction, because a
desktop draws pixels roughly a hundred times faster than a 240MHz
microcontroller, and it varies with what else you have open.

So the drawing code **counts the pixels it writes**, split into cheap ones
(opaque fills and full-row copies, which are memcpy-shaped) and expensive ones
(colour-keyed blits, which test every pixel). The count is a property of your
game, identical on every machine that runs it. Converting it to a device time
is then one multiplication by a number in `budget.h`.

That is the same principle as the transfer estimate, applied to the other half
of the frame: **measure the work, not the machine.**

---

## Summary

- Full-screen animation works. About 31fps today, 60fps within reach.
- Drawing is 4% of the frame. The wire is 96%.
- Dirty rectangles are how a quiet screen gets cheap, not a limit on a busy
  one. Pass the whole screen when the whole screen changes.
- The biggest lever would have been the bus, 40MHz to 80MHz doubling the
  ceiling for free — but it was tried at bring-up and 80MHz came out solid
  white on the wiring tested. 40MHz is the measured ceiling for now.
- The decision this eventually forces is a hardware one, SPI versus parallel
  panel, and it is due before the PCB rather than now.
