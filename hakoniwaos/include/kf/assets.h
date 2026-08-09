/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * Bulk read-only asset loading: entries packed by tools/kf_pack_assets.py
 * into one ".kfpack" file, mounted by kf/hal/assets.h (a memory-mapped
 * flash partition on the device, a loaded file on desktop) and parsed here,
 * in core, so the pack format lives in exactly one C++ reader regardless of
 * which backend supplied the bytes.
 *
 * MORE THAN ONE ASSET TYPE, ON PURPOSE, FROM THE FIRST VERSION: sprites are
 * the only type this file actually loads today, but the board's planned
 * MAX98357A I2S amplifier means PCM sound effects are coming later, and
 * they want exactly the same treatment sprites do -- packed into flash,
 * named, memory-mapped, loaded identically on both backends. Every
 * directory entry therefore carries a kf_asset_type tag (see below) and the
 * DIRECTORY WALK ITSELF -- bounds-checking name/data_offset/data_bytes,
 * building the table kf_assets_init() keeps in KF_ARENA_ASSETS -- does not
 * assume "asset" means "sprite": it validates every entry the same way
 * regardless of type, and only the last step (decoding the type-specific
 * metadata into something a caller can use) branches on kf_asset_type. A
 * future `kf_assets_get_clip()` reads the identical table this file already
 * builds; it does not need a second pack format, a second loader, or a
 * second parallel pipeline -- see KF_ASSET_TYPE_AUDIO_CLIP below for the
 * shape it will use, and tools/kf_pack_assets.py's header comment for the
 * matching detail on the packer side.
 *
 * WHERE THE PAYLOAD BYTES LIVE, AND WHY KF_ARENA_ASSETS STAYS SMALL: a
 * sprite's `pixels` pointer (kf/types.h's kf_sprite) points straight into
 * whatever kf_hal_assets_base() returned -- mapped flash on the device, a
 * loaded file buffer on desktop -- and is NEVER copied into
 * KF_ARENA_ASSETS. That arena (2MB of PSRAM, kf/budget.h) could not hold
 * the flash asset budget (many megabytes) even once, and copying would be
 * pure waste when the device can already address the bytes where they sit.
 * The same will be true of audio clips once they exist: a PCM buffer is
 * exactly as unsuitable for a PSRAM copy as a sprite's pixels are. What
 * DOES go through KF_ARENA_ASSETS is the small, bounded directory table
 * this file builds once at init -- "decoded sprites and game data" is
 * exactly what kf/arena.h's own comment on that arena already says it is
 * for.
 *
 * LOOKUP: kf_assets_get() does a plain linear string comparison over the
 * directory table, not a hash. See tools/kf_pack_assets.py's own header
 * comment for the full reasoning -- in short, real packs are small (tens of
 * entries) and looked up once at load time, never per frame, so an O(n)
 * scan costs nothing that matters and avoids a hash algorithm that would
 * have to agree, forever, between this file and the Python packer.
 *
 * Valid C++ (not the HAL boundary -- kf/hal/assets.h is), same as kf/pet.h,
 * kf/blit.h and the rest of core.
 */

#ifndef KF_ASSETS_H
#define KF_ASSETS_H

#include "kf/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bound on how many directory entries kf_assets_init() will build a table
 * row for, of ANY type -- sprites and, eventually, audio clips share this
 * one limit rather than each getting their own.
 *
 * 512: the full creature roster is 379 poses (one directory entry per
 * pose, not per animation frame -- an animation's frames are stored
 * contiguously inside its one entry's payload, so nine-frame art does not
 * multiply this count), and 512 clears that with room rather than needing
 * to be raised again as families keep getting added. Measured, not
 * assumed, against the animated-indexed-sprites plan's kf_sprite growth
 * (frame_count, palette, indices -- landing in the commit right after this
 * one): sizeof(kf_sprite) becomes 40 bytes and sizeof(AssetEntry) -- the
 * private row type in hakoniwaos/src/assets.cpp -- becomes 96 bytes
 * (64-bit host; the ESP32 target's 32-bit pointers make every row
 * smaller, so this is the conservative figure), so 512 rows is
 * 512 * 96 = 49,152 bytes, 48KB of the 2MB KF_ARENA_ASSETS (kf/budget.h)
 * -- under 2.5%. Re-measure sizeof(kf_sprite) rather than re-deriving this
 * by hand if either figure changes again. This was previously 64, sized
 * for a single test sprite; it had to be raised before that growth even
 * landed because a 94-entry creature pack already exceeded it. */
#define KF_ASSETS_MAX_ENTRIES 512

/* What kind of payload a directory entry holds. Matches
 * tools/kf_pack_assets.py's ASSET_TYPE_* constants exactly -- see that
 * file's format comment for the on-disk byte value, which this enum's
 * values are defined to equal. */
typedef enum {
    /* RGB565 pixels, the kf_sprite shape kf_assets_get() already returns.
     * The only type this file actually builds a usable view for today. */
    KF_ASSET_TYPE_SPRITE = 0,

    /* RESERVED, NOT YET LOADED. No kf_assets_get_clip() exists yet -- this
     * value exists so the directory format does not need to change shape
     * when one is added. Intended shape, so the packer and the eventual
     * loader agree on it in advance: 16-bit signed PCM, mono, a modest
     * sample rate (around 22kHz -- enough for short sound effects, not
     * music), uncompressed. Uncompressed on purpose: decoding MP3 or
     * similar per playback costs CPU and RAM a one-shot effect clip does
     * not justify, the same "measure the real cost, do not pay for
     * generality nothing here needs" reasoning kf/budget.h applies
     * everywhere else. */
    KF_ASSET_TYPE_AUDIO_CLIP = 1
} kf_asset_type;

/* Bring the asset pipeline up: mounts the pack via kf/hal/assets.h, checks
 * its size against KF_FLASH_ASSET_BUDGET_BYTES (kf/budget.h) -- the one
 * constraint check that belongs to core rather than either backend, same
 * reasoning as every other budget check in this codebase -- validates the
 * header, and walks the directory (every entry, regardless of type -- see
 * this header's own comment on why the walk itself is type-agnostic),
 * building a table in KF_ARENA_ASSETS.
 *
 * Panics (KF_PANIC, via KF_ASSERT) on a missing pack, a pack over budget, a
 * bad magic/version, a corrupt directory, or more entries than
 * KF_ASSETS_MAX_ENTRIES. There is no degraded mode for a missing asset
 * pack this early, the same call kf_app_init() already makes for a missing
 * display or storage backend. */
kf_result kf_assets_init(void);

/* Look up a SPRITE by name (plain C string, compared against the pack's
 * stored names -- see this header's own comment on why a scan, not a
 * hash). Returns NULL if no entry matches exactly, OR if an entry with
 * that name exists but is not KF_ASSET_TYPE_SPRITE (a future
 * kf_assets_get_clip() would be the equivalent call for
 * KF_ASSET_TYPE_AUDIO_CLIP -- deliberately no single "any type" getter, so
 * the return type a caller gets always matches what they asked for).
 *
 * The returned pointer is valid for the remainder of the program: it
 * points into a permanent (KF_ARENA_ASSETS) table row whose own `pixels`
 * field points into the mounted pack, which is never unmounted before
 * kf_assets_shutdown(). Safe to cache once and reuse every frame -- exactly
 * what kf/demo.cpp does, calling this once at init rather than per draw. */
const kf_sprite *kf_assets_get(const char *name);

void kf_assets_shutdown(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KF_ASSETS_H */
