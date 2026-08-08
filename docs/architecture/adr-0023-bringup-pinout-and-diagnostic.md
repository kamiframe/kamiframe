# ADR 0023: The bring-up pinout, and a diagnostic separate from the firmware

**Status:** Accepted
**Date:** 2026-08-06

## Context

ADR 0020 delivered real ESP32 HAL backends and a pin file, `kf_esp_pins.h`,
covering the display and the buttons. Parts arrived in August 2026, so the
pin file now has to cover everything the first real board needs, and someone
with no embedded background has to be able to wire it.

Two things were missing. The pin file had no I2C bus and no microSD pins,
which are the two peripherals the firmware's own documented gaps depend on
(`esp_time.cpp`'s wall clock, and asset/save storage). And there was nothing
to run on a freshly wired board that would say which wire was wrong.

## Decisions

### 1. The microSD card gets its own SPI bus (SPI3), not the display's

Sharing one bus is the textbook arrangement and would have saved four pins
on a board that has very few to spare.

Rejected because the common level-shifted microSD breakout modules -- the
6-pin ones with an onboard regulator and a buffer chip -- are well known for
failing to release MISO when their chip-select goes high. On a shared bus
that corrupts display traffic intermittently, producing a screen that mostly
works and occasionally tears. Intermittent corruption on first-ever hardware,
debugged by someone who has not debugged hardware before, is the worst
possible failure mode to design in for the sake of four pins.

This constraint is about the module, not about SPI. **A custom PCB with a
card socket wired directly should share the display's bus**, and the pin
budget below says it will want to.

### 2. GPIO 33-37 are documented as unavailable, not just 26-32

The pin file previously named GPIO26-32 as the reserved range. That is the
quad SPI flash, and it is not broken out on the DevKitC-1 headers at all, so
it was never a hazard. The octal PSRAM on the N16R8 uses **33-37**, and 35,
36 and 37 *are* on the J3 header, sitting between pins that are perfectly
usable.

Using one does not fail loudly. It produces a board that boot loops or
corrupts memory, which reads as a soldering fault. Both ranges are now named
explicitly, and the visual wiring guide marks them in red.

### 3. I2C on 13/14; the whole sensor suite shares it

The DS3231 is the only I2C device the MVP wires, but the IMU, the ambient
light sensor and the DRV2605L haptic driver all live on the same two wires
later. Spending two pins now covers four peripherals.

Recorded here because it has a consequence: the DS3231 and the MPU-6050 both
answer on `0x68`. When the IMU is added, its `AD0` pin has to be pulled high
to move it to `0x69`.

### 4. The diagnostic is a separate ESP-IDF project, not a build flag

`ports/esp32-bringup` shares exactly one file with the firmware
(`kf_esp_pins.h`) and depends on nothing else in the repo -- not hakoniwaos,
not the arena allocator, not LVGL, not Lua, not the pet.

The alternative, a `KF_BRINGUP` flag inside `ports/esp32`, would have avoided
a second project. Rejected because it puts the whole firmware's build on the
critical path of "is this wire connected." When nothing appears on a screen
you have just wired for the first time, the list of things that could be
wrong should be as short as it can possibly be, and "maybe hakoniwaos failed
to compile" should not be on it.

Sharing the pin header is the part that matters: a bring-up that passes
cannot be testing different pins than the firmware will use.

### 5. Bring-up clocks are half the firmware's

20MHz to the panel, 10MHz to the card, against `KF_DISPLAY_SPI_HZ`'s assumed
40MHz. Breadboard jumper wires are not controlled-impedance anything, and a
panel that works at 20MHz and fails at 40 is a wire-length problem rather
than a wiring-order problem. Separating those two failures is worth more
during bring-up than speed is.

## The pin budget, and what it implies

After removing flash (26-32), octal PSRAM (33-37), native USB (19, 20),
UART0 (43, 44), the strapping pins (0, 3, 45, 46) and the RGB LED (38 on
v1.1, 48 on v1.0), **23 pins are available**. This layout spends 19, and
reserves the remaining four for I2S audio -- where the amplifier and the
microphone must share BCLK and WS, because a second I2S bus would need three
pins that do not exist.

Seven of those pins go one-per-button. **That is a finding for the flagship
PCB, not just an observation:** an I2C GPIO expander would return six pins
for two, on a bus that already exists. Not decided here, but the number is
recorded so it is not a surprise during layout.

## Verified

- `ports/esp32-bringup` builds clean for esp32s3 against real ESP-IDF
  v6.0.2, zero warnings under the IDF's own `-Wall -Wextra -Werror`.
- `ports/esp32` still builds clean with the extended `kf_esp_pins.h`, so the
  additions did not disturb ADR 0020's backends.
- The diagnostic's two drawing routines (`draw_alignment_frame()` and
  `draw_button_bands()`) were extracted verbatim, compiled natively against
  a stub that captures a full framebuffer, and rendered to images that were
  inspected. Both were also checked for full coverage (no pixel left
  unwritten by the strip loop) and for band-mapping overflow. A test frame
  that is itself wrong would send someone looking for a wiring fault that
  does not exist.
- The DevKitC-1 header order and the RGB LED pin were checked against
  Espressif's own v1.1 user guide rather than remembered.

## Not verified

