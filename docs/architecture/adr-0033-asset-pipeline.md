# ADR 0033: The asset pipeline — sprites packed into flash, memory-mapped, not copied

**Status:** Accepted
**Date:** 2026-08-09

## Context

The pet has always rendered as a coloured rectangle, or at best a small
hand-drawn blob, because there was nowhere to put art. Concretely, three
things were missing at once:

- `ports/esp32` used ESP-IDF's default single-app partition table — one
  1MB app partition, no NVS beyond a token 24KB, no assets partition at
  all — so there was no flash region an asset pack could even live in.
- `KF_FLASH_ASSET_BUDGET_BYTES` in `kf/budget.h` had budgeted 10MB of flash
  for assets that had no home: no partition backed it, and nothing ever
  checked a real pack against it.
- `KF_ARENA_ASSETS` (2MB of PSRAM) reported `0` bytes used, every frame,
  because nothing ever allocated from it except `kf/demo.cpp`'s own
  procedurally-generated tileset.

The one real sprite in the codebase, `examples/hello_sprite/sprite_data.h`,
was a 32x32 blob baked into a C header by `tools/make_test_sprite.py` —
explicitly a stand-in, checked in "because there is no asset pipeline yet."
This ADR is what replaces it.

## Decision

### The pack format

`tools/kf_pack_assets.py` writes a single binary file (`.kfpack`): a
16-byte header, a flat directory of fixed-size entries, then raw payload
data. No compression, no nesting, no per-platform variants — reading it
needs nothing but "seek and read a struct." The full byte-for-byte layout
is documented in that file's own header comment (the canonical copy) and
mirrored, field for field, in `hakoniwaos/src/assets.cpp`'s parser; nowhere
else needs to know the format.

Every directory entry carries an explicit **asset type** byte, not an
assumption that every entry is a sprite. This is deliberate and
forward-looking, not incidental: the board's planned MAX98357A I2S
amplifier plays arbitrary PCM, sound effects are coming later, and they
need exactly the same treatment sprites get — packed into flash, named,
memory-mapped, loaded identically on both backends. Building a second,
parallel pipeline for audio later would be exactly the kind of forked,
mimicking-not-shared mechanism this project's architecture rules already
forbid for the HAL. So the directory entry shape is:

```
name[32]  asset_type(1)  reserved(3)  type_meta[8]  data_offset(4)  data_bytes(4)
```

`type_meta` is 8 raw bytes whose meaning depends entirely on `asset_type`.
For `ASSET_TYPE_SPRITE` (0, the only type anything loads today) it is
width/height/color_key/has_color_key. `ASSET_TYPE_AUDIO_CLIP` (1) is
defined and documented — 16-bit signed PCM, mono, ~22kHz, uncompressed,
chosen because decoding a compressed format per playback costs CPU and RAM
a one-shot effect clip does not justify — but **no code anywhere reads or
writes it yet**. Nothing was implemented for audio; the field was left
open. `hakoniwaos/src/assets.cpp`'s directory walk (bounds-checking
name/data_offset/data_bytes) is intentionally type-agnostic — it validates
every entry the same way regardless of `asset_type`, and only the final
decode step branches on type — so a future `kf_assets_get_clip()` reads
the identical table this walk already builds. It is an addition to that
file, not a rewrite of it, and not a second pack format.

Pixel (and, later, PCM) data is stored **native-endian** — little-endian on
every target this project builds for (ESP32-S3 and every desktop host) —
never byte-swapped for a panel. `ports/esp32/hal/kf_panel_profile.h`'s
`big_endian_fb` flag exists precisely because one supported display module
needs its pixels reversed on the way to the glass and the other does not;
that swap happens in the display driver, per frame, right before the wire,
and must never be baked into the asset file, or the same pack would be
silently wrong on whichever panel it was not tuned for.

**Lookup is a linear string compare, not a hash.** `kf_assets_get()` scans
the parsed directory with `strcmp`. Real packs here are tens of entries,
looked up once at load time and cached (`kf/demo.cpp` calls it once in
`kf_demo_init()`, never per frame), so an O(n) scan costs nothing that
matters. A hash would need the Python packer and the C++ reader to agree
on an algorithm bit-for-bit forever — a cross-language contract this
codebase avoids paying for where it can (`tools/kf_debug.py`'s
`BUTTON_BITS` table makes the identical call, for the identical reason,
already).

### The partition table

`ports/esp32/partitions.csv` replaces ESP-IDF's default single-app table:

