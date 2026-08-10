# Animated, Indexed Sprites — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Sprites stop being stored raw. Every sprite in a pack becomes 8bpp
palette-indexed — half the bytes, losslessly — and a pack entry stops being one
still and becomes a whole animation, so a pose can play nine frames without a
single extra directory lookup.

**Architecture:** One migration, not two, because they touch the same three
files. A new `kf_asset_type` (`KF_ASSET_TYPE_SPRITE_INDEXED`) carries both
changes at once: `type_meta` gains a frame count, and the payload gains a
palette in front of the index bytes. Frames live contiguously in that one
payload, so frame *k* is `base + k * width * height` — O(1), no per-frame name.
The existing RGB565 type is not removed; the reader and the blitter both handle
both, which is what lets the branch stay green through the whole sequence.
Playback is a millisecond accumulator on `kf_creature` in `hakoniwaos/`,
ticking at its own ~10fps regardless of what the display is doing.

**Tech Stack:** C++17, CMake, `tools/kf_pack_assets.py` / `kf_ingest_sprites.py`
(stdlib Python, no Pillow), CTest via `kamiframe-headless --verify-*` check
modes.

## Status: all six tasks COMPLETE, 2026-08-10

| Task | Commits | Measured result |
|---|---|---|
| 1 — format and dual-format reader | `e06b643`, fixes `4cf367d` `23600b1` `6591a7e` `5d10489` | reader parses both formats |
| entry cap raise (unplanned) | `088ced1` | `KF_ASSETS_MAX_ENTRIES` 64 → 512 |
| 2 — blitter | `8f2faa7` | indexed blit pixel-identical to RGB565 |
| 3 — convert default pack | `918d487` | 2,116 → **1,156 B**, golden checksums unmoved |
| 4 — ingest packs animations | `c24e0c5`, `4fc5d71` | creature pack 438,056 → **224,496 B** |
| 5 — drop the `_01` suffix | `2bd9a58` | longest entry name 26 chars of 31 |
| 6 — playback clock | `2f9dcb4`, `c688769` | 37/37, dirty-rect assertions unchanged |

**The losslessness bet paid off.** Both golden rendering checksums stayed
byte-identical through the whole migration, which is the strongest available
proof that 8bpp costs nothing in quality on this art.

**Anyone picking this up should know:** the format works and plays frames, but
at the time of writing every *shipped* sprite is still single-frame, so the
playback path is a no-op against the art in the repo. Animated art is a
separate generation spend, not a code gap.

## Decisions taken DURING execution — not in the original plan

Recorded here because the plan is what the next implementer reads. Do not
re-litigate these.

**KFDBG gating: split observe from mutate** (Chris, 2026-08-10). The old
boundary was accidental — `KF_DBG_INPUT_INJECT_ENABLE` gated only button
injection while `ADVANCE`/`RESET`/`MULT`/care/jump ran ungated, so switching off
injection gave false assurance that a serial cable could not cheat the pet.
Now: `KF_DBG_BRIDGE_ENABLE` gates read-only commands (`PING`, `SHOT`, `STATE`,
`SCANLINE`, `VSYNC`, `WATCH`); a mutate flag gates everything that changes the
pet or simulation (`FEED`, `PLAY`, `REST`, `BATH`, `FLUSH`, `JUMP`, `ADVANCE`,
`RESET`, `MULT`, `BTN`). Dev builds enable both; release is observe-only or off.

**Stop generating sleeping poses.** `KF_CREATURE_POSE_SLEEPING` is unreachable —
Core has no sleep field, so nothing can select it. Roughly 140 generations were
already spent on sleeping art the game cannot display. Do not generate more
until the sleep plan lands.

**Adults are deferred and split across two billing periods** (Chris,
2026-08-10). 19 forms at ~82 generations each is ~1,558, plus ~632 to animate
the roster, against ~1,127 remaining. Rather than scope down or top up, the
work splits across two monthly allowances at no extra cost.

**Animation scope: idle first.** One `animate_object` v3 call covering the three
directions the game uses costs ~5 generations and returns 8-16 frames, so
animating the idle pose of every creature that has art is ~35 generations.
That is the cheapest way to make the whole game feel alive.

**Measured PixelLab costs** (clean, one call at a time, balance read either
side — see `.superpowers/sdd/pixellab-cost-experiment*.md`):

| Tool | Cost |
|---|---|
| `create_8_direction_object` | 20 |
| `create_object_state` | 20 |
| `create_character` v3 (base) | 2 |
| `create_character_state` | 20 |
| `animate_object` v3, 3 directions | ~5 |

Two cost-reduction hypotheses were tested and **both failed**: pose variants on
the cheap `create_character` path still cost 20, and animation frames cannot
serve as standalone poses (they read as transitional, and a back view has no
face to emote with). Do not re-test these without new information.

**Do not measure generation cost with two agents running.** They share one
global counter and each will attribute the other's spend to itself. This
produced a figure that was wrong by 14x before it was caught.

## READ THIS BEFORE DISPATCHING ANY TASK FROM THIS PLAN

**This document's code listings are copied verbatim by implementers, and it has
manufactured five defects that way.** Three were comments contradicting their
own code; one was a real `ValueError` — `make_indexed_asset(..., has_color_key=True)`
with no `color_key=` — that Task 4's implementer hit at runtime; one was
`kf_arena_init_all()` called four times against an assert that fires on the
second call, which cost two implementers time because Task 1 fixed it in the
source and nobody fixed it here.

Two rules follow, and the second matters more:

1. When a review finds a bad comment or pattern, grep **this file** as well as
   the source tree. A defect here costs one defect *per remaining task*.
2. **Update this plan when a decision is made, before dispatching the task it
   affects** — not afterwards, and not by appending amendments to a generated
   brief. Briefs are generated *from* this file, so a stale line here is
   re-served to every implementer that follows.

## The two decisions already taken, and why

**8bpp indexed, not 4bpp.** Measured against the real art in
`examples/creature_demo/assets.kfpack` (49 sprites): each sprite individually
uses **6 to 27 distinct RGB565 colours**, and the union across all 49 is **201**,
transparency included. So 8 bits per pixel is *lossless* — every colour that
exists gets its own index, and the converted pack renders pixel-identical
output. 4bpp would halve it again but needs the 27-colour sprites quantised
down to 16, which is a real, visible quality cost paid to solve a problem
that does not exist:

| Format | Bytes/frame @48x48 | Full roster (379 poses x 9 frames) | Verdict |
|---|---|---|---|
| Raw RGB565 (today) | 4,608 | ~15.7 MB | over the 12 MB budget |
| **8bpp indexed** | **2,304** | **~7.5 MB** | fits, ~4.5 MB spare |
| 4bpp indexed | 1,152 | ~3.8 MB | fits, and lossy |

