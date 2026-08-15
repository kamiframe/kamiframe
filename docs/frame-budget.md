# The frame budget, and whether full-screen animation works

**Short answer: yes, at 60fps today.** A scrolling tile background with
sprites on top, every pixel redrawn every frame, runs at about **60fps** on
the default 2in ST7789. Drawing is not the problem. The wire to the screen
is — and the obvious free lever, a faster SPI clock, was tried at bring-up,
failed on the 2.8in ILI9341, and held at 80MHz on the ST7789 (see "Run the
bus faster" below). That doubled the ceiling and is where today's 60fps came
from.

On an ILI9341 build the same scene runs at about **31fps**, because that
panel's measured ceiling is 40MHz. The clock is a property of the panel, not
of the project: each profile in `ports/esp32/hal/kf_panel_profile.h` carries
its own measured `spi_hz`.

Run it yourself:

```
build\kamiframe-sim --stress
```

Scrolling tilemap, twelve moving sprites, no dirty-rectangle cleverness at all.
The console prints what it would cost on hardware.

---

## The measurement

From `kamiframe-headless --frames 300 --stress`. The desktop backends model
the primary panel's measured 80MHz, so this is what an ST7789 build costs:

```
device estimate: draw  1259 us + transfer 15361 us
   serial (today)        16620 us  ->   60.1 fps
   overlapped (DMA+2buf) 15361 us  ->   65.0 fps
pixels drawn:  76800 opaque +  12288 keyed   dirty 100%
```

Drawing the entire screen costs about **1.3ms**. Sending it costs about
**15.4ms**. Drawing is 8% of the frame. On a 40MHz ILI9341 the transfer is
30.7ms instead and drawing is 4%.

That is the single most important fact about this hardware, and it is the
opposite of what a web or desktop background suggests. You are not
compute-bound. You are display-bandwidth-bound.

---

## Why the wire is the bottleneck

A 240x320 RGB565 frame is 153,600 bytes. The panel is connected by a serial
line that moves one bit per clock:

```
153,600 bytes x 8 = 1,228,800 bits
1,228,800 / 80,000,000 = 15.4 ms
```

Nothing you do in software changes that. It is a property of the cable.

Measured with the bus clock varied, everything else identical:

| Link | Full frame | Ceiling (overlapped) |
|---|---|---|
| SPI 40MHz (the ILI9341's measured clock) | 30.7 ms | **32.6 fps** |
| SPI 60MHz | 20.5 ms | **48.8 fps** |
| SPI 80MHz (the ST7789's, and the default) | 15.4 ms | **65.1 fps** |
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
the difference between the full-frame ceiling and effectively unlimited, and
between a battery
that lasts a day and one that lasts a week, because the display link is also
one of the bigger power draws.

The right way to think about it: **the dirty rectangle is how a quiet screen
gets cheap. It is not what makes a busy screen possible.**

---

## Where the remaining headroom is

60fps with a full screen of animation is here, on the default panel. Past
that — or on the 40MHz ILI9341, where it is not here yet — these are the
levers, in rough order of cost to you.

### 1. Overlap drawing and sending (free, and already planned)

Right now the model assumes the CPU draws the whole frame and then waits for
all of it to go out: 1.3 + 15.4 = 16.6ms. With DMA and a second buffer, the
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

### 2. Run the bus faster — already spent on the default panel

40MHz was a conservative assumption, and the theoretical case for 80MHz
looked good: the ST7789 datasheet is around 62MHz for serial writes, and
80MHz is widely used in practice on the ESP32-S3. Going from 40 to 80 doubles
the ceiling — **32fps to 65fps** — for a single configuration line and no code
changes, if the wiring cooperates.

**It depends entirely on the panel, which is why the clock now lives with the
panel.** Both figures come from `docs/hardware-bringup.md`'s Stage 2b clock
sweep, on breadboard wiring:

- **2.8in ILI9341, 2026-08-08: 40MHz.** 40 rendered the test card correctly;
  80MHz came out **solid white** — wholesale data corruption, not a subtle
  glitch.
- **2in ST7789, 2026-08-14: 80MHz.** The sweep held at 80, and the real game
  then rendered correctly there over a long run, which is the stronger of the
  two results: a seven-second test card cannot see an occasional dropped bit,
  and minutes of on-screen text can.

Each lives in its panel's `spi_hz` in `ports/esp32/hal/kf_panel_profile.h`.
A single global constant could only ever have been right for one of them.

**On the default panel this lever is now spent.** 80MHz is about as fast as
the S3's SPI peripheral drives a display at all, so better wiring may make 80
more *reliable* but will not unlock more. The next step up is a parallel
interface, below. On an ILI9341 build it is worth re-testing if the wiring
improves (shorter leads, a PCB instead of jumpers) — but treat that as a
re-test, not a free win still on the table.

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
  taking 80MHz SPI from 65fps to 87fps, or 40MHz from 32fps to 43fps. It also
  changes what the pet looks like, so it is a design call.
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

**The clock is measured, not assumed, and it is not global.** The device
reads its panel profile's `spi_hz`: 80MHz on the default ST7789, 40MHz on the
ILI9341. `KF_DISPLAY_SPI_HZ` in `budget.h` still exists at `80000000`, but
only as the figure the **desktop** backends report as their link speed — it
tracks the primary panel so the simulator is pessimistic against the device
you are most likely to build, and nothing on the device reads it.

The drawing figures are still estimates, and are flagged in the file:

**`KF_DRAW_OPAQUE_PX_PER_US = 100`, `KF_DRAW_KEYED_PX_PER_US = 25`.** How fast
the device fills pixels. Genuine guesses, and could reasonably be out by a
factor of two either way.

That uncertainty does not change any conclusion here, which is worth saying
explicitly. Drawing is 8% of the frame at 80MHz. If the estimate is wrong by
4x in the pessimistic direction, drawing becomes about a third of the frame
and transfer still dominates. The conclusion "you are wire-bound" is robust to
being quite badly wrong about the CPU.

They get corrected as the device is measured further, and the simulator's
numbers become accurate rather than merely useful.

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

- Full-screen animation works. About 60fps today on the default 2in ST7789,
  about 31fps on the 2.8in ILI9341.
- Drawing is 8% of the frame. The wire is 92%.
- Dirty rectangles are how a quiet screen gets cheap, not a limit on a busy
  one. Pass the whole screen when the whole screen changes.
- The biggest lever was the bus, and it has been pulled: 80MHz on the ST7789
  doubled the ceiling for one line of configuration. The ILI9341 came out
  solid white at 80 and stays at its measured 40MHz, which is why the clock
  is a per-panel field rather than a project-wide constant.
- The decision this eventually forces is a hardware one, SPI versus parallel
  panel, and it is due before the PCB rather than now.