| Partition | Type | Size | Why |
|---|---|---|---|
| `nvs` | data | 24KB | ESP-IDF's own standard size, untouched. The pet's entire save is 70 bytes (`KF_PET_SAVE_BYTES`); this is wear-levelling headroom, not sized for this workload specifically. |
| `phy_init` | data | 4KB | Standard, untouched. |
| `otadata` | data | 8KB | Standard OTA slot selector — see "Why two app slots" below. |
| `ota_0` / `ota_1` | app | 1.5MB each | Current firmware is ~640KB (measured, see Verified). 1.5MB is real headroom: Lua and LVGL are already linked in, and every slice of this project so far has grown the binary, never shrunk it. |
| `assets` | data, custom subtype `0x40` | 12MB | Everything left over after the above, rounded to a clean number rather than consuming the chip's last ~896KB (which is simply unpartitioned spare capacity, not lost, just not committed). |

**`KF_FLASH_ASSET_BUDGET_BYTES` in `kf/budget.h` was raised from 10MB to
12MB to match this table exactly** — not to make a test pass (`kf/budget.h`'s
own banner explicitly forbids that), but because this is the actual
refinement that header's comment predicted would happen "once the
partition table is written." `kf/assets.cpp` checks a loaded pack's size
against this constant, which is what makes an over-budget pack fail on
desktop the same way it eventually would against real flash, rather than
being a limit only the device happens to enforce.

**Why two app slots, wired to no OTA code:** a partition table is baked in
at flash time, and repartitioning a device already in someone's hands is
real, avoidable pain — the kind of "expensive to undo" decision this
project's own instructions ask to flag rather than foreclose quietly.
`otadata` + `ota_0` + `ota_1` cost about 3MB of a 16MB chip and, with no
OTA client code written, today behave identically to a single `factory`
partition: nothing has ever chosen a non-default slot, so the bootloader
just runs `ota_0`. What they buy is the *ability* to add a real update
mechanism (`esp_https_ota` or similar — genuinely separate, unstarted
work) later without ever touching this layout again. The alternative — one
`factory` partition, cheaper today — would have foreclosed that option
quietly; this is the deliberate, named alternative instead.

The `assets` partition's type/subtype is `data`/`0x40` (the first
"individual application" custom subtype ESP-IDF's own docs reserve, not a
filesystem subtype like `spiffs`) because this partition holds one raw
`.kfpack` blob addressed directly with `esp_partition_mmap()`, not a
mounted filesystem — claiming to be one would be a lie the mount code
never backs up. It is found by **name** (`KF_HAL_ASSETS_PARTITION_LABEL`,
`"assets"`) at runtime, so the exact subtype value is not load-bearing.

### Loading: memory-mapped on device, loaded-from-disk on desktop, never copied into PSRAM

`kf/hal/assets.h` is the HAL split — `kf_hal_assets_mount()` /
`_base()` / `_size()` / `_unmount()` — mirroring the existing
`kf/arena.h` (core) / `kf/hal/memory.h` (HAL) split exactly: the HAL's job
is "get me a block of bytes with the right physical properties," core's
job is "parse what's inside them."

- **`ports/esp32/hal/esp_assets.cpp`** finds the `assets` partition by
  label and calls `esp_partition_mmap()`, mapping it into
  `ESP_PARTITION_MMAP_DATA` space (byte/halfword-aligned reads allowed,
  unlike the instruction-space mapping). The returned pointer addresses
  flash directly through the CPU's normal flash cache — the same path
  instruction fetches already use — so reading through it is ordinary
  memory access, safe from any task or ISR that can execute code from
  flash at all, which on this chip is everywhere the blitter runs
  (`kf/blit.cpp` is never called from an ISR, and nothing here disables
  the flash cache the way `spi_flash`'s own erase/write critical sections
  briefly do).
- **`simulator/src/host/host_assets.cpp`** reads the whole pack file into a
  heap-allocated buffer once, at mount time — the desktop's honest
  equivalent of "bytes the program addresses directly and never copies
  again," since desktop has no real flash to map.

Neither backend puts pixel bytes in `KF_ARENA_ASSETS`. A `kf_sprite`'s
`pixels` pointer goes straight into whatever `kf_hal_assets_base()`
returned. This is the requirement that actually drives the design: a 2MB
PSRAM arena cannot hold a 12MB asset budget even once, and copying would
be pure waste when the device can already address the bytes where they
sit. What **does** go through `KF_ARENA_ASSETS` is the small, bounded
directory table `kf_assets_init()` builds — one row per packed entry,
around 80 bytes each — which is exactly the "decoded sprites and game
data" `kf/arena.h`'s own comment already says that arena is for, and is
why its high-water mark is no longer permanently zero.

### The API

```c
kf_result kf_assets_init(void);
const kf_sprite *kf_assets_get(const char *name);
void kf_assets_shutdown(void);
```