`KF_FLASH_ASSET_BUDGET_BYTES` is 12 MB (`hakoniwaos/include/kf/budget.h`, and
it must equal `ports/esp32/partitions.csv`'s `assets` partition). 7.5 MB leaves
~4.5 MB for audio, fonts and Lua. 4bpp stays available later — the format
below can add it as one more `kf_asset_type` with no version bump — but it is
not worth degrading the art to buy space nothing is asking for.

**Per-entry palettes, not one global palette.** 201 colours is the union of
*egg, baby, child, teen 0 and the shrine only*. Ten adult families will very
likely push past 256, and a global palette is a trap that springs late — after
the format is frozen and the art is generated. So the palette travels with the
sprite. In fact this plan goes one step finer than "per entity": the palette is
computed **per pack entry** (one entity, one pose, one direction, all its
frames), which falls straight out of storing it inline in the payload. That is
6-27 colours per palette instead of 201, it removes the 256-colour ceiling as
a concern entirely, and the duplication costs ~64 bytes per entry — about 24 KB
across the full roster, against 7.5 MB of pixels.

## Global Constraints

- **Maximum 8 dirty rectangles per frame** (`KF_MAX_DIRTY_RECTS`,
  `hakoniwaos/include/kf/framebuffer.h:44`). Past 8 the framebuffer collapses to
  one screen-sized bounding box and re-transfers ~31ms of pixels against a
  33.3ms budget. The creature costs **1 rect** in a still frame and **2** while
  walking (erase + draw, which merge when they overlap). **This plan must not
  change either number** — see "The per-frame cost, stated up front" below.
- **`hakoniwaos/` must stay heap-free.** `python3 tools/check_no_heap.py` runs
  in `dev.sh test` and fails the build. No `malloc`/`new` in any Core file. The
  palette is read in place from the mounted pack; nothing is allocated for it.
- **`hakoniwaos/` must stay free of floating point.** The playback clock is an
  integer millisecond accumulator. No `float`, no `double`, no `sin`.
- **240x320 RGB565, no alpha anywhere.** Transparency is colour-key only,
  magenta `KF_RGB(255,0,255)` by the convention in
  `tools/kf_ingest_sprites.py:53`.
- **The desktop build is the real firmware against a desktop HAL backend.**
  Everything in `hakoniwaos/` runs identically on the ESP32. There is no
  emulator, and two codebases that mimic each other is a failure state.
- **`tools/kf_pack_assets.py` discards the alpha channel;
  `tools/kf_ingest_sprites.py` is the correct entry point** and resolves
  transparent pixels to the colour key. Keep that division: the packer owns the
  byte layout and knows nothing about alpha; the ingest tool owns alpha
  resolution and reuses the packer's `pack()` rather than reimplementing it.
- **The pack format is versioned** (`FORMAT_VERSION = 1`, header byte 4) and
  documented in exactly two places that must agree:
  `tools/kf_pack_assets.py`'s header comment (canonical) and
  `hakoniwaos/src/assets.cpp`'s parser. This plan does **not** bump the version
  — see Task 1's design note.
- **Existing tests: 33/33 passing.** Build with `cmake --build build -j8`.
  **Never** `cmake -B build`. Run everything with `bash dev.sh test`.
- **Pointing `KF_ASSET_PACK` at a non-default pack makes `headless_determinism`,
  `headless_fullscreen` and `asset_pipeline_check` fail legitimately.** Use the
  runtime override `kf_host_assets_set_pack_path()` inside a check and restore
  it before returning, the way `run_creature_screen_sprite_check()` already
  does.

## Losslessness is the correctness test

Because 8bpp is lossless for this art, the conversion must produce **rendered
output that has not changed by one byte**. That is a far stronger check than
any assertion written by hand, and it is proved three times over, at increasing
scope:

1. **Task 2** draws the same test sprite twice — once as RGB565, once as
   indexed — into the framebuffer and asserts the two are `memcmp`-identical.
   Direct, targeted, and it lands before any real art moves.
2. **Task 3** converts the *default* pack, so `headless_determinism` and
   `headless_fullscreen` — golden FNV-1a checksums of 300 rendered frames —
   must pass with their **checksum constants untouched**. From Task 3 onward
   every later task is under that guard.
3. **Task 4** gives the ingest tool a `--verify-lossless` mode that expands
   every indexed payload back to RGB565 and compares it to the RGB565 it would
   otherwise have written, for all 49 sprites, exactly.

**If a checksum moves, the conversion is lossy and something is wrong.** Do not
update a golden constant to make a test pass; that is what `kf/budget.h`'s own
banner forbids for budget numbers and the same rule applies here.

**One honest exception.** `asset_pipeline_check` cannot pass unchanged and must
not be expected to. Read `run_asset_check()` in
`simulator/src/headless/headless_main.cpp:4221` — it asserts `asset_type == 0`,
`data_bytes == width*height*2`, and byte-compares the loaded `pixels` against
the file's own bytes, deliberately without reusing the C++ parser, "so this is
a real 'matches what the packer wrote' proof rather than the parser grading
itself." It is a *format-layout* check whose entire job is to notice format
drift. Task 3 rewrites it for the indexed layout and keeps that property. The
two golden **rendering** checksums are what stay frozen.

## The per-frame cost, stated up front

Numbers from `hakoniwaos/include/kf/budget.h`: `KF_DRAW_OPAQUE_PX_PER_US` 100,
`KF_DRAW_KEYED_PX_PER_US` 25, `KF_FRAME_BUDGET_US` 33,333,
`KF_DISPLAY_SPI_HZ` 40 MHz.

| | Today | After this plan |
|---|---|---|
| Dirty rects, creature standing still | 1 | 1 |
| Dirty rects, creature walking | 2 | 2 |
| Creature draw | 2,304 **keyed** px = 92µs | 2,304 **keyed** px = 92µs |
| Creature erase | 2,304 opaque px = 23µs | 2,304 opaque px = 23µs |
| Worst-frame dirty bytes | ≤ 13,824 | ≤ 13,824 |
| Wire time for a 48x48 rect | 0.92ms | 0.92ms |

**Animation adds nothing to any of these**, because
`kf_creature_screen_frame()` already erases `g_previous` and redraws the sprite
*unconditionally, every frame*, whether or not the creature moved
(`simulator/src/pet/kf_creature_screen.cpp:669`). Advancing to a different
frame changes which bytes are read, not how many pixels are written or how many
rectangles are marked.

**Indexing does not move the creature into a worse cost bucket either**, because
the creature sprite is already colour-keyed and already charged to
`keyed_pixels`. An indexed blit has no `memcpy` fast path — every pixel is a
byte load, a key compare and a palette table load — so it is charged to
`keyed_pixels` too, by the same "bucket reflects cost *shape*" rule
`kf_draw_count_pixels()` already documents. The only case that genuinely gets
slower is an *un-keyed, opaque* sprite, which today gets a whole-row `memcpy` at
100 px/µs and indexed would get 25 px/µs. There is exactly one such sprite in
the repo (the demo's procedural tileset, which is not packed at all and is not
touched by this plan), and that 4:1 gap is the reason **the RGB565 type is kept
rather than removed** — a full-screen opaque background is a real, cheap cost
shape and deleting it would be a regression dressed as tidiness.

**The one number this plan is guessing at:** `KF_DRAW_KEYED_PX_PER_US = 25` is
flagged in `budget.h` as "ASSUMPTION, NOT MEASURED". It assumes a per-pixel test
and store; an indexed blit adds a table load per pixel on top of that. The
palette is at most 512 bytes and is read repeatedly, so it should sit in cache,
but this is not measured and cannot be until there is a board. Task 2 adds a
comment saying so next to the constant rather than inventing a second constant
nobody can calibrate. Even at half the assumed rate a full-screen indexed blit
is 6ms of a 33ms frame, and the creature is 0.18ms, so nothing in this plan is
close to the edge.

## The risk this plan sits on top of

ADR 0033's own "Not verified" section says `esp_partition_mmap()` returning
readable pixel data on real silicon **has never been confirmed**, and that
whether `ESP_PARTITION_MMAP_DATA` can hold a 12 MB mapping in one call on the S3
is unmeasured (`esp_partition.h` names a 4MB figure for the *original* ESP32).

This plan sits directly on that path and makes the mapping *larger* in
practice: today's real pack is 228 KB; a full nine-frame roster is ~7.5 MB.
What this plan assumes, stated so it can be checked rather than discovered:

- The whole pack is addressable through one mapped base pointer, and a
  `const uint8_t *` into the middle of it is a valid read at any offset.
- Reading a palette entry and an index byte from that pointer is an ordinary
  memory access, the same assumption the existing RGB565 `pixels` pointer
  already makes — indexing changes *how many* distinct addresses a blit touches
  per pixel (two: index, then palette) but not the nature of the access.
- Nothing in this plan copies pixel bytes into `KF_ARENA_ASSETS`. That arena is
  2 MB and could not hold the pack; the palette is read in place too.

If the mmap turns out to be capped below the pack size at bring-up, this plan
is not what breaks — but it is what makes the failure loud, because the pack
gets much bigger. The mitigation, if it comes to that, is per-entity packs
mounted one at a time, which the directory format already permits and which
this plan does not pre-build.

---

### Task 1: The format, and a reader that understands both

The pack learns a second sprite type. Nothing draws it yet and no art moves —
this task ends with a loader that can decode an indexed animation and a fixture
pack to prove it on.

**Files:**
- Modify: `hakoniwaos/include/kf/types.h` (`kf_sprite_format`, `kf_sprite` grows)
- Modify: `hakoniwaos/include/kf/assets.h` (`KF_ASSET_TYPE_SPRITE_INDEXED`)
- Modify: `hakoniwaos/src/assets.cpp` (second decode branch)
- Modify: `tools/kf_pack_assets.py` (format comment, `_indexed_sprite_type_meta()`, `--test-sprite --indexed [--frames N]`)
- Create: `examples/hello_sprite/assets_indexed.kfpack` (checked in, 4,344 bytes, the permanent test fixture)
- Test: `simulator/src/headless/headless_main.cpp` (new `run_indexed_asset_check()`)
- Modify: `simulator/CMakeLists.txt` (register `indexed_asset_check`, define `KF_INDEXED_FIXTURE_PACK_PATH`)

**Interfaces:**
- Consumes: the existing type-agnostic directory walk in
  `kf_assets_init()`, and `kf_host_assets_set_pack_path()` from
  `simulator/src/host/host_assets.h`.
- Produces: `KF_ASSET_TYPE_SPRITE_INDEXED = 2`; `kf_sprite_format`;
  `kf_sprite` with `indices`, `palette`, `palette_count`, `frame_count`,
  `format`; `KF_SPRITE_KEY_INDEX`. Tasks 2-6 all depend on these exact names.

**Design note — the trap in this task is the version bump you must not make.**
The obvious move is to grow the 52-byte directory entry and bump
`FORMAT_VERSION` to 2. Do not. `type_meta` is 8 bytes whose meaning depends
entirely on `asset_type`, and ADR 0033 says in its own "Cost to change" section
that adding a type is "one new `kf_asset_type` value, one `type_meta` layout,
one decode branch, one accessor" — that is exactly this change. A version bump
would make every existing pack unreadable and would force the reader to carry
two directory layouts instead of two payload layouts. Using a new type instead
means the reader handles old and new **from the first commit**, which is the
only reason the branch can stay green while the art migrates one pack at a time.

The second trap: an index byte that exceeds `palette_count` reads past the
palette into whatever payload follows. The reader cannot catch that without
scanning every index byte at startup, which is a 7.5 MB scan for a problem the
producer can rule out for free. So the *producer* guarantees it (Task 4's
packer asserts `max(index) < palette_count`), the *independent verifier*
re-checks it from the file's own bytes, and the reader validates only the
arithmetic — exactly the posture the existing reader already takes toward a
corrupt `width`/`height`.

**Dirty-rect cost: 0.** Nothing draws in this task.

- [ ] **Step 1: Write the failing test**

Add to `simulator/src/headless/headless_main.cpp`, near the other
`run_*_check()` functions:

```cpp
/* Proves the indexed sprite type decodes: the palette, the frame count, the
 * index data, and -- the point of the whole format -- that expanding index
 * bytes through the palette reproduces exactly the RGB565 the RGB565
 * test_sprite already carries. Mounts examples/hello_sprite/assets_indexed
 * .kfpack through the runtime override (host_assets.h) and restores the
 * default before returning, so nothing later in this process inherits it and
 * headless_determinism/headless_fullscreen/asset_pipeline_check keep seeing
 * the pack they were checksummed against. */
static int run_indexed_asset_check(void) {
    bool ok = true;
    auto check = [&ok](bool cond, const char *what) {
        if (!cond) { KF_LOGE(TAG, "FAILED: %s", what); ok = false; }
    };

    kf_arena_init_all();

    /* The RGB565 original, from the default pack, read first. */
    check(kf_assets_init() == KF_OK, "default pack mounts");
    const kf_sprite *rgb = kf_assets_get("test_sprite");
    check(rgb != nullptr && rgb->format == KF_SPRITE_FORMAT_RGB565,
          "the default pack's test_sprite is still RGB565");
    std::vector<kf_color> expected;
    if (rgb != nullptr) {
        expected.assign(rgb->pixels,
                        rgb->pixels + static_cast<size_t>(rgb->width) * rgb->height);
    }
    kf_assets_shutdown();

    /* The indexed fixture. kf_arena_init_all() is NOT called again here --
     * it panics on a second call (kf/arena.cpp: `KF_ASSERT(!g_initialised,
     * "kf_arena_init_all called twice")`), and KF_ARENA_ASSETS is not one of
     * the resettable arenas (only KF_ARENA_SCRATCH is). Mounting a second
     * pack in the same process just grows the same arena's directory-table
     * allocation a little further along; kf_assets_shutdown() above already
     * dropped the first mount's g_up flag so kf_assets_init() below is
     * legal. Found the hard way while implementing Task 1 -- see
     * simulator/src/headless/headless_main.cpp's run_indexed_asset_check()
     * for where this landed. */
    kf_host_assets_set_pack_path(KF_INDEXED_FIXTURE_PACK_PATH);
    check(kf_assets_init() == KF_OK, "the indexed fixture pack mounts");

    const kf_sprite *ix = kf_assets_get("test_sprite");
    check(ix != nullptr, "kf_assets_get finds the indexed test_sprite");
    if (ix != nullptr) {
        check(ix->format == KF_SPRITE_FORMAT_INDEXED8,
              "it reports KF_SPRITE_FORMAT_INDEXED8");
        check(ix->width == 32u && ix->height == 32u, "it is 32x32");
        check(ix->frame_count == 1u, "it has one frame");
        check(ix->pixels == nullptr, "an indexed sprite has no RGB565 pixels");
        check(ix->indices != nullptr && ix->palette != nullptr,
              "it has both index data and a palette");
        check(ix->palette_count == 32u,
              "the measured palette for this blob is 32 colours");
        check(ix->has_color_key && ix->color_key == ix->palette[0],
              "the colour key is palette entry 0");

        bool identical = (expected.size() ==
                          static_cast<size_t>(ix->width) * ix->height);
        for (size_t i = 0; identical && i < expected.size(); ++i) {
            if (ix->palette[ix->indices[i]] != expected[i]) { identical = false; }
        }
        check(identical,
              "expanding every index through the palette reproduces the "
              "RGB565 sprite byte for byte -- 8bpp is lossless for this art");
    }

    const kf_sprite *anim = kf_assets_get("test_sprite_anim");
    check(anim != nullptr, "kf_assets_get finds the 3-frame animation");
    if (anim != nullptr) {
        check(anim->frame_count == 3u, "it reports three frames");
        const size_t stride = static_cast<size_t>(anim->width) * anim->height;
        check(std::memcmp(anim->indices, anim->indices + stride, stride) != 0,
              "frame 1 differs from frame 0 -- the fixture is really animated, "
              "not three copies of the same picture");
    }

    kf_assets_shutdown();
    kf_host_assets_set_pack_path(nullptr);

    if (!ok) { return 1; }
    KF_LOGI(TAG, "indexed-assets: format decodes and is lossless");
    return 0;
}
```

Wire the flag in alongside the other `--verify-*` handlers:

```cpp
} else if (std::strcmp(argv[i], "--verify-indexed-assets") == 0) {
    return run_indexed_asset_check();
}
```

- [ ] **Step 2: Run it to make sure it fails**

Run: `cmake --build build -j8`
Expected: FAIL to compile — `'KF_SPRITE_FORMAT_RGB565' was not declared`,
`'struct kf_sprite' has no member named 'format'`. That is the correct failure.

- [ ] **Step 3: Grow `kf_sprite`**

In `hakoniwaos/include/kf/types.h`, replace the `kf_sprite` block:

```c
/* How a sprite's pixels are stored. A property of the DATA, not a request
 * from the caller -- which is why it lives on the sprite rather than being
 * a flag passed to kf_blit(). Note for Task 1: kf/blit.h does not read this
 * field yet at that point, and asserts sprite->pixels != nullptr, so an
 * indexed sprite panics if blit'd before Task 2 lands. (Task 2 closes this
 * gap -- see its own Step 3 below. By the time both tasks have landed, the
 * checked-in header no longer carries this "not yet" wording; it reads
 * "since Task 2, kf/blit.h ... branch[es] on this field" instead.) */
typedef enum {
    KF_SPRITE_FORMAT_RGB565 = 0,  /* `pixels` is valid */
    KF_SPRITE_FORMAT_INDEXED8 = 1 /* `indices` + `palette` are valid */
} kf_sprite_format;

/* The palette slot a colour-keyed indexed sprite reserves for "do not draw
 * this pixel". Fixed at 0 by convention rather than stored per sprite: it
 * costs nothing, it makes the blitter's key test a compare against a
 * compile-time constant, and the packer is what guarantees it. As of Task 1
 * that means tools/kf_pack_assets.py's quantize_rgb565(), which always
 * seeds the palette with the key colour at slot 0, plus
 * make_indexed_asset()'s own check that palette[0] matches the declared
 * key. tools/kf_ingest_sprites.py routes through both when it builds a
 * pack entry (its build_entry()), so a real creature pack gets the same
 * guarantee. */
#define KF_SPRITE_KEY_INDEX 0u

/* An immutable sprite, possibly with more than one frame. Pixels live in
 * flash or a mounted asset pack and are never copied.
 *
 * FRAMES ARE CONTIGUOUS. For an indexed sprite, frame k starts at
 * `indices + k * width * height` -- one directory entry per animation, O(1)
 * frame addressing, no per-frame name lookup. RGB565 sprites are always
 * frame_count == 1; multi-frame RGB565 was never packed and is not worth
 * adding when everything is migrating to indexed anyway. */
typedef struct {
    const kf_color *pixels;  /* RGB565 data; NULL when format is INDEXED8 */
    const uint8_t *indices;  /* 8bpp palette indices; NULL when RGB565 */
    const kf_color *palette; /* palette_count entries; NULL when RGB565 */
    uint16_t width;
    uint16_t height;
    uint16_t frame_count;   /* always >= 1 */
    uint16_t palette_count; /* 0 when RGB565 */
    kf_color color_key;     /* for INDEXED8 this equals palette[0] */
    bool has_color_key;     /* pixels equal to color_key are not drawn */
    uint8_t format;         /* a kf_sprite_format value */
} kf_sprite;
```

Nothing that constructs a `kf_sprite` needs changing: every site uses value
initialisation (`kf_sprite{}` in `hakoniwaos/src/assets.cpp:178`,
`hakoniwaos/src/demo.cpp:46`, `simulator/src/headless/headless_main.cpp:4382`),
which zero-fills the new fields, and `0` is `KF_SPRITE_FORMAT_RGB565`. Set
`frame_count = 1` explicitly at those three sites so a zeroed frame count never
reaches the blitter.

- [ ] **Step 4: Declare the new asset type**

In `hakoniwaos/include/kf/assets.h`, inside `kf_asset_type`, after
`KF_ASSET_TYPE_AUDIO_CLIP`:

```c
    /* 8bpp palette-indexed pixels, one to many frames, stored contiguously
     * in one payload. Half the bytes of KF_ASSET_TYPE_SPRITE and -- for
     * every sprite this project has measured, 6 to 27 colours each --
     * exactly lossless. KF_ASSET_TYPE_SPRITE is NOT deprecated by this:
     * a large opaque sprite still blits four times faster as raw RGB565,
     * because an un-keyed RGB565 row is a memcpy and an indexed row can
     * never be one. Both are returned by kf_assets_get() as a kf_sprite;
     * the caller reads ::format if it cares. As of Task 1, kf/blit.h does
     * NOT handle indexed sprites -- it asserts sprite->pixels != nullptr,
     * which an indexed sprite deliberately leaves null. Drawing one panics
     * until Task 2 teaches the blitter this format. (Task 2 closes this
     * gap; the checked-in header's comment is updated at that point to say
     * kf/blit.h branches on ::format instead of asserting against it --
     * see Task 2's own Step 3.) See tools/kf_pack_assets.py's format
     * comment for the type_meta layout and the payload's palette-then-
     * frames shape. */
    KF_ASSET_TYPE_SPRITE_INDEXED = 2
```

- [ ] **Step 5: Document the format in the packer, and produce it**

In `tools/kf_pack_assets.py`, add to the "Asset types" section of the module
docstring, after the `ASSET_TYPE_AUDIO_CLIP` paragraph:

```
    ASSET_TYPE_SPRITE_INDEXED (2): 8bpp palette-indexed pixels, one or
        more frames.
        type_meta: width (u16 @0), height (u16 @2), frame_count (u16 @4),
        palette_count_minus_1 (u8 @6 -- the palette holds this + 1 entries,
        so 1..256 are expressible in one byte), flags (u8 @7; bit 0 =
        has_color_key, bits 1-7 reserved and 0).

        Payload, in this order:
          - palette: (palette_count_minus_1 + 1) * 2 bytes, RGB565
            little-endian, zero-padded up to a multiple of 4 so the index
            data that follows starts on a 4-byte boundary the same way
            data_offset itself does;
          - indices: frame_count * width * height bytes, one byte per
            pixel, row-major, frame 0 first. FRAME k STARTS AT
            palette_bytes_padded + k * width * height -- contiguous on
            purpose, so a player addresses a frame with a multiply instead
            of a directory lookup.
        data_bytes therefore equals palette_bytes_padded +
        frame_count * width * height, and is checked against exactly that.

        When flags bit 0 is set, PALETTE ENTRY 0 IS THE COLOUR KEY and
        index 0 is never drawn. Fixed by convention rather than stored,
        so the blitter compares against a compile-time constant; the
        producer is what guarantees it -- quantize_rgb565() (below), which
        always seeds the palette with the key colour first, plus
        make_indexed_asset()'s own check that palette[0] matches the
        declared color_key. tools/kf_ingest_sprites.py routes through both
        when it builds a pack entry (its build_entry()), so a real creature
        pack gets the same guarantee.

        NO FORMAT VERSION BUMP. This is a new asset_type in the existing
        52-byte directory entry, which is precisely the extension path
        adr-0033-asset-pipeline.md's "Cost to change" section describes.
        A reader that does not know this type still walks the directory
        correctly and simply builds no view for the entry.
```

Add the constant and the helpers:

```python
ASSET_TYPE_SPRITE_INDEXED = 2


def _indexed_sprite_type_meta(width: int, height: int, frame_count: int,
                               palette_count: int, has_color_key: bool) -> bytes:
    """Packs ASSET_TYPE_SPRITE_INDEXED's 8-byte type_meta block. See the
    module docstring's "Asset types" section for the field layout."""
    if not 1 <= palette_count <= 256:
        raise ValueError(f"palette_count {palette_count} outside 1..256")
    if not 1 <= frame_count <= 0xFFFF:
        raise ValueError(f"frame_count {frame_count} outside 1..65535")
    meta = struct.pack("<HHHBB", width, height, frame_count,
                        palette_count - 1, 1 if has_color_key else 0)
    assert len(meta) == TYPE_META_BYTES
    return meta


def make_indexed_asset(name, width, height, frames, palette, has_color_key):
    """`frames` is a list of bytes objects, each width*height index bytes,
    frame 0 first. `palette` is a list of RGB565 ints, entry 0 being the
    colour key when has_color_key. Returns one packable entry dict, the same
    shape make_test_sprite() returns."""
    for i, f in enumerate(frames):
        if len(f) != width * height:
            raise ValueError(f"'{name}' frame {i}: {len(f)} bytes, expected "
                              f"{width * height}")
        hi = max(f) if f else 0
        if hi >= len(palette):
            raise ValueError(
                f"'{name}' frame {i}: index {hi} is past the end of a "
                f"{len(palette)}-entry palette -- the packer will not write a "
                "sprite whose own indices read off the end of its palette")
    pal = b"".join(struct.pack("<H", c) for c in palette)
    pal += b"\x00" * ((-len(pal)) % ALIGN)
    return {
        "name": name,
        "asset_type": ASSET_TYPE_SPRITE_INDEXED,
        "type_meta": _indexed_sprite_type_meta(width, height, len(frames),
                                                len(palette), has_color_key),
        "data": pal + b"".join(frames),
    }


def quantize_rgb565(frames_565, key565):
    """Turns a list of RGB565 pixel lists into (palette, index_frames).
    The colour key, if present anywhere, is forced to palette slot 0 --
    KF_SPRITE_KEY_INDEX in hakoniwaos/include/kf/types.h. Every other colour
    follows in first-seen order, which is stable for a given input and so
    keeps a regenerated pack byte-identical. Raises if the union exceeds 256:
    that is the point at which 8bpp stops being lossless, and silently
    quantising would be exactly the quality loss this format was chosen to
    avoid."""
    palette = [key565]
    index_of = {key565: 0}
    out = []
    for pixels in frames_565:
        buf = bytearray(len(pixels))
        for i, c in enumerate(pixels):
            slot = index_of.get(c)
            if slot is None:
                if len(palette) == 256:
                    raise ValueError(
                        "more than 256 distinct colours -- 8bpp indexing is "
                        "lossless only up to 256; split this entry or move to "
                        "a wider format deliberately")
                slot = len(palette)
                index_of[c] = slot
                palette.append(c)
            buf[i] = slot
        out.append(bytes(buf))
    return palette, out
```

Extend `_describe()` with a branch for the new type (it currently falls through
to a bare byte count), and add the CLI flags:

```python
    p.add_argument("--indexed", action="store_true",
                    help="emit --test-sprite as ASSET_TYPE_SPRITE_INDEXED "
                         "instead of raw RGB565")
    p.add_argument("--frames", type=int, default=1,
                    help="with --indexed, also emit 'test_sprite_anim' with "
                         "this many frames (the blob shifted one pixel right "
                         "per frame), as an animation fixture")
```

with `--indexed` routing `make_test_sprite()`'s pixel list through
`quantize_rgb565()` and `make_indexed_asset()`.

- [ ] **Step 6: Generate the fixture pack and check it in**

Run:
```bash
python3 tools/kf_pack_assets.py --test-sprite --indexed --frames 3 \
    -o examples/hello_sprite/assets_indexed.kfpack
```
Expected on stderr: two entries, `test_sprite: indexed sprite 32x32 1 frame(s)
32-colour palette key=palette[0]` and `test_sprite_anim: indexed sprite 32x32
3 frame(s) 32-colour palette key=palette[0]`, 4,344 bytes total (120 bytes of
header and two 52-byte directory entries, 1,088 bytes for `test_sprite`'s
64-byte palette plus 1,024 index bytes, 3,136 for `test_sprite_anim`'s same
64-byte palette plus 3,072 index bytes across three frames -- reusing the
identical palette across frames, since the animation fixture is the same
pixels rotated, not new colours).

- [ ] **Step 7: Decode it in the reader**

In `hakoniwaos/src/assets.cpp`, after the existing
`if (row.type == KF_ASSET_TYPE_SPRITE) { ... }` block, add the second branch —
note it is *another* branch on the same already-validated row, not a second
directory walk:

```cpp
        } else if (row.type == KF_ASSET_TYPE_SPRITE_INDEXED) {
            const uint16_t width = read_u16(row.type_meta + 0);
            const uint16_t height = read_u16(row.type_meta + 2);
            const uint16_t frame_count = read_u16(row.type_meta + 4);
            const uint16_t palette_count =
                static_cast<uint16_t>(row.type_meta[6]) + 1u;
            const bool has_color_key = (row.type_meta[7] & 0x01u) != 0u;

            KF_ASSERT(frame_count >= 1u,
                      "indexed sprite '%s': frame_count is 0 -- every sprite "
                      "has at least one frame",
                      row.name);

            /* Palette first, zero-padded to 4 so the index data behind it
             * starts aligned -- see tools/kf_pack_assets.py's own payload
             * description. */
            const uint32_t palette_bytes =
                static_cast<uint32_t>(palette_count) * 2u;
            const uint32_t palette_padded = (palette_bytes + 3u) & ~3u;
            const uint64_t index_bytes = static_cast<uint64_t>(frame_count) *
                                          static_cast<uint64_t>(width) *
                                          static_cast<uint64_t>(height);
            const uint64_t expected_bytes = palette_padded + index_bytes;
            KF_ASSERT(data_bytes == expected_bytes,
                      "indexed sprite '%s': data_bytes (%" PRIu32
                      ") does not match palette + frames*w*h (%llu) -- "
                      "corrupt pack",
                      row.name, data_bytes,
                      static_cast<unsigned long long>(expected_bytes));

            row.sprite.pixels = nullptr;
            row.sprite.palette =
                reinterpret_cast<const kf_color *>(base + data_offset);
            row.sprite.indices = base + data_offset + palette_padded;
            row.sprite.width = width;
            row.sprite.height = height;
            row.sprite.frame_count = frame_count;
            row.sprite.palette_count = palette_count;
            row.sprite.has_color_key = has_color_key;
            /* Mirrors the key colour onto the sprite so a caller that only
             * knows about kf_sprite::color_key still reads something true.
             * The blitter never uses it for an indexed sprite -- it compares
             * KF_SPRITE_KEY_INDEX against the index byte instead, which is
             * cheaper and cannot disagree with the palette. */
            row.sprite.color_key = row.sprite.palette[KF_SPRITE_KEY_INDEX];
            row.sprite.format = KF_SPRITE_FORMAT_INDEXED8;
        }
```

`kf_assets_get()` must now return the sprite for **either** sprite type. Change
its guard:

```cpp
const kf_sprite *kf_assets_get(const char *name) {
    const AssetEntry *e = find_entry(name);
    if (e == nullptr || (e->type != KF_ASSET_TYPE_SPRITE &&
                          e->type != KF_ASSET_TYPE_SPRITE_INDEXED)) {
        return nullptr;
    }
    return &e->sprite;
}
```

Update `kf/assets.h`'s `kf_assets_get()` comment: it currently says a name
whose entry "is not `KF_ASSET_TYPE_SPRITE`" returns NULL, which would now
contradict the code. Say instead that it returns any *sprite* type, RGB565 or
indexed, and that the caller reads `kf_sprite::format` only if it cares.

Also set `row.sprite.frame_count = 1u;` in the existing RGB565 branch.

- [ ] **Step 8: Register the test**

In `simulator/CMakeLists.txt`, beside the `KF_CREATURE_DEMO_PACK_PATH`
definition (~line 199):

```cmake
        KF_INDEXED_FIXTURE_PACK_PATH="${CMAKE_SOURCE_DIR}/examples/hello_sprite/assets_indexed.kfpack"
```

and after the `asset_pipeline_check` block:

```cmake
    # Proves ASSET_TYPE_SPRITE_INDEXED decodes -- palette, frame count,
    # contiguous frames -- and, the point of the whole format, that
    # expanding the indices through the palette reproduces the RGB565
    # test_sprite byte for byte. Mounts the indexed fixture through the
    # RUNTIME override and restores the default, so it never disturbs the
    # golden-checksum tests above.
    add_test(NAME indexed_asset_check
             COMMAND kamiframe-headless --verify-indexed-assets)
```

- [ ] **Step 9: Run the test and the whole suite**

Run: `cmake --build build -j8 && ctest --test-dir build -R indexed_asset_check --output-on-failure`
Expected: PASS, with `indexed-assets: format decodes and is lossless`.

Run: `bash dev.sh test`
Expected: **34/34 pass** — the original 33 with no checksum touched, plus the
new one. Nothing has been converted yet, so any change to an existing test here
means the `kf_sprite` growth broke something.

- [ ] **Step 10: Confirm Core stayed heap-free**

Run: `python3 tools/check_no_heap.py .`
Expected: no findings.

- [ ] **Step 11: Commit**

```bash
git add hakoniwaos/include/kf/types.h hakoniwaos/include/kf/assets.h \
        hakoniwaos/src/assets.cpp hakoniwaos/src/demo.cpp \
        tools/kf_pack_assets.py examples/hello_sprite/assets_indexed.kfpack \
        simulator/src/headless/headless_main.cpp simulator/CMakeLists.txt
git commit -m "A sprite can be indexed, and can have more than one frame"
```

---

### Task 2: The blitter draws indexed pixels, and draws a chosen frame

The drawing half. Two new entry points take a frame number; the two existing
ones become frame-0 wrappers, so every call site in the repo keeps compiling
and keeps producing identical bytes.

**Files:**
- Modify: `hakoniwaos/include/kf/blit.h`
- Modify: `hakoniwaos/src/blit.cpp`
- Modify: `hakoniwaos/include/kf/budget.h` (one comment beside `KF_DRAW_KEYED_PX_PER_US`)
- Test: `simulator/src/headless/headless_main.cpp` (new `run_indexed_blit_check()`)
- Modify: `simulator/CMakeLists.txt`

**Interfaces:**
- Consumes: `kf_sprite`, `kf_sprite_format`, `KF_SPRITE_KEY_INDEX` (Task 1).
- Produces: `kf_blit_frame(const kf_sprite *, int16_t x, int16_t y, uint16_t frame)`
  and `kf_blit_frame_mirrored(const kf_sprite *, int16_t x, int16_t y, uint16_t frame)`.
  Task 6 calls both.

**Design note — the precedent in `kf/blit.h` argues both ways, and the
distinction matters.** `kf_blit_mirrored()` is a separate function rather than
a `bool mirror` flag, and the header spends a paragraph on why: a flag gives a
caller somewhere to accidentally pass `true` into a hot path, and keeping the
functions apart leaves `kf_blit()`'s golden output untouched *by construction*.

That reasoning does not transfer, and the reason it does not is worth writing
into the header. **Mirroring is a behaviour the caller chooses. Format is a
property of the data.** A caller cannot pass the wrong format by accident —
there is nothing to pass; the sprite already knows. And the direction of travel
is opposite: mirroring stays rare, while *every* sprite becomes indexed. Four
separate public functions (`kf_blit`, `kf_blit_indexed`, `kf_blit_mirrored`,
`kf_blit_indexed_mirrored`) is the combinatorial trap that pure separation
leads to here, and it would force every call site to branch on format itself.

So: **branch once on `sprite->format` at the top of each blit, outside the
per-pixel loop, and dispatch to a per-format inner loop.** The cost is one
predictable branch per *call*, not per pixel. The frame parameter is added as a
new pair of entry points rather than by changing the existing signatures,
because that keeps the "call sites untouched by construction" half of the
mirrored precedent — which is the half that still applies.

The trap inside the trap: **`frame` must be clamped, not wrapped.** A caller
asking for frame 7 of a 3-frame sprite is a bug (Task 6's playback cursor
surviving a pose change is exactly how it would happen). Wrapping to frame 1
hides it behind plausible-looking animation; clamping to frame 0 shows a
correct, still sprite, which is visibly wrong in the right way and reads back
in a test.

**Dirty-rect cost: 0 new.** `kf_blit_frame()` marks exactly the same clipped
rect `kf_blit()` does; a frame is a different *source*, not a different
destination.

- [ ] **Step 1: Write the failing test**

```cpp
/* The centrepiece losslessness proof, and it does not need a golden
 * constant to make it: draw the RGB565 test_sprite and the indexed one at
 * the same place into the same framebuffer, and compare the two results
 * byte for byte. If 8bpp indexing lost anything for this art, this fails.
 *
 * Also pins frame addressing (frame k reads k*w*h into the payload), the
 * out-of-range clamp, mirrored equivalence, and the draw-counter bucket. */
static int run_indexed_blit_check(void) {
    bool ok = true;
    auto check = [&ok](bool cond, const char *what) {
        if (!cond) { KF_LOGE(TAG, "FAILED: %s", what); ok = false; }
    };

    kf_arena_init_all();
    kf_fb_init();

    check(kf_assets_init() == KF_OK, "default pack mounts");
    const kf_sprite *rgb = kf_assets_get("test_sprite");
    check(rgb != nullptr, "RGB565 test_sprite found");

    const size_t fb_bytes =
        static_cast<size_t>(KF_DISPLAY_WIDTH) * KF_DISPLAY_HEIGHT * sizeof(kf_color);
    std::vector<uint8_t> from_rgb(fb_bytes);
    std::vector<uint8_t> from_indexed(fb_bytes);

    if (rgb != nullptr) {
        kf_fill(KF_RGB(8, 16, 24));
        kf_blit(rgb, 40, 50);
        std::memcpy(from_rgb.data(), kf_fb_pixels(), fb_bytes);
    }
    kf_assets_shutdown();

    /* kf_arena_init_all() is NOT called again here, or anywhere else in this
     * function -- it panics on a second call (kf/arena.cpp:
     * `KF_ASSERT(!g_initialised, "kf_arena_init_all called twice")`), and
     * KF_ARENA_ASSETS is not one of the resettable arenas (only
     * KF_ARENA_SCRATCH is). One kf_arena_init_all() call at the top of this
     * function is enough for every kf_assets_shutdown()/kf_assets_init()
     * remount below -- see run_indexed_asset_check() (Task 1) for where
     * this was found the hard way. */
    kf_host_assets_set_pack_path(KF_INDEXED_FIXTURE_PACK_PATH);
    check(kf_assets_init() == KF_OK, "indexed fixture mounts");
    const kf_sprite *ix = kf_assets_get("test_sprite");
    check(ix != nullptr, "indexed test_sprite found");

    if (ix != nullptr) {
        kf_fill(KF_RGB(8, 16, 24));
        kf_blit(ix, 40, 50);
        std::memcpy(from_indexed.data(), kf_fb_pixels(), fb_bytes);
        check(from_rgb == from_indexed,
              "an indexed blit and an RGB565 blit of the same sprite produce "
              "a byte-identical framebuffer");

        /* Mirrored, same claim. */
        kf_fill(KF_RGB(8, 16, 24));
        kf_blit_mirrored(ix, 40, 50);
        std::vector<uint8_t> ix_mirror(fb_bytes);
        std::memcpy(ix_mirror.data(), kf_fb_pixels(), fb_bytes);
        kf_assets_shutdown();
        kf_host_assets_set_pack_path(nullptr);
        check(kf_assets_init() == KF_OK, "default pack remounts");
        const kf_sprite *rgb2 = kf_assets_get("test_sprite");
        kf_fill(KF_RGB(8, 16, 24));
        if (rgb2 != nullptr) { kf_blit_mirrored(rgb2, 40, 50); }
        check(std::memcmp(ix_mirror.data(), kf_fb_pixels(), fb_bytes) == 0,
              "a mirrored indexed blit matches a mirrored RGB565 blit");
        kf_assets_shutdown();
    }

    /* Frames. */
    kf_host_assets_set_pack_path(KF_INDEXED_FIXTURE_PACK_PATH);
    check(kf_assets_init() == KF_OK, "indexed fixture remounts");
    const kf_sprite *anim = kf_assets_get("test_sprite_anim");
    check(anim != nullptr && anim->frame_count == 3u, "3-frame fixture found");
    if (anim != nullptr) {
        std::vector<uint8_t> f0(fb_bytes), f1(fb_bytes), f_oob(fb_bytes);
        kf_fill(KF_BLACK); kf_blit_frame(anim, 40, 50, 0);
        std::memcpy(f0.data(), kf_fb_pixels(), fb_bytes);
        kf_fill(KF_BLACK); kf_blit_frame(anim, 40, 50, 1);
        std::memcpy(f1.data(), kf_fb_pixels(), fb_bytes);
        kf_fill(KF_BLACK); kf_blit_frame(anim, 40, 50, 99);
        std::memcpy(f_oob.data(), kf_fb_pixels(), fb_bytes);

        check(f0 != f1, "frame 1 draws something different from frame 0");
        check(f0 == f_oob,
              "an out-of-range frame clamps to frame 0 rather than wrapping "
              "-- a wrap would hide a stale cursor behind plausible-looking "
              "animation");

        kf_fill(KF_BLACK);
        kf_draw_counters_reset();
        kf_blit_frame(anim, 40, 50, 0);
        const kf_draw_counters counters = kf_draw_counters_get();
        check(counters.opaque_pixels == 0u &&
                  counters.keyed_pixels ==
                      static_cast<uint32_t>(anim->width) * anim->height,
              "an indexed blit is charged entirely to the keyed bucket -- it "
              "has no memcpy fast path, so its cost SHAPE is per-pixel "
              "whether or not a key is tested");
    }

    kf_assets_shutdown();
    kf_host_assets_set_pack_path(nullptr);

    if (!ok) { return 1; }
    KF_LOGI(TAG, "indexed-blit: pixel-identical to RGB565, frames address "
                  "correctly");
    return 0;
}
```

Wire in `--verify-indexed-blit` beside the other flags.

- [ ] **Step 2: Run it to make sure it fails**

Run: `cmake --build build -j8`
Expected: FAIL — `'kf_blit_frame' was not declared in this scope`.

- [ ] **Step 3: Declare the new entry points**

Add to `hakoniwaos/include/kf/blit.h`, after `kf_blit_mirrored()`:

```c
/* Draw one frame of a possibly-multi-frame sprite. kf_blit() above is
 * exactly kf_blit_frame(sprite, x, y, 0) and kf_blit_mirrored() is exactly
 * kf_blit_frame_mirrored(sprite, x, y, 0) -- the two-argument forms are kept
 * so every existing call site stays untouched and its output stays identical
 * by construction, the same reason kf_blit_mirrored() is its own function.
 *
 * `frame` is CLAMPED to frame 0 when it is >= sprite->frame_count, not
 * wrapped. Asking for a frame that does not exist is a caller bug -- most
 * plausibly an animation cursor that survived a change of sprite -- and
 * wrapping would hide it behind animation that looks fine. Frame 0 is a
 * correct, still image, which is visible in a screenshot and catchable in a
 * test.
 *
 * BOTH FORMATS, ONE FUNCTION. Unlike mirroring, which is a behaviour the
 * CALLER chooses and therefore gets its own function so nobody can pass the
 * wrong flag into a hot path, the pixel format is a property of the DATA:
 * the sprite already knows, there is nothing for a caller to get wrong, and
 * every sprite in this project is migrating to indexed anyway. Making format
 * a fourth function instead of a branch would give us kf_blit x {plain,
 * mirrored} x {rgb565, indexed} and force every call site to branch on
 * something it should not have to know about. So these branch once on
 * sprite->format, at the top, outside the per-pixel loop.
 *
 * COST. An indexed row is a byte load, a key compare against
 * KF_SPRITE_KEY_INDEX, and a palette lookup, per pixel -- there is no memcpy
 * fast path and there never can be one, so an indexed blit is charged
 * entirely to the keyed draw counter regardless of has_color_key, exactly as
 * kf_blit_mirrored() already is and for the same "bucket reflects cost
 * SHAPE" reason. An un-keyed RGB565 sprite still gets its whole-row memcpy
 * and its opaque bucket; that four-times-cheaper path is why
 * KF_ASSET_TYPE_SPRITE was not deleted when indexing arrived. */
void kf_blit_frame(const kf_sprite *sprite, int16_t x, int16_t y,
                   uint16_t frame);
void kf_blit_frame_mirrored(const kf_sprite *sprite, int16_t x, int16_t y,
                            uint16_t frame);
```

- [ ] **Step 4: Implement**

In `hakoniwaos/src/blit.cpp`, add to the anonymous namespace:

```cpp
/* Where frame `frame`'s index bytes start. Clamps out of range to frame 0 --
 * see kf/blit.h's own comment on why clamping beats wrapping here. */
const uint8_t *frame_indices(const kf_sprite *sprite, uint16_t frame) {
    const uint16_t f = (frame < sprite->frame_count) ? frame : 0u;
    return sprite->indices +
           (static_cast<size_t>(f) * sprite->width * sprite->height);
}

/* The indexed inner loop, shared by the plain and mirrored blits. `step` is
 * +1 for a normal row and -1 for a mirrored one, and `src_x_first` is the
 * source column the leftmost destination column reads -- which is what makes
 * mirror-aware clipping fall out for free, exactly as kf_blit_mirrored()'s
 * own comment already explains for the RGB565 case. */
void blit_indexed_rows(const kf_sprite *sprite, kf_rect c, int src_y0,
                       int src_x_first, int step, const uint8_t *frame_base) {
    kf_color *fb = kf_fb_pixels();
    const int width = c.x1 - c.x0;
    const int height = c.y1 - c.y0;
    const kf_color *pal = sprite->palette;
    const bool keyed = sprite->has_color_key;

    /* No memcpy fast path exists for indexed data in either direction: every
     * destination pixel needs a palette lookup even when nothing is keyed
     * out. Charged accordingly. */
    g_counters.keyed_pixels += kf_rect_area(c);

    for (int row = 0; row < height; ++row) {
        const uint8_t *src =
            frame_base + (static_cast<size_t>(src_y0 + row) * sprite->width);
        kf_color *dst =
            fb + (static_cast<size_t>(c.y0 + row) * KF_DISPLAY_WIDTH) + c.x0;
        int sx = src_x_first;
        for (int col = 0; col < width; ++col, sx += step) {
            const uint8_t idx = src[sx];
            if (keyed && idx == KF_SPRITE_KEY_INDEX) {
                continue;
            }
            dst[col] = pal[idx];
        }
    }
}
```

Rename the existing bodies of `kf_blit()` and `kf_blit_mirrored()` into
`kf_blit_frame()` and `kf_blit_frame_mirrored()`, insert the format branch
after the clip, and make the old names one-line wrappers. The plain case:

```cpp
void kf_blit_frame(const kf_sprite *sprite, int16_t x, int16_t y,
                   uint16_t frame) {
    KF_ASSERT(sprite != nullptr, "kf_blit_frame(nullptr)");

    const kf_rect want = {x, y,
                          clamp16(static_cast<int32_t>(x) + sprite->width,
                                  INT16_MIN, INT16_MAX),
                          clamp16(static_cast<int32_t>(y) + sprite->height,
                                  INT16_MIN, INT16_MAX)};
    const kf_rect c = kf_rect_intersect(want, kFullScreen);
    if (kf_rect_is_empty(c)) {
        return;
    }

    const int src_x0 = c.x0 - x;
    const int src_y0 = c.y0 - y;

    if (sprite->format == KF_SPRITE_FORMAT_INDEXED8) {
        KF_ASSERT(sprite->indices != nullptr && sprite->palette != nullptr,
                  "indexed sprite has no indices or no palette");
        blit_indexed_rows(sprite, c, src_y0, src_x0, +1,
                          frame_indices(sprite, frame));
        kf_fb_mark_dirty(c);
        return;
    }

    KF_ASSERT(sprite->pixels != nullptr, "sprite has no pixels");
    /* ... the existing RGB565 opaque/keyed bodies, unchanged ... */
    kf_fb_mark_dirty(c);
}

void kf_blit(const kf_sprite *sprite, int16_t x, int16_t y) {
    kf_blit_frame(sprite, x, y, 0u);
}
```

and the mirrored case identically, passing `src_x_first = (x + sprite->width -
1) - c.x0` and `step = -1` into `blit_indexed_rows()`.

- [ ] **Step 5: Say what is now unmeasured**

Add to `hakoniwaos/include/kf/budget.h`, beneath `KF_DRAW_KEYED_PX_PER_US`:

```c
/* AND ONE MORE THING THIS NUMBER IS NOW COVERING. Since indexed sprites
 * landed (KF_ASSET_TYPE_SPRITE_INDEXED, kf/assets.h) the keyed bucket also
 * carries per-pixel PALETTE LOOKUPS, which the original figure did not
 * contemplate: an indexed pixel is a byte load, a compare, and a 16-bit
 * table read, against the keyed figure's assumed load-compare-store. The
 * palette is at most 512 bytes and is read for every pixel of a blit, so it
 * should stay cache-resident and the difference should be small -- but this
 * is a guess sitting on top of a guess. Split it into its own constant if
 * bring-up measures a real gap. Do not split it before then: a second
 * uncalibrated number is worse than one, because it looks like knowledge. */
```

- [ ] **Step 6: Register and run**

```cmake
    add_test(NAME indexed_blit_check
             COMMAND kamiframe-headless --verify-indexed-blit)
```

Run: `cmake --build build -j8 && ctest --test-dir build -R indexed_blit_check --output-on-failure`
Expected: PASS — `indexed-blit: pixel-identical to RGB565, frames address correctly`.

Run: `bash dev.sh test`
Expected: **35/35**. `headless_determinism`, `headless_fullscreen` and
`blit_mirror_check` must all still pass with no constant edited — the RGB565
paths were moved into new function bodies, not changed, and that is what proves
the move was mechanical.

- [ ] **Step 7: Commit**

```bash
git add hakoniwaos/include/kf/blit.h hakoniwaos/src/blit.cpp \
        hakoniwaos/include/kf/budget.h \
        simulator/src/headless/headless_main.cpp simulator/CMakeLists.txt
git commit -m "The blitter reads indexed pixels, and reads a frame you name"
```

---

### Task 3: Convert the default pack, and let the golden checksums judge it

The proof that matters. `examples/hello_sprite/assets.kfpack` is what
`headless_determinism` and `headless_fullscreen` render 300 frames from. Convert
it, and those two checksums must not move by one bit. Doing this now, before the
art migrates, puts every later task under that guard.

**Files:**
- Modify: `examples/hello_sprite/assets.kfpack` (regenerated, indexed)
- Modify: `simulator/src/headless/headless_main.cpp` (`run_asset_check()` for the new layout)
- Modify: `simulator/CMakeLists.txt` (the `asset_pipeline_check` comment)
- Modify: `docs/architecture/adr-0033-asset-pipeline.md` (a superseding note)

**Interfaces:**
- Consumes: Task 1's `--test-sprite --indexed`, Task 2's blitter.
- Produces: no new API. Produces the guarantee every later task leans on.

**Design note — the trap is expecting `asset_pipeline_check` to survive
untouched.** It will not, and that is correct. `run_asset_check()`
(`headless_main.cpp:4221`) hardcodes `asset_type == 0`, `data_bytes ==
width*height*2`, and `sprite->pixels[0] == sprite->color_key`. Its own comment
explains why it duplicates the byte layout instead of sharing constants with the
parser: "this check exists to catch the two ever drifting apart, so it must not
share the one piece of code that could hide that drift." A format-drift detector
that did not notice a format change would be broken. Rewrite it for the indexed
layout and **keep the independent re-parse** — it must still read the file's own
bytes with its own arithmetic, and it should now additionally expand the
palette by hand and compare against what `kf_assets_get()` reported.

What must **not** change is `KAMIFRAME_GOLDEN_CHECKSUM` (`2aceae654b21ca1b`) or
`KAMIFRAME_GOLDEN_CHECKSUM_STRESS` (`3fbdcabfba49e9ef`) in
`simulator/CMakeLists.txt`. If either moves, stop: the conversion is lossy, or
the blitter is wrong, and no amount of updating the constant will make that
untrue.

**Dirty-rect cost: 0.** Nothing about what is drawn changes.

- [ ] **Step 1: Record what the pack is now, so the shrink is a measurement**

Run: `ls -l examples/hello_sprite/assets.kfpack`
Expected: 2116 bytes (1 entry, 32x32 RGB565 = 2048 payload + 16 header + 52
directory).

- [ ] **Step 2: Regenerate it indexed**

Run:
```bash
python3 tools/kf_pack_assets.py --test-sprite --indexed \
    -o examples/hello_sprite/assets.kfpack
ls -l examples/hello_sprite/assets.kfpack
```
Expected: around 1156 bytes — 1024 index bytes + 64 palette + 16 + 52. Roughly
45% of the original, which is the 2:1 pixel saving diluted by a fixed 68-byte
overhead that a 32x32 sprite cannot amortise. A 48x48 creature sprite amortises
it far better; see Task 4's measurement.

- [ ] **Step 3: Run the two golden tests before touching anything else**

Run: `cmake --build build -j8 && ctest --test-dir build -R "headless_determinism|headless_fullscreen" --output-on-failure`
Expected: **both PASS with the checksum constants exactly as they were.** This
is the moment the whole plan is either right or wrong. If a checksum mismatch
appears, do not edit the constant: go back to Task 2's
`run_indexed_blit_check()`, which should have caught it, and work out why it
did not.

- [ ] **Step 4: Watch `asset_pipeline_check` fail, deliberately**

Run: `ctest --test-dir build -R asset_pipeline_check --output-on-failure`
Expected: FAIL, on "the pack file's own directory marks this entry as
ASSET_TYPE_SPRITE (0)" and on the `data_bytes == w*h*2` assertion. This is the
format-drift detector doing its job.

- [ ] **Step 5: Rewrite the check for the indexed layout**

In `run_asset_check()`, keep the structure and the independent re-parse; replace
the sprite-specific assertions:

```cpp
    if (sprite != nullptr) {
        check(sprite->format == KF_SPRITE_FORMAT_INDEXED8,
              "the checked-in default pack is now indexed -- see "
              "docs/superpowers/plans/2026-08-10-animated-indexed-sprites.md");
        check(sprite->width == 32u && sprite->height == 32u,
              "test_sprite is 32x32, the fixed size "
              "tools/kf_pack_assets.py --test-sprite always writes");
        check(sprite->frame_count == 1u, "test_sprite is a single frame");
        check(sprite->has_color_key, "test_sprite carries a color key");
        check(sprite->indices[0] == KF_SPRITE_KEY_INDEX,
              "the sprite's own corner pixel (outside the body ellipse in "
              "the generator's math) reads back as the colour-key INDEX -- a "
              "real decoded pixel, not zeroed or garbage memory");
        check(sprite->palette[KF_SPRITE_KEY_INDEX] == sprite->color_key,
              "palette entry 0 is the colour key, the convention "
              "KF_SPRITE_KEY_INDEX names");
```

and, inside the independent file re-parse, replace the `asset_type == 0` and
`data_bytes == w*h*2` assertions with the indexed layout, computing the padded
palette size from the file's own bytes rather than from anything the C++ parser
produced:

```cpp
                const uint8_t asset_type = entry[32];
                check(asset_type == 2u,
                      "the pack file's own directory marks this entry as "
                      "ASSET_TYPE_SPRITE_INDEXED (2)");

                uint16_t width = 0u, height = 0u, frame_count = 0u;
                std::memcpy(&width, entry + 36, sizeof(width));
                std::memcpy(&height, entry + 38, sizeof(height));
                std::memcpy(&frame_count, entry + 40, sizeof(frame_count));
                const uint16_t palette_count =
                    static_cast<uint16_t>(entry[42]) + 1u;
                const bool has_color_key = (entry[43] & 0x01u) != 0u;

                check(width == sprite->width && height == sprite->height &&
                          frame_count == sprite->frame_count &&
                          palette_count == sprite->palette_count &&
                          has_color_key == sprite->has_color_key,
                      "the metadata in the file's own type_meta matches what "
                      "kf_assets_get() reported");

                uint32_t data_offset = 0u, data_bytes = 0u;
                std::memcpy(&data_offset, entry + 44, sizeof(data_offset));
                std::memcpy(&data_bytes, entry + 48, sizeof(data_bytes));

                const uint32_t palette_padded =
                    ((static_cast<uint32_t>(palette_count) * 2u) + 3u) & ~3u;
                check(data_bytes == palette_padded +
                                        static_cast<uint32_t>(frame_count) *
                                            width * height,
                      "data_bytes recorded in the file's own directory "
                      "matches padded palette + frames*w*h");

                /* The real "matches what the packer wrote" proof, now that a
                 * pixel is two file reads instead of one: expand every index
                 * through the file's OWN palette bytes and compare against
                 * the colours the loaded sprite yields. Done with this
                 * check's own arithmetic, not the parser's, for the same
                 * reason this whole block re-parses by hand. */
                bool payload_matches = true;
                const uint8_t *pal_raw = raw.data() + data_offset;
                const uint8_t *idx_raw = pal_raw + palette_padded;
                for (uint32_t i = 0; i < static_cast<uint32_t>(width) * height;
                     ++i) {
                    const uint8_t slot = idx_raw[i];
                    const uint16_t from_file =
                        static_cast<uint16_t>(pal_raw[slot * 2u]) |
                        static_cast<uint16_t>(pal_raw[slot * 2u + 1u] << 8);
                    if (from_file != sprite->palette[sprite->indices[i]]) {
                        payload_matches = false;
                        break;
                    }
                }
                check(payload_matches,
                      "every pixel expanded from the file's own palette and "
                      "index bytes matches what the loaded sprite yields");
```

- [ ] **Step 6: Update the CMake comment beside the test**

The `asset_pipeline_check` comment in `simulator/CMakeLists.txt` currently says
the payload is compared as `width*height*2` RGB565 bytes. Rewrite it to
describe the palette-expansion comparison. A comment that describes the old
format would be a defect in this codebase's terms.

- [ ] **Step 7: Note the supersession in ADR 0033**

ADR 0033 states `ASSET_TYPE_SPRITE (0)` is "the only type anything loads today"
and that `examples/hello_sprite/assets.kfpack` holds a 32x32 RGB565 blob. Add a
short **Superseded in part** note under its Decision section pointing at this
plan: a third asset type exists, the default pack is now indexed, the directory
entry shape and `FORMAT_VERSION` are unchanged, and the extension went exactly
the way that ADR's own "Cost to change" section predicted.

- [ ] **Step 8: Run everything**

Run: `bash dev.sh test`
Expected: **35/35**, with `KAMIFRAME_GOLDEN_CHECKSUM` and
`KAMIFRAME_GOLDEN_CHECKSUM_STRESS` untouched in the diff. Confirm that with
`git diff simulator/CMakeLists.txt` — the only change to that file should be the
comment.

- [ ] **Step 9: Commit**

```bash
git add examples/hello_sprite/assets.kfpack \
        simulator/src/headless/headless_main.cpp simulator/CMakeLists.txt \
        docs/architecture/adr-0033-asset-pipeline.md
git commit -m "The default pack goes indexed, and the golden frames do not move"
```

---

### Task 4: The ingest tool packs animations

The art side. `tools/kf_ingest_sprites.py` learns to gather a state's frames
into one entry, build that entry's palette, and emit the indexed type. The real
creature pack halves.

**Files:**
- Modify: `tools/kf_character_manifest.py` (`SpriteSpec.entry_name`, `iter_entries()`)
- Modify: `tools/kf_ingest_sprites.py` (frame grouping, palette build, `--verify-lossless`)
- Modify: `examples/creature_demo/assets.kfpack` (regenerated)

**Interfaces:**
- Consumes: `packer.make_indexed_asset()`, `packer.quantize_rgb565()` (Task 1).
- Produces: `SpriteSpec.entry_name` (the sprite name **with** its `_NN` frame
  suffix still attached at this task — Task 5 removes it) and
  `iter_entries(raw)`, yielding one `EntrySpec` per animation with its ordered
  list of `SpriteSpec` frames. Task 5 changes what `entry_name` returns and
  nothing else.

**Design note — the trap is doing the rename in this task.** It is tempting to
drop the `_01` suffix here, while the ingest tool is already open. Do not: the
pack entry name is the contract with `kf_creature_sprite_name()`, which still
emits `_01`, and changing one without the other leaves
`creature_screen_sprite_check` failing across a commit boundary. Producer and
consumer move together, in Task 5, in one commit. **This task changes the
payload format and nothing about the names**, so every existing name lookup
keeps resolving and every test keeps passing.

The second trap: the palette must be built across **all frames of the entry at
once**, not per frame. Frames share one payload and one palette; a per-frame
palette would mean index 7 meaning different colours in frame 0 and frame 1.
`quantize_rgb565()` takes a list of frames for exactly this reason.

The third: **the colour key must be in the palette even when no pixel uses it.**
`quantize_rgb565()` seeds slot 0 with the key unconditionally. A fully opaque
sprite would otherwise put a real colour in slot 0 and have the blitter refuse
to draw it.

**Dirty-rect cost: 0.**

- [ ] **Step 1: Write the failing test — a tool self-check, run by CTest**

Add `--verify-lossless` to `tools/kf_ingest_sprites.py`: after packing, re-read
the written file's own bytes, expand every entry's indices through its own
palette, and compare against the RGB565 the same PNGs produce through the
existing `validate_and_load()` path.

```python
def verify_lossless(data: bytes, results: list[IngestResult]) -> list[str]:
    """Expands every indexed entry in a freshly-written pack back to RGB565
    and compares it, pixel for pixel, against the RGB565 those same PNGs
    resolve to. Reads the pack's OWN bytes rather than reusing the packing
    bookkeeping -- the same reasoning verify_pack() already gives: the check
    cannot pass by the packer merely agreeing with itself.

    This is the whole justification for choosing 8bpp over 4bpp. If it ever
    reports a problem, the art has more than 256 colours in one entry or the
    quantiser has a bug; either way the pack is no longer a lossless
    re-encoding of the source and must not ship as one."""
    problems = []
    by_name = {}
    for r in results:
        if r.status == "ok":
            by_name.setdefault(r.entry_name, []).append(r)

    version, entry_count, directory_offset = struct.unpack_from("<HHI", data, 4)
    for i in range(entry_count):
        e = directory_offset + i * packer.DIRECTORY_ENTRY_BYTES
        name = data[e:e + packer.NAME_BYTES].split(b"\x00", 1)[0].decode("ascii")
        asset_type = data[e + 32]
        if asset_type != packer.ASSET_TYPE_SPRITE_INDEXED:
            problems.append(f"entry '{name}': expected an indexed sprite, "
                             f"got asset_type {asset_type}")
            continue
        w, h, frames = struct.unpack_from("<HHH", data, e + 36)
        pal_count = data[e + 42] + 1
        off, nbytes = struct.unpack_from("<II", data, e + 44)
        pal_padded = ((pal_count * 2) + 3) & ~3
        palette = list(struct.unpack_from(f"<{pal_count}H", data, off))
        idx = data[off + pal_padded:off + nbytes]

        expected = by_name.get(name, [])
        if len(expected) != frames:
            problems.append(f"entry '{name}': pack says {frames} frame(s), "
                             f"{len(expected)} source PNG(s) fed it")
            continue
        for k, r in enumerate(expected):
            src = struct.unpack(f"<{w * h}H", r.rgb565)
            got = [palette[b] for b in idx[k * w * h:(k + 1) * w * h]]
            if list(src) != got:
                bad = next(j for j in range(w * h) if src[j] != got[j])
                problems.append(
                    f"entry '{name}' frame {k}: pixel {bad} expanded to "
                    f"0x{got[bad]:04X}, source is 0x{src[bad]:04X} -- the "
                    "indexed encoding is LOSSY, which it must never be")
    return problems
```

`IngestResult` gains `entry_name` and `rgb565` (the RGB565 bytes
`validate_and_load()` already builds) so this has something to compare against.

- [ ] **Step 2: Run it to make sure it fails**

Run:
```bash
python3 tools/kf_ingest_sprites.py examples/creature_demo/sprites \
    -o /tmp/kf-probe.kfpack --verify-lossless
```
Expected: FAIL — either `AttributeError` on the new fields, or every entry
reported as "expected an indexed sprite, got asset_type 0", because nothing
emits the new type yet.

- [ ] **Step 3: Give the manifest an entry name and an entry iterator**

In `tools/kf_character_manifest.py`, add to `SpriteSpec`:

```python
    @property
    def entry_name(self) -> str:
        """The .kfpack DIRECTORY ENTRY name this sprite's frames pack into.

        Identical to sprite_name today. It becomes the frame-less base name
        in a follow-up commit, once kf_creature_sprite_name() stops emitting
        a frame suffix -- see docs/superpowers/plans/2026-08-10-animated-
        indexed-sprites.md Task 5. Introduced separately from sprite_name
        (which names a FILE, and correctly keeps its frame number forever,
        because a PNG really is one frame) so the two concepts can part
        company without a second sweep through every caller."""
        return self.sprite_name
```

and an iterator that groups them:

```python
@dataclass(frozen=True)
class EntrySpec:
    """One .kfpack entry: every frame of one entity's one state in one
    direction, in frame order. Frames live contiguously in a single entry
    (ADR 0033's format, extended -- see the plan named above) so a player
    addresses frame k arithmetically instead of by name."""
    entry_name: str
    frames: tuple[SpriteSpec, ...]

    @property
    def frame_count(self) -> int:
        return len(self.frames)


def iter_entries(raw: dict) -> Iterator[EntrySpec]:
    """iter_sprites(), grouped into pack entries. Order within a group is
    frame order (SpriteSpec.frame is 1-based and iter_sprites() yields them
    ascending); order between groups is iter_sprites()' own stable order."""
    groups: dict[str, list[SpriteSpec]] = {}
    order: list[str] = []
    for spec in iter_sprites(raw):
        if spec.entry_name not in groups:
            groups[spec.entry_name] = []
            order.append(spec.entry_name)
        groups[spec.entry_name].append(spec)
    for name in order:
        frames = sorted(groups[name], key=lambda s: s.frame)
        yield EntrySpec(entry_name=name, frames=tuple(frames))
```

Extend `validate_manifest()` to check `len(entry.frames) == frames[0].frame_count`
and that frame numbers are exactly `1..frame_count` with no gap — a missing
frame in the middle would otherwise silently shorten an animation.

- [ ] **Step 4: Make the ingest tool emit indexed entries**

In `tools/kf_ingest_sprites.py`, split `validate_and_load()` so it returns the
RGB565 bytes on the result rather than building a packer asset dict, then add:

```python
def build_entry(entry, results_by_name) -> dict | None:
    """Turns one EntrySpec's validated frames into one packable indexed
    asset. The palette is built ACROSS ALL FRAMES OF THIS ENTRY at once, not
    per frame: they share a payload and therefore must share a palette, or
    index 7 would mean one colour in frame 0 and another in frame 1.

    Per ENTRY, not per entity, and certainly not globally: measured, each of
    this roster's sprites uses 6 to 27 distinct colours while the union
    across just five entities is already 201, so a shared palette is a
    ceiling that would be hit late, after the format froze. A per-entry
    palette is ~64 bytes -- about 24KB across the whole projected roster,
    against 7.5MB of pixels."""
    frames = results_by_name.get(entry.entry_name, [])
    if len(frames) != entry.frame_count:
        return None
    key565 = packer.rgb565(*COLOR_KEY_RGB)
    per_frame = [list(struct.unpack(f"<{len(r.rgb565) // 2}H", r.rgb565))
                 for r in frames]
    palette, index_frames = packer.quantize_rgb565(per_frame, key565)
    spec = frames[0].spec
    return packer.make_indexed_asset(entry.entry_name, spec.width, spec.height,
                                      index_frames, palette,
                                      has_color_key=True, color_key=key565)
```

(`color_key=key565` is required, not optional, here: `make_indexed_asset()`
raises `ValueError` if `has_color_key=True` and `color_key` is `None` --
this is the same trap the earlier "carries bad comments" warning describes,
caught by actually running Step 2/5 rather than trusting the listing.)

Add the indexed branch to `verify_pack()` (it currently only knows
`data_bytes == w*h*2` for `ASSET_TYPE_SPRITE`), and wire `--verify-lossless`
into `main()` so it runs whenever `--out` is given.

- [ ] **Step 5: Run the tool against the real art**

Run:
```bash
python3 tools/kf_ingest_sprites.py examples/creature_demo/sprites \
    -o examples/creature_demo/assets.kfpack --strict --verify-lossless
ls -l examples/creature_demo/assets.kfpack
```
Expected (as of this figure's original writing, 49 sprites on disk): `ok: 49`,
`pack verification: OK`, `lossless verification: OK (49 entries, 112,896
pixels expanded and matched)`, and a file of roughly **116 KB** against the
previous **228,356 bytes** — a 49% reduction, which is the 2:1 pixel saving
less ~64 bytes of palette and 52 bytes of directory per entry.

**Stale by the time Task 4 actually ran:** `examples/creature_demo/sprites`
had grown to **94** PNGs (see this task's own top-of-file note), and the
roster manifest wants 379, so `--strict` fails on the 285 not yet drawn --
that gap is real and pre-existing, not something this task introduces or
should paper over by inventing placeholder art. Drop `--strict` for the
actual run; the honest result is `ok: 94`, and the pack drops from its
previous RGB565 size of 438,056 bytes (94 entries) by very close to half.

If a run reports a colour overflow instead, that is real news, not a bug:
some sprite has more than 256 colours and the "8bpp is lossless for this art"
premise no longer holds for it. Stop and report it rather than quantising.

- [ ] **Step 6: Run everything**

Run: `bash dev.sh test`
Expected: **35/35**. `creature_screen_sprite_check` matters most here — it mounts
this exact pack and samples real drawn pixels through `kf_blit()` and
`kf_blit_mirrored()`, so it is an end-to-end check that the converted art still
renders. It must pass **without modification**: every entry name is unchanged
in this task.

- [ ] **Step 7: Commit**

```bash
git add tools/kf_character_manifest.py tools/kf_ingest_sprites.py \
        examples/creature_demo/assets.kfpack
git commit -m "The art pipeline packs a whole animation into one indexed entry"
```

---

### Task 5: The `_01` that stopped being true

Once frames live inside one entry, a pack entry named `baby_neutral_s_01`
claims to be frame one of something. It is not; it is the whole animation. The
name goes.

**Files:**
- Modify: `hakoniwaos/src/creature.cpp` (`kf_creature_sprite_name()`)
- Modify: `hakoniwaos/include/kf/creature.h` (the naming comment)
- Modify: `tools/kf_character_manifest.py` (`entry_name` drops the suffix, docstring)
- Modify: `tools/kf_ingest_sprites.py` (nothing structural — it already goes through `entry_name`)
- Modify: `simulator/src/pet/kf_creature_screen.cpp` (`kShrineSpriteName`)
- Modify: `examples/creature_demo/assets.kfpack` (regenerated with the new names)
- Test: `simulator/src/headless/headless_main.cpp` (the name expectations in `run_creature_pose_check()`)

**Interfaces:**
- Consumes: `EntrySpec` / `entry_name` (Task 4).
- Produces: pack entry names of the shape `<stage><indices>_<pose>_<dir>` and,
  for the grudge variant, `<stage><indices>_<pose>_<dir>_grudge`.

**Design note — the resolution is a split, not a rename, and it costs zero file
renames.** The frame suffix is *correct* on a PNG: `baby_neutral_s_03.png`
really is frame three, one file per frame, and dropping it there would make the
art directory ambiguous the moment nine-frame animations arrive. It is *wrong*
on a pack entry, which now holds every frame. So:

- **PNG filenames keep `_NN`.** No file on disk is renamed. All 49 stay exactly
  as they are.
- **Pack entry names drop it.** `SpriteSpec.filename` and
  `SpriteSpec.entry_name` — which Task 4 deliberately introduced as separate
  concepts for this moment — part company here.

Keeping the suffix on entries was the alternative and it does not survive
contact with this codebase's own standard: a name that contradicts the code is
a defect, and "it's only cosmetic" is not an argument when the fix is a
one-line change to one `snprintf` and a regenerated pack. It also buys three
characters of the 31-character entry-name budget; the longest name goes from
`adult30_objecting_s_grudge_01` (29) to `adult30_objecting_s_grudge` (26).

**The trap: producer and consumer must move in the same commit.**
`kf_creature_sprite_name()` builds the name and the pack holds it. Change one
and every lookup in `creature_screen_sprite_check` returns null, and the screen
silently falls back to `kPlaceholderColor` — a *passing-looking* failure, since
that check has a branch for exactly that case. Regenerate the pack in the same
commit and verify by watching the check exercise the non-null path.

**Dirty-rect cost: 0.**

- [ ] **Step 1: Write the failing test**

In `run_creature_pose_check()`, update the `NameCase` expectations to the new
shape:

```cpp
    const NameCase names[] = {
        {KF_PET_STAGE_EGG, KF_CREATURE_POSE_NEUTRAL, KF_CREATURE_DIR_S,
         "egg_idle_s"},
        {KF_PET_STAGE_EGG, KF_CREATURE_POSE_SICK, KF_CREATURE_DIR_E,
         "egg_idle_e"},
        {KF_PET_STAGE_BABY, KF_CREATURE_POSE_NEUTRAL, KF_CREATURE_DIR_S,
         "baby_neutral_s"},
        {KF_PET_STAGE_BABY, KF_CREATURE_POSE_SLEEPING, KF_CREATURE_DIR_N,
         "baby_sleeping_n"},
        {KF_PET_STAGE_CHILD, KF_CREATURE_POSE_HAPPY, KF_CREATURE_DIR_W,
         "child_happy_w"},
    };
```

(match the existing struct's actual field order in the file — it carries a
direction as well as a stage and pose.)

- [ ] **Step 2: Run it to make sure it fails**

Run: `cmake --build build -j8 && ctest --test-dir build -R creature_pose_check --output-on-failure`
Expected: FAIL — `name: expected 'baby_neutral_s', got 'baby_neutral_s_01'`.

- [ ] **Step 3: Drop the suffix in the name builder**

In `hakoniwaos/src/creature.cpp`, `kf_creature_sprite_name()`:

```cpp
    if (stage == KF_PET_STAGE_EGG) {
        snprintf(out, out_len, "egg_idle_%s", dir_tok);
        return;
    }
    char stage_buf[16];
    stage_token(pet, stage_buf, sizeof(stage_buf));
    snprintf(out, out_len, "%s_%s_%s", stage_buf, pose_name(pose), dir_tok);
```

Rewrite the comment in `hakoniwaos/include/kf/creature.h` that currently reads
"Frame is always '01' -- multi-frame animation is unbuilt and out of scope
here." It must now say the opposite and say why:

```c
 * NO FRAME NUMBER. A pack entry holds EVERY frame of an animation
 * contiguously (see KF_ASSET_TYPE_SPRITE_INDEXED in kf/assets.h), so a name
 * ending "_01" would claim to be frame one of something it is in fact all
 * of. Which frame to draw is a runtime argument to kf_blit_frame()
 * (kf/blit.h), not part of the name -- which also means an animated pose
 * needs no naming change at all: the pose already picks the sprite, and an
 * animated one simply has more frames behind the same name. PNG FILES on
 * disk do keep a frame number, because one file really is one frame; see
 * tools/kf_character_manifest.py's SpriteSpec.filename versus its
 * .entry_name for where those two concepts part company.
```

- [ ] **Step 4: Drop it in the manifest's entry name**

```python
    @property
    def entry_name(self) -> str:
        """The .kfpack DIRECTORY ENTRY name this sprite's frames pack into:
        the sprite name with its frame number removed, because an entry
        holds every frame and a trailing "_01" on one would be a lie.

        Deliberately NOT the same as .filename: a PNG on disk is one frame
        and keeps its number forever. This is the split that lets nine-frame
        art sit in a directory unambiguously while the pack entry it feeds
        stays honest about being the whole animation.

        The authority for this shape is kf_creature_sprite_name()
        (hakoniwaos/src/creature.cpp), which is what a runtime lookup
        actually asks for; this property agrees with that function, never
        the other way round."""
        suffix = f"_{self.frame:02d}"
        assert self.sprite_name.endswith(suffix)
        return self.sprite_name[:-len(suffix)]
```

Update this module's `THE NAMING CONVENTION` docstring block: the `frame`
bullet must now say the frame index appears in the FILENAME only, and the
convention line becomes
`<stage_token><branch_indices>_<pose>_<dir>[_grudge]` for entries and
`…_<frame>.png` for files.

- [ ] **Step 5: Fix the one hardcoded name**

`simulator/src/pet/kf_creature_screen.cpp:224`:

```cpp
constexpr const char *kShrineSpriteName = "shrine_idle_s";
```

Search for any other literal ending `_01` before moving on:

Run: `grep -rn '_01"' hakoniwaos simulator tools examples --include="*.cpp" --include="*.h" --include="*.py"`
Expected: no remaining pack-entry literals — only PNG filenames, if any.

- [ ] **Step 6: Regenerate the pack in the same commit**

Run:
```bash
python3 tools/kf_ingest_sprites.py examples/creature_demo/sprites \
    -o examples/creature_demo/assets.kfpack --strict --verify-lossless
python3 tools/kf_character_manifest.py stats
```
Expected: 49 entries again, the same byte size as Task 4 minus 49x3 name bytes
(the name field is fixed at 32 bytes, so the file size is in fact **unchanged** —
only the bytes inside the name fields differ). `stats` should report the longest
name as 26 characters.

- [ ] **Step 7: Run everything, and check the right branch ran**

Run: `bash dev.sh test`
Expected: **35/35**. Then confirm `creature_screen_sprite_check` actually found
sprites rather than falling back to the placeholder:

Run: `./build/simulator/kamiframe-headless --verify-creature-screen-sprites`
(use the real binary path from `ctest -N`)
Expected: its own log lines reporting real sprite pixels, not the placeholder
colour. If it passes but the placeholder branch ran, the names are wrong and the
test is passing for the wrong reason.

- [ ] **Step 8: Commit**

```bash
git add hakoniwaos/src/creature.cpp hakoniwaos/include/kf/creature.h \
        tools/kf_character_manifest.py simulator/src/pet/kf_creature_screen.cpp \
        examples/creature_demo/assets.kfpack \
        simulator/src/headless/headless_main.cpp
git commit -m "An entry is a whole animation, so it stops calling itself frame one"
```

---

### Task 6: Frames actually play

The clock. Animations run at ~10fps while the display targets 30, so the frame
cursor has its own accumulator and does not care what the frame rate is.

**Files:**
- Modify: `hakoniwaos/include/kf/creature.h` (`kf_anim`, `KF_ANIM_FRAME_MS`, `kf_creature_tick_anim()`)
- Modify: `hakoniwaos/src/creature.cpp`
- Modify: `simulator/src/pet/kf_creature_screen.cpp` (draw the current frame; tick on the egg path)
- Modify: `simulator/src/pet/kf_creature_screen.h` (a debug accessor for the cursor)
- Test: `simulator/src/headless/headless_main.cpp` (new `run_creature_anim_check()`)
- Modify: `simulator/CMakeLists.txt`

**Interfaces:**
- Consumes: `kf_blit_frame()` / `kf_blit_frame_mirrored()` (Task 2),
  `kf_sprite::frame_count` (Task 1).
- Produces: `kf_anim` on `kf_creature`; `kf_creature_tick_anim(kf_creature *, uint32_t dt_ms)`;
  `kf_creature_screen_debug_anim_frame(void)`.

**Design note — the trap is the egg, which is the one thing that must animate
and the one thing that is gated out of `kf_creature_update()`.**
`kf_creature_screen_frame()` skips `kf_creature_update()` entirely while
`pet->stage == KF_PET_STAGE_EGG` (`kf_creature_screen.cpp:656`), because an egg
that strolls across the field reads as wrong. Putting the animation cursor
inside `kf_creature_update()` would therefore leave the egg — the exact sprite
the owner asked to see bobbing and squishing — as the only thing in the game
that never animates.

So the cursor advance is its own function, `kf_creature_tick_anim()`, called
first thing by `kf_creature_update()` **and** directly by the screen's egg
branch. One implementation, two callers, not a second less-tested path — the
same shape `handle_care_buttons()` already uses in that file for exactly this
reason.

The second trap: **the cursor must reset when the resolved sprite changes.** A
9-frame walk cycle leaves the cursor at 7; switch to a 3-frame objecting pose
and frame 7 does not exist. `kf_blit_frame()` clamps it to frame 0 (Task 2), so
nothing reads out of bounds — but the animation would visibly jump. The screen
already has the machinery to notice: `resolve_sprite()`'s `SpriteCache` compares
the requested name every frame precisely to know when the answer could have
changed. Reset the cursor on a cache miss.

The third: **`KF_ANIM_FRAME_MS` is 100 (10fps) and the accumulator carries the
remainder.** At a 33ms tick, `100 / 33` is not an integer, so `accum_ms += dt;
while (accum_ms >= KF_ANIM_FRAME_MS) { accum_ms -= KF_ANIM_FRAME_MS; advance(); }`
is what keeps the *average* rate right over time. Resetting the accumulator to
zero on each advance instead would drift to 3 ticks per frame — 9.1fps — and
would make a 9-frame cycle 90ms longer than intended every loop. It also keeps
determinism: the headless clock is synthetic, so the same `dt_ms` sequence
produces the same cursor sequence on every machine.

**Dirty-rect cost: 0 new, and this is the load-bearing claim of the task.**
`kf_creature_screen_frame()` already erases `g_previous` and redraws the sprite
on **every** frame, moving or not. Advancing a frame changes which source bytes
are read, not how many destination pixels are written or how many rectangles are
marked. The budget stays at 1 rect standing still, 2 walking, ≤ 13,824 bytes,
and `run_creature_screen_check()`'s existing `worst_rects <= 2` /
`worst_bytes <= 48*48*2*3` assertions must pass unchanged.

- [ ] **Step 1: Write the failing test**

```cpp
/* Playback: the cursor advances on its own ~10fps clock regardless of the
 * display's, wraps at the end, resets when the sprite changes, and -- the
 * thing that would quietly wreck the frame budget if it were wrong -- costs
 * no extra dirty rectangles, because the screen already redrew the creature
 * every frame anyway. */
static int run_creature_anim_check(void) {
    bool ok = true;
    auto check = [&ok](bool cond, const char *what) {
        if (!cond) { KF_LOGE(TAG, "FAILED: %s", what); ok = false; }
    };

    /* The clock, in isolation: pure Core, no screen, no assets. */
    kf_creature c;
    const kf_rect field = {0, 0, 240, 260};
    kf_rng_seed(1u);
    kf_creature_init(&c, field);
    check(c.anim.frame == 0u, "a fresh creature starts on frame 0");

    /* 100ms of animation at a 33ms display tick is 3 ticks with 1ms of
     * remainder carried, not 3 ticks with the remainder thrown away. */
    kf_creature_tick_anim(&c, 33u);
    kf_creature_tick_anim(&c, 33u);
    check(c.anim.frame == 0u, "66ms is not yet a frame");
    kf_creature_tick_anim(&c, 33u);
    check(c.anim.frame == 0u, "99ms is still not a frame, at 100ms each");
    kf_creature_tick_anim(&c, 33u);
    check(c.anim.frame == 1u, "the cursor advanced exactly once by 132ms");
    check(c.anim.accum_ms == 32u,
          "and kept the 32ms remainder rather than resetting to zero");

    /* Ten seconds at 33ms should be 100 frames' worth of advance, give or
     * take one tick's rounding -- the accumulator carrying its remainder is
     * what makes this true rather than 9.1fps drift. */
    kf_creature_init(&c, field);
    uint32_t advances = 0u;
    uint16_t last = c.anim.frame;
    for (int i = 0; i < 303; ++i) {
        kf_creature_tick_anim(&c, 33u);
        if (c.anim.frame != last) { ++advances; last = c.anim.frame; }
    }
    check(advances >= 99u && advances <= 100u,
          "10 seconds of 33ms ticks advances ~100 frames, not ~91 -- the "
          "accumulator is carrying its remainder");

    /* Wrapping. */
    kf_creature_init(&c, field);
    c.anim.frame = 2u;
    kf_creature_anim_wrap(&c, 3u);
    check(c.anim.frame == 2u, "an in-range cursor is left alone");
    c.anim.frame = 7u;
    kf_creature_anim_wrap(&c, 3u);
    check(c.anim.frame == 0u,
          "a cursor past the end of a shorter animation resets to 0 rather "
          "than wrapping to a frame the previous pose happened to be on");

    /* On screen, against the real animated fixture, and against the budget. */
    kf_arena_init_all();
    kf_fb_init();
    kf_host_assets_set_pack_path(KF_INDEXED_FIXTURE_PACK_PATH);
    check(kf_assets_init() == KF_OK, "indexed fixture mounts");
    kf_pet_session_init();
    kf_creature_screen_init();

    size_t worst_rects = 0;
    for (int i = 0; i < 300; ++i) {
        kf_fb_clear_dirty();
        kf_creature_screen_frame(33u);
        const kf_dirty_rects d = kf_fb_dirty_rects();
        if (static_cast<size_t>(d.count) > worst_rects) {
            worst_rects = static_cast<size_t>(d.count);
        }
    }
    check(worst_rects <= 2u,
          "animating the creature costs no extra dirty rectangles -- the "
          "screen already erased and redrew it every frame");

    kf_assets_shutdown();
    kf_host_assets_set_pack_path(nullptr);

    if (!ok) { return 1; }
    KF_LOGI(TAG, "creature-anim: cursor keeps its own clock, budget unmoved");
    return 0;
}
```

Wire in `--verify-creature-anim`.

- [ ] **Step 2: Run it to make sure it fails**

Run: `cmake --build build -j8`
Expected: FAIL — `'struct kf_creature' has no member named 'anim'`.

- [ ] **Step 3: Declare the clock**

Add to `hakoniwaos/include/kf/creature.h`:

```c
/* How long one animation frame is held, in milliseconds.
 *
 * 100ms is 10fps, in the middle of the 8-12fps band hand-drawn character
 * animation reads best at, and DELIBERATELY UNRELATED to the display's
 * KF_TARGET_FPS of 30: animation timing is an art decision and refresh rate
 * is a hardware one, and tying them together would mean a panel that ran at
 * 60 made every creature move twice as fast. The accumulator below is what
 * keeps the two independent. */
#define KF_ANIM_FRAME_MS 100u

/* An animation cursor. Integer milliseconds only -- hakoniwaos/ has no
 * floating point (kf/budget.h's own reasoning: the device's FPU is for Lua,
 * and Core stays exact and cheap).
 *
 * accum_ms CARRIES ITS REMAINDER across advances rather than resetting to
 * zero. At a 33ms display tick, three ticks is 99ms and four is 132ms; a
 * reset-to-zero cursor would take four ticks every time and run at 7.6fps
 * instead of 10, and a nine-frame cycle would come out 40% slow. Carrying
 * the remainder makes the AVERAGE rate exactly KF_ANIM_FRAME_MS regardless
 * of what dt_ms happens to be, which also means the same dt_ms sequence
 * produces the same frame sequence on every machine -- what the headless
 * checks' synthetic clock relies on. */
typedef struct {
    uint32_t accum_ms;
    uint16_t frame;
} kf_anim;
```

Add `kf_anim anim;` to `kf_creature`, and declare:

```c
/* Advance the creature's animation cursor by one frame's worth of wall time.
 *
 * Called first thing by kf_creature_update(), AND directly by a caller that
 * deliberately skips the wander -- today that is the egg, which sits still
 * by design (see simulator/src/pet/kf_creature_screen.cpp) and would
 * otherwise be the one thing in the game that never animates, which is
 * precisely backwards from what was asked for. One implementation, two
 * callers, rather than a second copy of the same arithmetic. */
void kf_creature_tick_anim(kf_creature *c, uint32_t dt_ms);

/* Bring the cursor back in range for an animation of `frame_count` frames,
 * resetting to 0 when it is past the end. Called when the resolved sprite
 * CHANGES: a 9-frame walk leaves the cursor at 7, and a 3-frame objecting
 * pose has no frame 7. Resets rather than wraps, so a pose change starts at
 * the beginning of its own cycle instead of somewhere arbitrary inherited
 * from the pose before it. */
void kf_creature_anim_wrap(kf_creature *c, uint16_t frame_count);
```

- [ ] **Step 4: Implement**

In `hakoniwaos/src/creature.cpp`:

```cpp
void kf_creature_tick_anim(kf_creature *c, uint32_t dt_ms) {
    if (c == nullptr || dt_ms == 0u) { return; }
    c->anim.accum_ms += dt_ms;
    while (c->anim.accum_ms >= KF_ANIM_FRAME_MS) {
        c->anim.accum_ms -= KF_ANIM_FRAME_MS;
        c->anim.frame = static_cast<uint16_t>(c->anim.frame + 1u);
    }
}

void kf_creature_anim_wrap(kf_creature *c, uint16_t frame_count) {
    if (c == nullptr) { return; }
    if (frame_count == 0u || c->anim.frame >= frame_count) {
        c->anim.frame = 0u;
    }
}
```

`kf_creature_tick_anim()` lets `frame` run unbounded on purpose — it does not
know how many frames the current sprite has, and teaching it would mean handing
Core's wander a sprite pointer for no reason. The screen wraps it against the
resolved sprite, every frame, which is where that knowledge already lives.

Call it first thing in `kf_creature_update()`, **before** the dwell early
return, so a dwelling creature still animates:

```cpp
void kf_creature_update(kf_creature *c, kf_rect field, uint32_t dt_ms) {
    if (c == nullptr || dt_ms == 0u) { return; }
    kf_creature_tick_anim(c, dt_ms);
    /* ... existing body, unchanged ... */
```

and zero the cursor in `kf_creature_init()`: `c->anim.accum_ms = 0u;
c->anim.frame = 0u;`.

- [ ] **Step 5: Wire the screen**

In `simulator/src/pet/kf_creature_screen.cpp`:

- In the egg branch, beside `g_egg_bob_elapsed_ms += dt_ms;`, add
  `kf_creature_tick_anim(&g_creature, dt_ms);` with a comment pointing at
  `kf_creature_tick_anim()`'s own explanation of why the egg needs the direct
  call.
- In `resolve_sprite()`, on the cache-miss path (after the `strcmp` fails), add
  `g_creature.anim.frame = 0u; g_creature.anim.accum_ms = 0u;` — a different
  sprite starts its own cycle from the beginning.
- Before drawing, clamp against the resolved sprite and draw the frame:

```cpp
    if (sprite != nullptr) {
        kf_creature_anim_wrap(&g_creature, sprite->frame_count);
        if (mirrored) {
            kf_blit_frame_mirrored(sprite, now.x0, now.y0, g_creature.anim.frame);
        } else {
            kf_blit_frame(sprite, now.x0, now.y0, g_creature.anim.frame);
        }
    } else {
```

- Add the debug accessor to `kf_creature_screen.h`/`.cpp`, beside the existing
  ones:

```cpp
uint16_t kf_creature_screen_debug_anim_frame(void) {
    return g_creature.anim.frame;
}
```

- [ ] **Step 6: Register and run**

```cmake
    add_test(NAME creature_anim_check
             COMMAND kamiframe-headless --verify-creature-anim)
```

Run: `cmake --build build -j8 && ctest --test-dir build -R creature_anim_check --output-on-failure`
Expected: PASS — `creature-anim: cursor keeps its own clock, budget unmoved`.

- [ ] **Step 7: Run everything**

Run: `bash dev.sh test`
Expected: **36/36**, with the two golden checksums still untouched and
`run_creature_screen_check()`'s `worst_rects <= 2` / `worst_bytes <= 13824`
still holding. Note that `run_creature_screen_sprite_check()` drives every frame
with `dt_ms == 0`, so its cursor never moves and its pixel sampling is unchanged
by this task — check its comment still tells the truth after the edit and extend
it if not.

- [ ] **Step 8: Look at it**

Run: `bash dev.sh run`
Expected: the creature still walks, still bobs as an egg, and nothing flickers.
With today's one-frame art the animation is invisible — that is correct, and it
is the point: `frame_count == 1` means frame 0 every time. The fixture pack's
`test_sprite_anim` is what proves motion until nine-frame art exists.

- [ ] **Step 9: Commit**

```bash
git add hakoniwaos/include/kf/creature.h hakoniwaos/src/creature.cpp \
        simulator/src/pet/kf_creature_screen.cpp \
        simulator/src/pet/kf_creature_screen.h \
        simulator/src/headless/headless_main.cpp simulator/CMakeLists.txt
git commit -m "Frames play on their own clock, and the egg animates too"
```

---

## What this plan deliberately does not do

Each of these is separable, and three of them are named here specifically so
nobody mistakes them for oversights.

- **It does not generate nine-frame art.** The manifest's `default_frames` stays
  `1` and every entity still ships one frame per state. This plan builds the
  pipeline that can carry nine and proves it with a synthetic three-frame
  fixture; actually drawing 379 x 9 = 3,411 frames is a large, separate
  generation spend against `tools/kf_generate_sprites.py`, and the moment it
  happens is one `frames = 9` line in `character_manifest.toml` plus the PNGs.
  Nothing in the code changes.

- **It does not remove `KF_ASSET_TYPE_SPRITE`.** Raw RGB565 stays supported, and
  that is a decision rather than an omission. An un-keyed RGB565 row is a
  `memcpy` at an assumed 100 px/µs; an indexed row can never be one and costs an
  assumed 25. For a full-screen opaque background that is a 3ms difference per
  frame out of 33. Deleting a genuinely cheaper cost shape to make the codebase
  look tidier would be a regression, and third-party game packs (the whole point
  of the SDK) should get to choose. The reader carries one extra `else if`; the
  blitter carries one branch per call.

- **It does not deduplicate frames.** Walk cycles ping-pong, so nine frames are
  often five unique ones plus a sequence, and that would cut storage again. Out
  of scope now for two reasons. First, there is nothing to measure: every sprite
  in the repo is one frame, so any dedup ratio quoted today would be invented.
  Second, it trades the format's best property away — "frame *k* is at `base + k
  * w * h`" is a multiply with no table and no second indirection, which is
  exactly what makes `esp_partition_mmap()`'d flash cheap to read. A sequence
  table is a second thing the packer and the reader must agree on forever. When
  real animation exists and the saving can be measured, it fits without a
  version bump: a flags bit plus a sequence table at the front of the payload,
  or one more `kf_asset_type`.

- **It does not raise `KF_ASSETS_MAX_ENTRIES`.** It was 64 as this plan was
  originally written -- since raised; see the addendum immediately below for
  the current number and where to find it. The creature pack uses 49, and this
  plan leaves it at 49 — collapsing frames into entries is what stops nine-frame
  art from needing 441. But **the full 379-entry roster will not fit**, and that
  is a hard `KF_ASSERT` at mount, not a graceful degradation. Raising it is
  cheap and safe when the time comes: at ~92 bytes per row after this plan's
  `kf_sprite` growth, 379 entries is ~35 KB of the 2 MB `KF_ARENA_ASSETS`. It
  belongs in the commit that first packs a pack that needs it, per
  `kf/budget.h`'s "raising a limit is a decision of its own" rule.

  **Addendum, folded into Task 1 ahead of schedule:** a concurrent art-generation
  pass landed 45 more sprites (teen forms 1-3) while this plan was still
  in flight, taking the creature pack from 49 to 94 entries — past the 64 bound
  before this plan's own later task would have reached it. Raised to 512 in
  Task 1's commit instead of waiting: measured `sizeof(kf_sprite)` is 40 bytes
  (64-bit host) after this plan's growth, `sizeof(AssetEntry)` 96, so 512 rows
  is 48 KB of the 2 MB `KF_ARENA_ASSETS` — under 2.5%, and 512 clears the full
  379-pose roster with headroom rather than stopping at the next family added.
  See `hakoniwaos/include/kf/assets.h`'s `KF_ASSETS_MAX_ENTRIES` comment for the
  current number.

- **It does not touch the egg's squish.** `egg_bob_offset_y()`
  (`kf_creature_screen.cpp:186`) moves the egg up and down without new art, and
  its own comment says the *deforming* half needs per-frame artwork that does
  not exist. After this plan it does not need code either — it needs three or
  four squish frames in `egg_idle_s_01.png … _04.png` and `frames = 4` on
  `[stages.egg]`. Whether the positional bob then coexists with the drawn squish
  or is retired in favour of it is an art call, not a code one.

- **It does not verify anything on silicon.** ADR 0033's "Not verified" list is
  unchanged and one item on it now matters more: `esp_partition_mmap()` has
  never returned a real byte on a real ESP32-S3, and this plan grows the pack
  the mapping has to cover from 228 KB toward 7.5 MB. See "The risk this plan
  sits on top of" above for what is being assumed. Nothing here can close that
  gap without a board.
