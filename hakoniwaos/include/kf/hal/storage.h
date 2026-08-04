/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * HAL: storage (save state).
 *
 * A small key-value store, not a filesystem. `kf_store_*` is for what a
 * virtual pet needs to survive being switched off: small values, written
 * often, that must come back correctly even if power was cut mid-write. It
 * is NOT for bulk read-only data (sprites, sound, level data) -- that is
 * `kf/hal/assets.h`, a later header, backed by a flash partition rather than
 * a key-value store, because the two have opposite shapes: assets are large,
 * static, and want to be memory-mapped; save state is small, changing, and
 * wants atomicity.
 *
 * The device backend is ESP-IDF's NVS. `KF_STORE_MAX_KEY_LEN` and
 * `KF_STORE_MAX_VALUE_BYTES` in kf/budget.h are NVS's real limits, not a
 * design choice: a desktop backend that accepted more would let a save
 * format work here and fail the first time it runs on hardware.
 *
 * Valid C.
 */

#ifndef KF_HAL_STORAGE_H
#define KF_HAL_STORAGE_H

#include "kf/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KF_HAL_STORAGE_VERSION 1

/* Bring the store up. Idempotent to call again after kf_store_shutdown(). */
kf_result kf_store_init(void);

/* Write `bytes` under `key`, replacing any existing value under that key.
 *
 * Atomic and power-loss safe: a backend must guarantee that after this call
 * either returns or is interrupted by power loss, a subsequent
 * kf_store_read() of the same key returns either the OLD value or the NEW
 * one in full, never a partial write. That is NVS's own guarantee on the
 * device; the desktop backend earns the same guarantee itself, because nvs
 * dev boards are not the only place a save file gets tested.
 *
 * KF_ERR_INVALID if `key` is empty, longer than KF_STORE_MAX_KEY_LEN, or
 * contains anything outside `[A-Za-z0-9_]` (the safe subset every backend
 * can store, including one where the key becomes a filename). KF_ERR_INVALID
 * if `bytes` exceeds KF_STORE_MAX_VALUE_BYTES. KF_ERR_EXHAUSTED if the
 * backend's storage is full. */
kf_result kf_store_write(const char *key, const void *data, size_t bytes);

/* Read the value under `key` into `out` (capacity `max_bytes`).
 * `*out_bytes` is always set to the value's true stored size, even on
 * failure, so a caller whose buffer was too small knows how big to make the
 * next one.
 *
 * KF_ERR_UNAVAILABLE if `key` has never been written -- a fresh device, or a
 * fresh desktop save directory, both look like this, and a caller must treat
 * it as "no save yet" rather than a fault. KF_ERR_INVALID if `key` is
 * malformed (see kf_store_write) or `max_bytes` is smaller than the stored
 * value. */
kf_result kf_store_read(const char *key, void *out, size_t max_bytes,
                         size_t *out_bytes);

/* Remove a key. Not an error if it was never written. */
kf_result kf_store_erase(const char *key);

void kf_store_shutdown(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KF_HAL_STORAGE_H */
