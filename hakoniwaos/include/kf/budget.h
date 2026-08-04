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

/* ASSUMPTION, NOT MEASURED. Correct this at hardware bring-up (Phase 1b).
 *
 * ST7789 over SPI is commonly clocked at 40-80MHz on the ESP32-S3. 40MHz is
 * the conservative figure. At 40MHz a full 240x320 RGB565 frame is
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
 * than advisory. Not used until the Lua slice. */
#define KF_ARENA_LUA_BYTES          (1024u * 1024u)

/* Decoded sprites and other game assets held in RAM. */
#define KF_ARENA_ASSETS_BYTES       (2u * 1024u * 1024u)

/* -------------------------------------------------------------------------
 * Flash
 * ------------------------------------------------------------------------- */

/* 16MB part. Firmware, OTA slots, NVS and the partition table all take a
 * share, so the asset partition is smaller than the chip. Refine when the
 * partition table is written (Phase 1b). */
#define KF_FLASH_TOTAL_BYTES        (16u * 1024u * 1024u)
#define KF_FLASH_ASSET_BUDGET_BYTES (10u * 1024u * 1024u)

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

KF_STATIC_ASSERT(KF_ARENA_LUA_BYTES + KF_ARENA_ASSETS_BYTES
                     <= KF_POOL_PSRAM_BYTES,
                 "PSRAM arenas exceed 8MB. See kf/budget.h.");

KF_STATIC_ASSERT(KF_FLASH_ASSET_BUDGET_BYTES < KF_FLASH_TOTAL_BYTES,
                 "Asset budget leaves no room for firmware. See kf/budget.h.");

KF_STATIC_ASSERT(KF_DISPLAY_WIDTH > 0 && KF_DISPLAY_HEIGHT > 0,
                 "Display dimensions must be positive.");

#endif /* KF_BUDGET_H */
