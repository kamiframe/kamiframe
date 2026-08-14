# Hardware bring-up

How to wire the first real Kamiframe board on a breadboard, in what order,
and how to tell whether each part worked.

This doc and `ports/esp32/hal/kf_esp_pins.h` have to agree. That header is
where the firmware actually reads its pin numbers from; this file is what a
human reads while holding a wire. Change one, change the other.

There is a visual version of this with colour-coded wire lists and a board
map: `kamiframe-wiring-guide.html`, in the planning folder. Use that at the
bench. Use this one to understand why the pins are what they are.

---

## What the MVP is, and what it deliberately isn't

The parts ordered in August 2026 cover roughly the whole target spec. The
bring-up board uses four of them:

| Part | Why it's in the MVP |
|---|---|
| ESP32-S3-DevKitC-1 N16R8 | the target chip, in a form that plugs into a breadboard |
| 2" ST7789 IPS 240x320 | the exact panel `esp_display.cpp` is written for |
| DS3231 RTC + CR2032 | the pet ages while powered off; without this there is no pet |
| microSD reader | where assets and saves will live |
| 4-pin tactile switches | input |

Everything else waits: the IMU, the light sensor, the microphone, the
amplifier and speaker, the haptic driver and its motor, the buzzer, the
BME280, the TP4056 charger and the LiPo cells. Not because they are hard,
but because each one needs firmware that does not exist yet, and adding
untested hardware to untested wiring means you cannot tell which one is
wrong.

## The display for real units — buy later, decided now

The breadboard panels are development hardware. Neither is what the real
units should ship with, and ADR 0032 explains why in full: neither exposes
a tearing-effect (TE) signal, so both flicker on content that is actually
changing, and nothing in software can fix that.

The intended production display is Adafruit's EYESPI ecosystem. Not yet
bought — there is no reason to spend before the software needs it — but
recorded here so the decision does not have to be made again under time
pressure.

