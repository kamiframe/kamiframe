/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * Simulator-private control for the host storage backend.
 *
 * NOT part of the HAL. Core cannot see this header, the same way it cannot
 * see host_time.h: which directory a save file lives in is a desktop
 * development concern, not something game code should be able to reach.
 */

#ifndef KF_HOST_STORAGE_H
#define KF_HOST_STORAGE_H

/* Where kf_store_* reads and writes on disk. Defaults to "kf_save" next to
 * the process's current directory if never called -- which is what
 * kamiframe-sim uses with zero configuration (see BUILDING.md).
 *
 * kamiframe-headless's automated tests call this to point each run at its
 * own throwaway directory, so a test run is hermetic: it never reads a
 * leftover save from a previous run, and it never leaves one behind for a
 * developer to find and wonder about.
 *
 * Call before kf_store_init(); has no effect once the store is up. */
void kf_host_storage_set_dir(const char *path);

#endif /* KF_HOST_STORAGE_H */