in `kf/assets.h` (core, not HAL — same footing as `kf/pet.h`/`kf/blit.h`).
`kf_assets_init()` mounts the pack, checks its size against
`KF_FLASH_ASSET_BUDGET_BYTES`, validates the header/directory, and builds
the table above — panicking (`KF_ASSERT`) on anything wrong, the same "no
degraded mode this early" policy `kf_app_init()` already applies to a
missing display or storage backend. `kf_assets_get()` returns `NULL` for a
name that is absent *or* whose entry exists under a different
`asset_type` — there is deliberately no single "any type" getter, so the
return type a caller gets always matches what they asked for.
`kf_assets_init()`/`_shutdown()` are called from `kf_app_init()`/
`_shutdown()`, bracketing the demo's own init/shutdown the way every other
HAL module already does.

### Replacing the placeholder

`tools/make_test_sprite.py` and `examples/hello_sprite/sprite_data.h` are
deleted. `tools/kf_pack_assets.py --test-sprite` generates the identical
32x32 blob — ported pixel-for-pixel, verified byte-identical before this
change shipped (see Verified) — straight into `.kfpack` form, checked in
at `examples/hello_sprite/assets.kfpack`. `kf/demo.cpp`'s `kf_demo_init()`
now calls `kf_assets_get("test_sprite")` instead of including a generated
header, and reads the sprite's width/height from the returned struct at
runtime rather than from compile-time macros (this needed care around
`-Wsign-conversion`, see the comment in `kf_demo_init()`).

### Superseded in part

the animated-indexed-sprites plan's Task 1
added a third `kf_asset_type`, `ASSET_TYPE_SPRITE_INDEXED` (2), so the claim
above that `ASSET_TYPE_SPRITE` (0) is "the only type anything loads today" is
no longer true — `hakoniwaos/src/assets.cpp` decodes both, and
`kf_assets_get()` returns a `kf_sprite` for either. That plan's Task 3 then
regenerated `examples/hello_sprite/assets.kfpack` itself, so it now holds
`test_sprite` as an 8bpp palette-indexed, colour-keyed blob (32 colours, 1156
bytes) rather than the 2048-byte raw RGB565 blob described above —
losslessly, per that task's own checksum-guarded proof
(`headless_determinism`/`headless_fullscreen`'s golden FNV-1a checksums did
not move). The directory entry shape (52 bytes) and `FORMAT_VERSION` (still
1) are unchanged — exactly the extension path this ADR's own "Cost to
change" section predicted: one new `kf_asset_type` value, one `type_meta`
layout, one decode branch, no format-version bump.

**"Not verified"'s claim that `esp_partition_mmap()` has never returned
readable pixel data on real silicon** is also superseded:
the hardware bring-up plan confirmed it on the
bench, first against the small `hello_sprite` pack (1,156 bytes, the
indexed format above) and only later against the full 556,488-byte
`creature_demo` pack — the board booted, mapped the partition, and rendered
from it in both cases. The other two "Not verified" items (the S3's mmap
address-space ceiling for a much larger mapping, and OTA client code) remain
genuinely unverified.

## Alternatives considered

**A pre-hashed lookup table**, computed by the packer and re-derived by the
loader. Rejected — see "Lookup" above: no benefit at this scale, and a
forever cross-language contract to maintain instead.

**Copying pixel data into `KF_ARENA_ASSETS`** at load time, keeping
`kf_sprite.pixels` inside the tracked PSRAM budget like the demo's own
generated tileset does. Rejected: it cannot work past a couple of
megabytes of real art, and paying the copy cost for data the CPU can
already address in place is waste with no offsetting benefit.

**A single, sprite-only directory entry shape** (what this ADR's first
draft actually shipped, before the coordinator's forward-looking review
flagged the coming audio requirement). Rejected once audio's real shape
was named: it would have meant a second, parallel pack format and a
second, parallel loader for sound effects later — precisely the kind of
forked mechanism this project's HAL rule already refuses to allow. The
`asset_type` tag costs 4 bytes of padding and one `switch`-shaped `if` in
the loader today, in exchange for never having to design this twice.

**One `factory` app partition, no OTA slots.** Cheaper today (saves
~1.5MB), and rejected for the reason given above: repartitioning a shipped
device is expensive to undo, and the option is nearly free to keep open
now while nothing has shipped yet.

## Verified

- Desktop, from the repo root: `cmake -B build-desktop
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DKAMIFRAME_WARNINGS_AS_ERRORS=ON`,
  then `cmake --build build-desktop --target kamiframe-headless --parallel`
  and `cmake --build build-desktop --target kamiframe-sim --parallel` —
  both link clean, zero warnings, under the project's full strict set.
- `ctest --test-dir build-desktop --output-on-failure`: **all 14 tests
  pass** — the original 13, byte-for-byte unchanged (their golden
  checksums were never touched), plus the new `asset_pipeline_check`. The
  13 staying green with no checksum update is itself a proof: it means the
  pack-loaded `test_sprite` renders pixel-identical to the old baked-in
  header, which is what porting the generator pixel-for-pixel was for.