| Part | Adafruit # | Why this one |
|---|---|---|
| [2.0" 320x240 Color IPS TFT (ST7789)](https://www.adafruit.com/product/4311) | 4311 | The size the product actually wants, in the resolution the whole firmware assumes. IPS, so viewing angle does not depend on how someone holds it. |
| [EYESPI Breakout Board, 18-pin FPC](https://www.adafruit.com/product/5613) | 5613 | Fans the flex out to breadboard headers. Development only -- the real PCB carries its own FPC connector and this becomes unnecessary. |
| [EYESPI Cable, 100mm](https://www.adafruit.com/product/5239) | 5239 | The sensible default length for bench work. |
| [EYESPI Cable, 50mm](https://www.adafruit.com/product/5462) | 5462 | For a compact enclosure, where 100mm has to be folded somewhere. |
| [EYESPI Cable, 200mm](https://www.adafruit.com/product/5240) | 5240 | Only if the board and panel end up far apart. |

Buy the display, the breakout, and one 100mm cable. The other lengths are
worth knowing about, not worth buying up front.

**Verify TE before committing the PCB to this.** The EYESPI connector
standard includes a TE position, but a display only drives it if its own
flex routes it -- exactly the trap the ILI9341 sprang, where the controller
has the pin and the module does not bring it out. This 2.0" display's own
92-page guide never mentions TE at all, which is not proof of absence but is
not encouraging either. Check the breakout's pinout page for which numbered
pin TE is, then confirm against the display's schematic, or simply ask
Adafruit. One question answered before ordering beats rediscovering this
after a PCB run.

Two things this display gains regardless of how TE turns out, both of which
the current 2.8in module cannot do at all:

- **PWM backlight control.** The HiLetgo's LED pin is tied to 3V3 with no
  control, so the screen cannot be dimmed or switched off. On a
  battery-powered device that sleeps, that is a real cost, not a nicety.
- **A flex connector instead of eight jumper wires**, which is the correct
  mechanical answer for anything that goes in an enclosure.

**Both breadboard panels work as of 2026-08-13.** They are both 240x320 SPI
and wire almost identically — the 2.8" is an ILI9341, the 2" is an ST7789 —
and `esp_display.cpp` drives both through one panel-profile mechanism (ADR
0039, `KF_PANEL`). The **2" ST7789 is the default profile** (ADR 0059); the
ILI9341 is the supported alternative and is still the only one the `KFDBG
SCANLINE`/`VSYNC` diagnostics work against, since it is the only module here
with a data-out line.

```bash
idf.py -DKF_PANEL=ili9341 build     # only if you are on the 2.8"
```

Two things to know at the bench:

- **One panel at a time.** They share CS on GPIO10, so leaving both
  connected puts two controllers on one chip select.
- **The only wiring difference is GPIO6.** On the ILI9341 it is that
  module's SDO; on the ST7789 it is BL, a real backlight pin you can
  actually drive — the HiLetgo's LED is tied straight to 3V3 and has never
  been controllable. The firmware picks the role from the profile's
  `has_read_line`, so this is a wire to move, not a flag to set.

**The battery is last, not first.** The 503030 cells ship with bare leads
that need JST PH connectors soldered on, and feeding the devkit's 5V pin
from a TP4056 while USB is also connected needs care. None of that is hard;
all of it produces symptoms that look like software bugs if it happens while
the software is still unproven. Run off USB until every stage below passes.

---

## Pin allocation, and the reasoning

The ESP32-S3-DevKitC-1 breaks out 44 pins. After removing everything that
cannot be used, **23 are available**, and this layout spends 19 of them.

### What cannot be used, and why

| Pins | Reason |
|---|---|
| 26-32 | the module's quad SPI flash. Not on the headers, so not a hazard. |
| **33-37** | **octal PSRAM on the N16R8.** 35, 36 and 37 *are* on the J3 header. This is the one real trap on this board. |
| 19, 20 | native USB (D-/D+) |
| 43, 44 | UART0, the serial console every diagnostic prints to |
| 0, 3, 45, 46 | strapping pins, sampled at reset |
| 38 *and* 48 | the onboard RGB LED. v1.0 uses 48, v1.1 uses 38. Both avoided so the pinout doesn't depend on which board came out of the bag. |

Wiring GPIO 35, 36 or 37 does not fail cleanly. It produces a board that
boot loops or corrupts memory, which reads as "I soldered something wrong"
and costs an evening. They sit on the header between pins that are perfectly
fine, which is exactly why the visual guide marks them in red.

### The allocation

| Function | Pin | Notes |
|---|---|---|
| Display MOSI | 11 | SPI2 |
| Display SCLK | 12 | SPI2 |
| Display CS | 10 | |
| Display DC | 7 | |
| Display RST | 8 | |
| Display backlight | 6 | plain GPIO on/off, not PWM |
| I2C SDA | 13 | RTC now; IMU, light sensor, haptic driver later |
| I2C SCL | 14 | |
| SD SCLK | 39 | SPI3 |
| SD MOSI | 40 | SPI3 |
| SD MISO | 41 | SPI3 |
| SD CS | 42 | |
| Buttons UP/DOWN/LEFT/RIGHT | 4, 5, 15, 16 | active-low, internal pull-ups |
| Buttons A/B/MENU | 17, 18, 21 | |
| *reserved* I2S BCLK/WS | 1, 2 | shared by amplifier and microphone |
| *reserved* I2S DOUT / DIN | 9, 47 | DOUT swapped with Display DC — was 7, now 9 (`kf_esp_pins.h`) |

That leaves nothing spare. The passive buzzer never gets a pin, because the
MAX98357A does everything it does and better.

### Why the SD card gets its own SPI bus

Sharing one SPI bus between the display and the card is the textbook
arrangement and would save four pins. It is not what this layout does.

The common level-shifted microSD breakout modules -- the 6-pin ones with a
regulator and a buffer chip -- are well known for not releasing MISO when
their chip-select goes high. On a shared bus that corrupts the display's
traffic intermittently: a screen that mostly works, occasionally tears, and
gives no clue why. That is a miserable first-hardware debugging session, and
four pins is a cheap price to make the whole class of bug impossible.

This constraint is about the breakout module, not about SPI. A custom PCB
with a card socket wired directly can share the display's bus safely, and
should, because the pin budget above shows the devkit is nearly full.

### What this says about the real PCB

19 of 23 usable pins, with seven of them spent one-per-button, is a warning
worth acting on before the flagship board is laid out. An I2C GPIO expander
would return six pins for two, on a bus that already exists. Not a decision
for today, but the number is here so it isn't a surprise later.

---

## Assembly order

Wire one stage, flash, confirm, then wire the next. Resist doing it all at
once: a board that fails with everything connected gives you a list of
suspects, and a board that fails one stage after the last one passed gives
you an answer.

### 0. Soldering

Almost none. Header pins onto the display, the SD reader, and the RTC if it
did not come pre-soldered. Short ends through the board from the top, long
ends pointing down so they reach the breadboard. Push the header strip into
a breadboard first and rest the module on top of it -- the breadboard holds
everything square while you solder from above.

The tactile switches push straight into a breadboard. The devkit already has
its headers on.

**Modify the DS3231 module before installing the coin cell.** These boards
carry a trickle charger intended for a rechargeable LIR2032. A CR2032 is not
rechargeable, and left as-is the circuit pushes current into a cell that
cannot accept it. Remove the small resistor marked `201`, or the diode
beside it, near the battery holder. Heat one end, heat the other, flick it
off.

### 1. Power rails

`3V3` to a breadboard `+` rail, `GND` to a `-` rail, `5V` to the other `+`
rail, and one wire joining the two `-` rails together. Everything downstream
takes power from those rails rather than running its own wire back to the
board.

That last wire is not optional. The SD module runs from 5V and everything
else from 3V3; without a shared ground they have no common reference and the
card behaves erratically.

### 2. Display

Eight wires, per the table above. Backlight first in the test order because
it needs only power and one GPIO, and it answers "does this panel have
power at all" before any SPI is involved.

### 3. DS3231

Four wires. Two of them are the I2C bus that will later also carry the IMU,
the ambient light sensor and the haptic driver, which is the entire reason
only two pins are spent on it.

Note that the DS3231 and the MPU-6050 both answer on address `0x68`. When
the IMU is added, one of them has to move -- the MPU's `AD0` pin pulled high
puts it on `0x69`.

### 4. microSD

Six wires. Power it from **5V**, not 3V3: the module has its own regulator.
Format the card FAT32 first; Windows defaults to exFAT for anything 64GB and
up, and exFAT will not mount.

### 5. Buttons

Two wires each, no resistors -- the chip's internal pull-ups are enabled in
`esp_input.cpp` and a press just ties the pin to ground.

**A 4-pin tactile switch has two real connections, not four.** The legs are
joined in pairs inside the body, and pressing connects the two pairs. Wire
the pair that is already joined and you have made a permanent short, which
the firmware reports as a button held down forever. Wiring diagonally
opposite corners is always correct.

Four buttons is enough to start. An unwired pin simply never reads as
pressed.

---

## Running the diagnostic

**Plug into the port labelled `UART`, not the one labelled `USB`.** The
DevKitC-1 has two USB-C ports. `UART` goes through the onboard USB-to-serial
chip and is wired to GPIO43/44 -- the same UART0 every stage above prints
to, and the port `idf.py flash monitor` below needs. `USB` is the chip's
native USB (GPIO19/20, reserved in the pin table above) and nothing in this
repo talks over it yet. Plugging into `USB` instead is the most common
reason a board looks "dead" during bring-up when it is actually fine --
`idf.py` will simply find no serial port to flash.

Once it's plugged into `UART`, it shows up as a serial device you point
`idf.py -p` at -- `COMx` on Windows, `/dev/cu.usbserial-XXXX` (or
`/dev/cu.SLAB_USBtoUART`) on macOS. If it doesn't show up at all, that's
usually the Silicon Labs CP210x driver missing, not a wiring problem.

```
cd ports/esp32-bringup
idf.py set-target esp32s3
idf.py flash monitor
```

`ports/esp32-bringup` is a separate ESP-IDF project from `ports/esp32`. It
shares exactly one file with the firmware -- `kf_esp_pins.h` -- and depends
on nothing else in this repo. That is deliberate: when nothing appears on a
screen you have just wired for the first time, you want the shortest
possible list of things that could be wrong, and "maybe hakoniwaos failed to
build" should not be on it.

It runs six stages in order and prints `PASS` or `FAIL` for each, with the
specific wire to go and look at on failure. A failing stage never stops the
run, so one flash cycle gives you the whole picture rather than one fix per
cycle. There is no menu and no serial input: press `RST` on the devkit to
run everything again.

| Stage | Claim |
|---|---|
| 0 | the chip is what it says: 16MB flash, ~8MB PSRAM, 240MHz |
| 1 | the backlight GPIO drives the panel |
| 2 | the panel accepts frames over SPI, with the right geometry |
| 3 | the I2C bus works and the DS3231 is present and ticking |
| 4 | the card mounts, and a file survives a write/read round trip |
| 5 | every button reads correctly (runs forever) |

Stage 2 finishes on a test card that holds still for ten seconds: a red bar
across the top with a white notch at its left end, a green stripe down the
left edge, a blue bar across the bottom, and eight patches in the middle --
white/black, red/green, blue/yellow, cyan/magenta.

Every element of that is load-bearing. The bars and the stripe make geometry
faults visible rather than merely suspected: a clipped edge means the panel
needs an offset (`esp_lcd_panel_set_gap`), and a stripe on the wrong side
means the rotation byte is wrong. The patches separate the two colour faults
that otherwise look identical -- white and black survive both a byte-order
mistake and a BGR mistake, so *white and black correct while the saturated
patches are wrong* means byte order, and everything wrong together means
BGR. Three coloured bands could not tell those apart, which is exactly how
the ILI9341 byte-order bug cost an evening.

Stage 5 splits the screen into seven colour bands and lights a band while
its button is held.

### When a panel shows nothing at all

Set `kPanelDebugMode = true` at the top of `bringup_main.cpp` and reflash.
That turns on the dead-panel investigation kit, which is off by default
because it costs about three minutes per run:

- **stage 2a**, which drives each control line high for six seconds and low
  for six, slowly enough to follow with a multimeter at the panel's own
  pads. This is the only test here that proves a signal *arrives*. Every
  `esp_lcd` call returning `ESP_OK` does not, because these panels have no
  data-out pin and nothing is ever read back.
- **stage 2 steps A/B/C**, which send nothing, then one square, then a
  second square, so a panel that dies when written to can be told apart
  from one that never woke up.
- the test card holds for 90 seconds instead of 10, long enough to
  photograph.

This is what proved the first Waveshare module's DC line was dead at the
panel while its GPIO was driving perfectly. Reach for it before assuming the
wiring is wrong.

### The bring-up clock runs slower on purpose

Stage 2 drives the panel at **4MHz** and stage 4 drives the card at 10MHz,
both far below what the firmware will eventually ask for. Breadboard jumper
wires are not controlled-impedance anything, and the panel here runs through
a chain of Dupont jumpers with inline couplers. A panel that works at 4MHz
and fails at 40 is a wire-length problem, not a wiring-order problem, and
those are much easier to tell apart when the first attempt is the slow one.

(This started at 20MHz and was dropped to 4 during the first bring-up. The
constant is `kLcdClockHz` in `bringup_main.cpp`, and it is deliberately the
*safe* clock rather than the target one.)

### Stage 2b: measuring the real clock ceiling

`KF_DISPLAY_SPI_HZ` in `kf/budget.h` is 40MHz and marked **assumption, not
measured**. Every claim the desktop simulator makes about transfer cost and
frame budget rests on it, so replacing it with a measured number is one of
the things bring-up exists for.

Stage 2b redraws the test card at 4, 10, 20, 40 and 80MHz in turn, seven
seconds each, then returns the panel to the safe clock. The clock maps
straight onto a frame-rate ceiling, because a full 240x320 RGB565 frame is
153,600 bytes:

| SPI clock | Per frame | fps ceiling |
|---|---|---|
| 4MHz | 307ms | ~3 |
| 10MHz | 123ms | ~8 |
| 20MHz | 61ms | ~16 |
| 40MHz | 31ms | ~32 |
| 80MHz | 15ms | ~65 |

**The firmware cannot score this and it is important to know why.** The bus
is write-only -- neither module has a data-out pin -- so nothing is ever read
back, and `ESP_OK` only means the ESP32 clocked bytes out of its own pin. At
the speed where the wiring gives up, every call still returns `ESP_OK` and
the picture is simply wrong. So you watch the glass, not the log.

Look for the highest speed where the card is still perfect: solid patches,
clean edges, no fizzing, no torn or shifted rows, no colour speckle. The
first speed that misbehaves is the ceiling; the one below it is the answer,
and that is the number `KF_DISPLAY_SPI_HZ` should become.

80MHz is on the list rather than being obviously hopeless because SPI2's
IOMUX pins on the ESP32-S3 are exactly the ones this pinout uses (CLK 12,
MOSI 11, CS 10), which is the configuration that can bypass the GPIO matrix
and reach the full rate. Whether breadboard jumpers can is the real question.

#### Result, 2026-08-08

**40MHz.** It rendered the card correctly; 80MHz came out solid white, which
is what wholesale data corruption looks like on this panel. `KF_DISPLAY_SPI_HZ`
was already guessing 40MHz, so the number did not move -- but it is now
measured, its `ASSUMPTION, NOT MEASURED` banner is gone, and the ~32fps
full-frame ceiling the frame budget is built on is real.

`kRunClockSweep` is therefore `false`. Turn it back on when the wiring or the
panel changes.

**Still outstanding for the 2in ST7789**, which is now the default panel
(ADR 0059) but was verified for *picture*, not for *speed*: no clock sweep
has ever been run against it. It inherits 40MHz from a measurement taken on
the 2.8in ILI9341, which is safe — ST7789 modules commonly tolerate more —
but it means the frame budget's ~32fps full-frame ceiling is, on the primary
panel, a conservative inherited number rather than a measured one.

Two things worth remembering about that number. 40MHz is about four times the
ILI9341 datasheet's own write-cycle figure -- it works, and it is what
practically every driver for this panel does, but it is outside spec and
could be marginal on another unit or at a different temperature. And it was
measured through Dupont jumpers with inline couplers, which is close to the
worst wiring this project will ever have, so treat it as a floor rather than
a ceiling for the real PCB.

---

## The power-off test, which takes two runs

Stage 3 confirms in one run that the DS3231 is present and its oscillator is
running. It cannot confirm the thing that actually matters in one run,
because the thing that matters only happens between two of them: that the
clock keeps time with the USB cable pulled out.

So it takes two runs, and the diagnostic does the comparing:

1. run it once. The summary prints `RTC survives power-off  RUN AGAIN TO
   FIND OUT` and saves the clock's reading to flash.
2. **unplug the board completely** -- USB out, not just the `RST` button,
   which does not remove power and therefore tests nothing.
3. wait 30 seconds.
4. plug it back in and let it run again.

The second run prints `PASS` or `FAIL` on its own, along with how many
seconds the clock advanced while it was unplugged. There is no timestamp to
remember and nothing to compare by eye.

That is the whole feature. `kf_time_wall()`'s `valid` flag, the offline
fast-forward path in `kf/pet.h`, and every life-stage transition that
happens while the device is in a drawer all rest on it. A `FAIL` here means
the pet resets its age every time the device loses power, and the fix is
almost always the coin cell: flat, in backwards, not making contact in its
holder, or slowly killed by the trickle-charge resistor that should have
been removed during assembly.

The baseline lives in NVS, in flash, which is the same storage the firmware
saves the pet into -- so it survives exactly the power cut being tested. It
does not survive `idf.py erase-flash`, which simply puts you back at step 1.

---

## What is measured now, and what still is not

This section used to be called "known unknowns going in" and said that none
of it had been run on hardware. That is no longer true.

**Measured on a real board, 2026-08-07/08.** Every pin in `kf_esp_pins.h`,
via a full pass of the diagnostic: backlight, panel over SPI, I2C with a
DS3231 present and keeping time across a real power cut, a microSD card
mounting and round-tripping a file, and all seven buttons. `kf_esp_pins.h`
has dropped its *assumption, not measured* banner. `KF_DISPLAY_SPI_HZ` is a
measured 40MHz. The ILI9341 needed no x/y offset -- the test card's bars
reach all four edges.

One pin moved: LCD_DC from GPIO9 to GPIO7, because GPIO9 measured under a
millivolt at the panel while every other line swung a clean 3.3V. GPIO9 is
FSPIHD, one of SPI2's own IOMUX pins, and this build drives SPI2 on the other
three.

**Still not measured.** The I2S lines are reserved and have never been wired.
`KF_DISPLAY_TRANSFER_OVERHEAD_BYTES` in `kf/budget.h` keeps its assumption
banner -- the clock sweep tested throughput, not per-rectangle overhead. The
primary 2in ST7789 panel now displays correctly (ADR 0059) but has had no
timing measured on it at all; every figure above was taken on the 2.8in
ILI9341 and is inherited, not confirmed, on the panel that is now the default.
And none of these figures have been re-measured on a real PCB, where they
should all improve.

**Still true, and worth keeping:** both `ports/esp32` and `ports/esp32-bringup`
build clean against ESP-IDF v6.0.2 for the esp32s3 target, and the
diagnostic's drawing routines were rendered natively and inspected before
they were ever trusted on glass -- because a test frame that is itself wrong
sends you looking for a wiring fault that does not exist.