Any of it, on hardware. `kf_esp_pins.h` keeps its *assumption, not measured*
banner. The specific figures to correct on day one are `KF_DISPLAY_SPI_HZ`
and whether this particular ST7789 module needs an x/y offset -- which varies
between modules that are otherwise identical, and is why stage 2 draws a
border touching all four edges.

## Cost to change

A pin move is one line in `kf_esp_pins.h`, one row in
`docs/hardware-bringup.md`, and one entry in the visual guide's board map.
Moving the SD card back onto the display's bus is a slightly larger change
to `esp_storage.cpp`'s eventual card support and to stage 4 of the
diagnostic, and should happen at PCB design time rather than on a
breadboard.

## Bring-up results, 2026-08-07/08

The pinout survived first contact with one change and three surprises. All
of this is measured on the board in hand, not reasoned about.

### GPIO9 is not usable as LCD_DC on this board

Driven high as a plain GPIO it produced under a millivolt at the panel,
while RST, CS, CLK and DIN all swung a clean 3.3V through the same jumpers.
GPIO9 is FSPIHD, one of SPI2's own IOMUX function pins, and this build
drives SPI2 on 10/11/12 which are the other three. DC moved to GPIO7 and
I2S_DOUT took GPIO9 in exchange. See the comment in `kf_esp_pins.h`.

### The devkit's 5Vin pin is diode-blocked from USB VBUS

It reads 0.55V out of the bag. An IN-OUT solder jumper next to GPIO 10-12
bridges it. Once bridged, never feed external 5V into that pin while USB is
connected.

### SDIO probing breaks plain SD cards

`sdmmc_init.c` calls `sdmmc_io_reset()`, which sends CMD52; a memory card
answers with the function-number-error bit, `r1_sdio_response_to_err()`
maps that to `ESP_ERR_INVALID_ARG`, and `sdmmc_io_reset()` only tolerates
TIMEOUT, NOT_SUPPORTED and INVALID_CRC. The card mounts fine once
`CONFIG_SD_ENABLE_SDIO_SUPPORT=n` is set. This belongs in the real port's
`sdkconfig.defaults` too, not just the diagnostic's.

### Framebuffer byte order is a trap on any non-ST7789 panel

This one cost an evening and is the reason to write it down. RGB565 goes on
the wire high byte first. The ESP32-S3 is little-endian, and
`panel_io_spi_tx_color()` hands colour data to SPI exactly as it lies in
memory -- only commands and parameters get byte-reversed. So a `uint16_t`
framebuffer is transmitted backwards.

The ST7789 hides this: RAMCTRL (0xB0) carries a little-endian bit, and
`esp_lcd_new_panel_st7789()` sets it from `panel_dev_config.data_endian`.
But that bit is only ever sent by `esp_lcd_panel_init()`, and the ILI9341
path skips `esp_lcd_panel_init()` because 0xB0 is a different register on
that controller. The ILI9341 has no equivalent register at all. It is
always big-endian, so the framebuffer has to be.

Measured, on full-screen fills where no geometry is involved:

| sent   | intended | panel read | shown |
|--------|----------|-----------|-------|
| 0xF800 | red      | 0x00F8    | blue  |
| 0x07E0 | green    | 0xE007    | pink  |
| 0x001F | blue     | 0x1F00    | green |
| 0xFFFF | white    | 0xFFFF    | white |
| 0x0000 | black    | 0x0000    | black |

All five match a plain byte swap and nothing else. Three horizontal colour
bands could *not* distinguish this from a BGR-bit fault, because both
produce "wrong colours" and neither produces a colour you can name -- which
is why the diagnostic now draws a single static test card with white and
black patches in it and holds it long enough to photograph. White and black
are invariant under byte order and under BGR, so their being correct while
the saturated patches are wrong is the signature that separates the two.

If the flagship panel ends up being an ILI9341 rather than an ST7789,
`esp_display.cpp` needs the same treatment: its own init table, no
`esp_lcd_panel_init()`, and a big-endian framebuffer.

### The Waveshare 2inch module in hand is faulty

Its DC line measured 0.7mV at the panel while RST, CS, CLK and DIN swung
3.3V, and pulling the wire showed the GPIO driving perfectly unloaded.
Soldering direct to the pad restored 3.3V and the panel still never
displayed. A HiLetgo 2.8in ILI9341 on the identical eight wires lit up
immediately. Returned.

### Settled ILI9341 configuration

Confirmed by photograph on 2026-08-08, after the byte-order fix:

- `MADCTL` (0x36) = `0x88`. MY set, MX clear, BGR set. Portrait with the
  module's pin header at the TOP, which is the mounting decision taken.
  Every Arduino library prints `0x48` for this panel, which is the same
  portrait rotated 180 degrees, i.e. header at the bottom. Only the two
  mirror bits differ, so switching later is one byte.
- `COLMOD` (0x3A) = `0x55`, 16 bit.
- Inversion OFF. The alignment frame came out on a white field with it off
  and a black one with it on.
- Framebuffer stored big-endian, per the section above.

The eight-patch test card read correctly in every position: the green edge
stripe on the left, the blue bar at the bottom, and the patches in order
white/black, red/green, blue/yellow, cyan/magenta. Magenta photographs as
lavender and yellow as yellow-green on this panel, which is the glass and
the camera, not the data.
