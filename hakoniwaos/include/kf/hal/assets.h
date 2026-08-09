/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * HAL: assets (bulk read-only data -- sprite packs, and eventually sound
 * and level data).
 *
 * The opposite shape from kf/hal/storage.h's key-value store, which named
 * this split before either half existed: assets are large (bounded by
 * KF_FLASH_ASSET_BUDGET_BYTES in kf/budget.h -- see
 * ports/esp32/partitions.csv for where that lives on the device), static
 * (packed once by tools/kf_pack_assets.py, never written at runtime), and
 * want to be addressed directly rather than copied. Save state is the
 * opposite of all three.
 *
 * What this owns: getting the packed asset file's raw bytes into an address
 * space core can read from -- esp_partition_mmap() on the device (a
 * pointer straight into flash, zero copy), a loaded file buffer on
 * desktop. What this does NOT own: the pack format itself. kf/assets.h
 * (core, not HAL) parses the header and directory this HAL hands it, so the
 * format lives in exactly one C++ reader and one Python writer
 * (tools/kf_pack_assets.py) -- the same split kf/arena.h (core) draws
 * against kf/hal/memory.h (this HAL's sibling): this HAL gets a block of
 * bytes with the right physical properties, core decides what is inside it.
 *
 * Valid C.
 */

#ifndef KF_HAL_ASSETS_H
#define KF_HAL_ASSETS_H

#include "kf/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KF_HAL_ASSETS_VERSION 1

/* The flash partition label the device backend mounts. Named here, not
 * buried inside esp_assets.cpp, so ports/esp32/partitions.csv's own
 * "assets" row and the code that looks it up cannot silently drift apart --
 * both sides read this string, one from C++, one by eye against the CSV. */
#define KF_HAL_ASSETS_PARTITION_LABEL "assets"

/* Mount the asset pack.
 *
 * `pack_path` is a backend HINT, not a contract every backend honours: the
 * desktop backend opens exactly that path if non-NULL/non-empty, or its own
 * built-in default otherwise (see host_assets.cpp). The device backend
 * ignores it completely -- on-device there is no filesystem path to give,
 * only the one partition named KF_HAL_ASSETS_PARTITION_LABEL, so it always
 * mounts that regardless of what is passed. Passing NULL is the ordinary
 * case on both backends; kf_assets_init() in kf/assets.h never passes
 * anything else.
 *
 * KF_ERR_IO if the pack could not be found, opened, or mapped. Does not
 * validate the pack's CONTENTS -- magic, version, directory bounds -- that
 * is kf/assets.h's job, since the format belongs to core, not this HAL. */
kf_result kf_hal_assets_mount(const char *pack_path);

/* The mounted pack's bytes, and how many of them there are. NULL / 0 before
 * kf_hal_assets_mount() has succeeded.
 *
 * On the device this points directly into memory-mapped flash: reads
 * through it go through the CPU's normal flash cache, the same path
 * instruction fetches use, and are safe from any task or ISR context that
 * can safely execute code from flash at all -- which on this chip is
 * anywhere the blitter runs (kf/blit.cpp is never called from an ISR).
 * See docs/architecture/adr-0033-asset-pipeline.md's "Verified" section.
 * On desktop it points at a loaded copy of the file, owned by this HAL.
 *
 * Either way, kf/assets.h reads pixel data straight through this pointer
 * and never copies it -- see that header's own comment on why bulk pixel
 * bytes deliberately do NOT go through KF_ARENA_ASSETS. */
const uint8_t *kf_hal_assets_base(void);
size_t kf_hal_assets_size(void);

void kf_hal_assets_unmount(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KF_HAL_ASSETS_H */
