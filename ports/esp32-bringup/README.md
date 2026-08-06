# ports/esp32-bringup

A hardware diagnostic. **Not** the firmware.

```
idf.py set-target esp32s3
idf.py flash monitor
```

It answers one question per peripheral, in the order
[`docs/hardware-bringup.md`](../../docs/hardware-bringup.md) tells you to
wire them: *is this thing connected to the pins the firmware thinks it is?*

Six stages -- board identity, backlight, display, I2C/RTC, microSD, buttons
-- each printing `PASS` or `FAIL` plus the specific wire to go and look at.
A failing stage never stops the run, so one flash cycle gives you the whole
picture rather than one fix per cycle. No menu, no serial input: press `RST`
to run it all again.

## Why this is a separate project from `ports/esp32`

It shares exactly one file with the firmware,
[`../esp32/hal/kf_esp_pins.h`](../esp32/hal/kf_esp_pins.h), and depends on
nothing else in this repository -- not hakoniwaos, not the arena allocator,
not LVGL, not Lua, not the pet. There is deliberately no
`EXTRA_COMPONENT_DIRS`.

When nothing appears on a screen you have just wired for the first time, the
list of things that could be wrong should be as short as possible, and
"maybe hakoniwaos failed to compile" should not be on it. Sharing the pin
header is the part that matters: a bring-up that passes cannot be testing
different pins than the firmware will use.

See ADR 0023 for the full reasoning, including why the microSD card gets its
own SPI bus and why the bring-up clocks run at half the firmware's.

## Start here

`docs/hardware-bringup.md` for the procedure and the pin table.
`kamiframe-wiring-guide.html` in the planning folder for the colour-coded
version to keep open at the bench.
