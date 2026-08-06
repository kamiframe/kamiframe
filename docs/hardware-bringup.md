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

**Use the 2" Coolwell panel, not the 2.8" HiLetgo.** Both are 240x320 SPI
and they wire identically, but the 2.8" is an ILI9341 and the 2" is an
ST7789. `esp_display.cpp` calls `esp_lcd_new_panel_st7789()`, so the ILI9341
will stay black no matter how correct the wiring is. Supporting it later is
a one-function change and a good second target, since it also has a touch
controller.

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
| Display DC | 9 | |
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
| *reserved* I2S DOUT / DIN | 7, 47 | |

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

Stage 2 finishes on a deliberately diagnostic frame: a white field, a 6px
red border touching all four edges, and a green square in the top-left
corner only. That makes geometry problems visible instead of merely
suspected -- a clipped border means the panel needs an offset
(`esp_lcd_panel_set_gap`), a green square in the wrong corner means the
rotation settings are wrong, and both are one-line fixes you cannot make if
you cannot see them.

Stage 5 splits the screen into seven colour bands and lights a band while
its button is held.

### The bring-up clock runs slower on purpose

The diagnostic drives the panel at 20MHz and the card at 10MHz, both roughly
half of what the firmware will ask for. Breadboard jumper wires are not
controlled-impedance anything. A panel that works at 20MHz and fails at 40
is a wire-length problem, not a wiring-order problem, and it is much easier
to tell those apart if the first attempt is the slow one.

`KF_DISPLAY_SPI_HZ` in `kf/budget.h` is still an assumption (40MHz).
Measuring the real achievable figure is a bring-up task in its own right,
and it should be measured on something closer to the real board than a
breadboard.

---

## The one test the firmware cannot run for you

Stage 3 confirms the DS3231 is present and its oscillator is running. It
cannot confirm the thing that actually matters.

After stage 3 passes:

1. unplug the board completely, and leave it for 30 seconds
2. plug it back in and run the diagnostic again
3. the clock must report a time that moved forward across the gap, and must
   **not** report that its oscillator stopped

That is the whole feature. `kf_time_wall()`'s `valid` flag, the offline
fast-forward path in `kf/pet.h`, and every life-stage transition that
happens while the device is in a drawer all rest on it. If the oscillator
flag is set on every boot, the coin cell is dead, inserted backwards, or the
holder is not making contact -- and the pet will reset its age every time the
battery runs out.

---

## Known unknowns going in

Everything in `kf_esp_pins.h` is still marked *assumption, not measured*,
and stays that way until a real board has run against it. The same applies
to `KF_DISPLAY_SPI_HZ` and to the ST7789's need (or not) for an x/y offset,
which varies between modules that are otherwise identical.

What has been verified: both `ports/esp32` and `ports/esp32-bringup` build
clean against ESP-IDF v6.0.2 for the esp32s3 target, and the diagnostic's
two drawing routines were rendered natively and inspected, because a test
frame that is itself wrong would send you looking for a wiring fault that
does not exist.

What has not: any of it, on hardware. That is the point of the exercise.
