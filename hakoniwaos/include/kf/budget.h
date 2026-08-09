/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * ============================================================================
 *  THESE NUMBERS DESCRIBE REAL HARDWARE.
 *
 *  Do not raise one to make a test pass. Do not add a build option that
 *  relaxes one. Raising a limit is a design decision and belongs in a commit
 *  of its own, with the reason in the message.
 *
 *  Desktop is fast and roomy and will lie to you about every one of these.
 *  That is the entire reason this file exists.
 * ============================================================================
 *
 * Target: ESP32-S3-WROOM-1 N16R8, 2.0-2.8" IPS TFT 240x320 (ST7789), SPI+DMA.
 * See 02-min-spec-sheet.md in the planning folder for where these come from.
 *
 * This header must remain valid C. It is included by both builds and by the
 * HAL headers.
 */

#ifndef KF_BUDGET_H
#define KF_BUDGET_H

/* -------------------------------------------------------------------------
 * Display
 * ------------------------------------------------------------------------- */

/* ST7789 native orientation is portrait. */
#define KF_DISPLAY_WIDTH   240
#define KF_DISPLAY_HEIGHT  320

/* Pixels are RGB565 held in native-endian uint16_t. Any byte swapping the
 * panel needs is the display backend's job and must never reach the core. */
#define KF_DISPLAY_BPP     16
#define KF_FRAMEBUFFER_PIXELS (KF_DISPLAY_WIDTH * KF_DISPLAY_HEIGHT)
#define KF_FRAMEBUFFER_BYTES  (KF_FRAMEBUFFER_PIXELS * 2)

/* -------------------------------------------------------------------------
 * Frame timing
 * ------------------------------------------------------------------------- */

/* 30fps is the realistic full-frame target. See KF_DISPLAY_SPI_HZ below for
 * why 60 needs partial updates rather than a faster CPU. */
#define KF_TARGET_FPS          30
#define KF_FRAME_BUDGET_US     (1000000 / KF_TARGET_FPS)

/* MEASURED, 2026-08-08, on the first real board. This was an assumption for
 * the whole of Phase 1 and is not one any more.
 *
 * Stage 2b of ports/esp32-bringup swept the panel across 4/10/20/40/80MHz,
 * redrawing a test card at each. 40MHz was the highest that rendered
 * correctly; 80MHz produced a solid white screen, which is what wholesale
 * data corruption looks like on this panel. So the figure this file already
 * guessed turns out to be right, and the ~32fps ceiling below is real rather
 * than hoped for.
 *
 * Three caveats, because "measured" should not be read as "settled":
 *
 *   1. Measured on a HiLetgo 2.8in ILI9341 -- the officially supported
 *      option, not the primary panel. The 2in ST7789 is the reference panel
 *      and has not been measured. Re-measure when it arrives; ST7789 modules
 *      commonly tolerate more, so this may go up.
 *   2. 40MHz is roughly four times the ILI9341 datasheet's own write-cycle
 *      figure. It works, and is what practically every driver for this panel
 *      does, but it is outside spec and could be marginal on a different unit
 *      or at a different temperature. It is a working number, not a
 *      guaranteed one.
 *   3. Measured on a breadboard through Dupont jumpers with inline couplers,
 *      which is close to the worst wiring this project will ever have. A real
 *      PCB should do at least this well, so treat 40MHz as a floor.
 *
 * At 40MHz a full 240x320 RGB565 frame is
 *
 *     240 * 320 * 2 bytes * 8 bits = 1,228,800 bits
 *     1,228,800 / 40,000,000       = 30.7 ms
 *
 * of wire time before the CPU has drawn anything at all. That is most of a
 * 33.3ms frame. The desktop backend reports this as zero, which is the single
 * most misleading thing about developing on a PC, so the core estimates it
 * from the dirty rectangle and includes it in every budget report.
 *
 * Raising this is the single biggest lever on full-screen animation:
 *   40 MHz SPI      30.7 ms/frame   ~32 fps ceiling
 *   60 MHz SPI      20.5 ms/frame   ~49 fps ceiling
 *   80 MHz SPI      15.4 ms/frame   ~65 fps ceiling
 *   8-bit parallel   3.8 ms/frame   far above any target (needs an i80 panel)
 * See docs/frame-budget.md. */
#define KF_DISPLAY_SPI_HZ      40000000