- The new check (`headless_main.cpp`'s `run_asset_check()`,
  `kamiframe-headless --verify-assets`) proves the chain end to end:
  `kf_assets_init()` mounts and parses the real checked-in pack;
  `kf_assets_get("test_sprite")` finds it with the right dimensions and
  color key; a name absent from the pack returns `NULL`; and — the actual
  "matches what the packer wrote" proof the task asked for — the loaded
  sprite's pixel bytes are byte-identical to the pack **file's own bytes**
  at the offset its directory names, read by a small parse independent of
  `hakoniwaos/src/assets.cpp`'s own reader, so the check cannot pass by
  the parser merely agreeing with itself.
- `python3 tools/check_no_heap.py .` reports core heap-free.
- **Device: `idf.py build` for esp32s3 against ESP-IDF v6.0.2, from a
  freshly deleted `sdkconfig` (so the new `partitions.csv` config in
  `sdkconfig.defaults` is what actually generated it, not a stale cached
  value) — succeeded, zero warnings** (grepped the full build log: zero
  matches for "warning"). `esp_partition` was added to `main`'s
  `REQUIRES` for `esp_partition_mmap()`/`esp_partition_find_first()`;
  `esp_assets.cpp` and `hakoniwaos/src/assets.cpp` both compiled clean.
  Flash usage against the new table: `kamiframe-firmware.bin` is
  **0x9f9f0 bytes (653,808 bytes, ~638.5KB)** against the **1.5MB
  (0x180000-byte)** `ota_0` partition — **58.4% free**, real headroom
  over the old table's single 1MB partition (where the same firmware
  would already have used 62% of the whole thing). 653,808 bytes matches
  the ~653KB this task's own brief cited, confirming this build
  environment reflects the real one. `esptool_py_flash_to_partition()`
  wires `examples/hello_sprite/assets.kfpack` into the `assets` partition
  as part of `idf.py flash` — confirmed by inspecting the generated flash
  command, which correctly places it at `0x320000`, the `assets`
  partition's own offset. This was not run against real flash (see Not
  verified).

## Not verified

**No hardware.** Everything above is build-verified only, per this task's
own instruction — there is no board on the desk. Specifically unverified:

- `esp_partition_mmap()` actually mapping the `assets` partition and
  returning readable pixel data on real silicon. The API contract and the
  flash-cache reasoning in "Loading" above are correct as documented, but
  nothing has read a mapped byte on a real ESP32-S3 yet.
- Whether `ESP_PARTITION_MMAP_DATA` can hold a 12MB mapping in one call on
  the S3 without exhausting the mappable address space `esp_partition.h`'s
  own comment describes (it names a 4MB figure for the *original* ESP32;
  the S3's MMU is different and larger, but this project has not measured
  it).
- `esptool_py_flash_to_partition()` actually writing `assets.kfpack` to
  the right flash offset on a real `idf.py flash` — the CMake wiring is in
  place and the partition table's offsets are correct by construction
  (verified with ESP-IDF's own `gen_esp32part.py --verify`), but no board
  has received that write.
- The `ota_0`/`ota_1` layout's actual usefulness: no OTA client code
  exists, so "the option stays open" is a claim about the partition table,
  not a working update mechanism.
- `KF_ASSET_TYPE_AUDIO_CLIP`: reserved and documented, never exercised.
  There is no packer support, no loader, and no `kf_assets_get_clip()` —
  intentionally, per this task's scope. A future audio slice would add a
  decoder branch to `hakoniwaos/src/assets.cpp` (mirroring the sprite
  branch already there), a `kf_assets_get_clip()` returning a small
  `kf_audio_clip`-shaped view (data pointer + sample_rate/channels/
  bits_per_sample decoded from `type_meta`), and `--wav`/similar packer
  support in `tools/kf_pack_assets.py` — all additions to the files this
  ADR already built, not a new pipeline.

## Cost to change

Adding an asset type: one new `kf_asset_type` value, one `type_meta`
layout documented in the packer's docstring, one decode branch in
`kf_assets_init()`, one `kf_assets_get_<type>()` accessor. The directory
walk, the partition, the HAL split and the lookup mechanism are all
already type-agnostic, so this is additive.

Growing the asset budget past 12MB: one number in `kf/budget.h`, one
number in `partitions.csv` (they must be changed together — nothing
enforces that mechanically beyond the comment on each), and less spare
tail space on the chip (currently ~896KB unallocated).

Adding real OTA: the partition table already has the slots; this needs an
actual client (`esp_https_ota` or equivalent), which is unstarted,
separate work this ADR deliberately did not begin.

Reversing the "never copy into PSRAM" design would mean revisiting every
`kf_sprite` consumer's assumption that `pixels` stays valid for the
program's lifetime without a refcount or arena reset — cheap while nothing
depends on it yet, expensive once game code exists that assumes it.