/* ASSUMPTION, NOT MEASURED. Correct at hardware bring-up. (KF_DISPLAY_SPI_HZ
 * above no longer carries this banner -- stage 2b measured it -- but this one
 * still does: per-rectangle overhead was never what that sweep tested.)
 *
 * Every separate rectangle sent to the panel costs a small fixed overhead
 * before its pixels: the ST7789 needs CASET (column address window, 1
 * command byte + 4 parameter bytes), RASET (row address window, 1 + 4) and
 * RAMWR (1 command byte, then the pixel burst) before it will accept a new
 * region. That is 11 bytes of protocol overhead the link has to carry the
 * same as any pixel byte, once per rectangle, not once per frame.
 *
 * This is why sending many small dirty rectangles instead of one big one is
 * not free: past some count, the addressing overhead costs more than the
 * extra pixels a slightly bigger rectangle would have sent instead. See
 * KF_MAX_DIRTY_RECTS in kf/framebuffer.h and
 * docs/architecture/adr-0011-dirty-rect-list.md. */
#define KF_DISPLAY_RECT_OVERHEAD_BYTES 11u

/* Whether the device overlaps drawing with transmission.
 *
 * 0: the CPU draws, then waits for the whole frame to go out. Frame cost is
 *    draw + transfer.
 * 1: DMA pushes the previous frame while the CPU draws the next one. Frame
 *    cost is max(draw, transfer), which for this hardware means the wire
 *    alone. Costs a second framebuffer (or band buffers) in internal SRAM.
 *
 * Currently 0, because no backend does it yet. The budget report prints BOTH
 * numbers regardless, so the headroom double buffering would buy is always
 * visible rather than something to discover later. */
#define KF_DISPLAY_DOUBLE_BUFFERED 0

/* -------------------------------------------------------------------------
 * Drawing speed
 *
 * ASSUMPTIONS, NOT MEASURED. Correct at bring-up.
 *
 * Your desktop draws pixels roughly a hundred times faster than a 240MHz
 * microcontroller, so host wall-clock time says nothing useful about the
 * device. Instead the core COUNTS pixels written and converts the count to an
 * estimated device time using the figures below. That number is
 * host-independent: it is the same whether you run the simulator on a laptop
 * or a workstation, which is what makes it worth reporting.
 *
 * Opaque fills and full-row sprite copies are memcpy-shaped and fast.
 * Colour-keyed blits test every pixel and are several times slower.
 * ------------------------------------------------------------------------- */
#define KF_DRAW_OPAQUE_PX_PER_US   100u
#define KF_DRAW_KEYED_PX_PER_US     25u

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

/* -------------------------------------------------------------------------
 * Memory
 *
 * The S3 has two pools with genuinely different properties. Your desktop has
 * one flat heap, so nothing on desktop will ever tell you which pool a
 * buffer would have landed in, or that you have exhausted the small fast one.
 * The arenas below exist to make that visible on both targets.
 * ------------------------------------------------------------------------- */

/* Internal SRAM: ~512KB total on the S3, but FreeRTOS, the WiFi stack and
 * ESP-IDF itself take a large bite. 320KB is a deliberately pessimistic
 * working figure until bring-up measures the real headroom. */
#define KF_POOL_INTERNAL_BYTES  (320u * 1024u)

/* Octal PSRAM: 8MB, noticeably slower than internal SRAM, with real
 * restrictions on DMA. */
#define KF_POOL_PSRAM_BYTES     (8u * 1024u * 1024u)

/* Arenas. Each is a fixed block carved out of a pool at startup. Exhaustion
 * is a hard failure on both targets, not a slow desktop. */

/* Framebuffer must be internal SRAM and DMA-capable. */
#define KF_ARENA_FRAMEBUFFER_BYTES  KF_FRAMEBUFFER_BYTES        /* 153,600 */

/* Per-frame scratch. Reset to empty at the top of every frame, so it costs
 * nothing to use and cannot leak or fragment. */
#define KF_ARENA_SCRATCH_BYTES      (48u * 1024u)

/* Lua heap. lua_newstate takes an allocator, so this cap is exact rather
 * than advisory: the whole block is handed to a suballocator
 * (simulator/src/lua/kf_lua_alloc.cpp) that does the actual per-object
 * alloc/realloc/free lua_newstate's callback needs, the same shape
 * KF_ARENA_LVGL below uses for LVGL's pool. See ADR 0014. */
#define KF_ARENA_LUA_BYTES          (1024u * 1024u)

/* Decoded sprites and other game assets held in RAM. */
#define KF_ARENA_ASSETS_BYTES       (2u * 1024u * 1024u)

/* LVGL's own object/style heap -- see ADR 0013. Real-world LVGL deployments
 * report needing 80-140KB; this is sized generously against that figure,
 * same "measure honestly, don't cut it close" reasoning every other arena
 * here follows, not the bare 64KB minimum LVGL itself defaults to. Not used
 * until the menu slice; declared now for the same reason KF_ARENA_LUA is:
 * so the budget arithmetic below stays honest about what has to fit. */
#define KF_ARENA_LVGL_BYTES         (256u * 1024u)

/* -------------------------------------------------------------------------
 * Flash
 * ------------------------------------------------------------------------- */

/* 16MB part. Firmware (x2, for OTA -- see ports/esp32/partitions.csv),
 * NVS and the partition table all take a share, so the asset partition is
 * smaller than the chip.
 *
 * REFINED, not just assumed, now that the partition table exists
 * (docs/architecture/adr-0033-asset-pipeline.md): 12MB is what is actually
 * left over after two 1.5MB app slots (current firmware is ~640KB; real
 * headroom for LVGL+Lua+Wifi/BLE to grow into) plus NVS/phy_init/otadata,
 * not a round number picked in isolation. This MUST equal
 * partitions.csv's "assets" partition size exactly -- kf/assets.cpp checks
 * a loaded pack against this number, but nothing checks this number against
 * the CSV itself, so a change to one needs the same commit to change the
 * other. */
#define KF_FLASH_TOTAL_BYTES        (16u * 1024u * 1024u)
#define KF_FLASH_ASSET_BUDGET_BYTES (12u * 1024u * 1024u)

/* -------------------------------------------------------------------------
 * Storage (save state)
 *
 * kf/hal/storage.h is a small key-value store, not a filesystem, because the
 * device backend is ESP-IDF's NVS (non-volatile storage) and NVS is a
 * key-value store: these two numbers are ITS real limits, not a design
 * choice made here. A desktop save file that quietly accepted a longer key
 * or a bigger value than the device ever could would be exactly the kind of
 * lie this whole header exists to prevent -- a save file that works on your
 * PC and fails on hardware the first time a contributor tries it there.
 * ------------------------------------------------------------------------- */

/* NVS keys are at most 15 bytes, not counting the NUL terminator. Fixed by
 * ESP-IDF, not tunable. */
#define KF_STORE_MAX_KEY_LEN     15u

/* ASSUMPTION, NOT MEASURED, same caveat as KF_DISPLAY_SPI_HZ above. A single
 * NVS entry fits in one 4000-byte page in the common case; larger blobs need
 * NVS's multi-page bookkeeping, which is a real device behaviour this store
 * deliberately does not paper over on desktop. A save format that stays
 * under this without needing to think about it is one that will not need
 * re-thinking at hardware bring-up. */
#define KF_STORE_MAX_VALUE_BYTES 4000u

/* -------------------------------------------------------------------------
 * Compile-time checks. These fail the build, which is the point.
 * ------------------------------------------------------------------------- */

#if defined(__cplusplus)
#define KF_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#else
#define KF_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif

KF_STATIC_ASSERT(KF_ARENA_FRAMEBUFFER_BYTES + KF_ARENA_SCRATCH_BYTES
                     <= KF_POOL_INTERNAL_BYTES,
                 "Internal SRAM arenas exceed the internal pool budget. "
                 "You cannot fit this on the device. See kf/budget.h.");

KF_STATIC_ASSERT(KF_ARENA_LUA_BYTES + KF_ARENA_ASSETS_BYTES +
                          KF_ARENA_LVGL_BYTES <=
                      KF_POOL_PSRAM_BYTES,
                 "PSRAM arenas exceed 8MB. See kf/budget.h.");

KF_STATIC_ASSERT(KF_FLASH_ASSET_BUDGET_BYTES < KF_FLASH_TOTAL_BYTES,
                 "Asset budget leaves no room for firmware. See kf/budget.h.");

KF_STATIC_ASSERT(KF_DISPLAY_WIDTH > 0 && KF_DISPLAY_HEIGHT > 0,
                 "Display dimensions must be positive.");

#endif /* KF_BUDGET_H */
