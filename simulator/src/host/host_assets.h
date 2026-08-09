/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * Simulator-private control for the host assets backend.
 *
 * NOT part of the HAL, the same way host_storage.h and host_time.h are not:
 * kf_assets_init() (hakoniwaos/src/assets.cpp) always calls
 * kf_hal_assets_mount(nullptr), so Core never chooses which pack file gets
 * mounted -- that stays a desktop development/runtime concern, picked before
 * Core ever starts.
 */

#ifndef KF_HOST_ASSETS_H
#define KF_HOST_ASSETS_H

/* Overrides the pack path kf_hal_assets_mount(nullptr) resolves to, in
 * place of the compiled-in KF_HOST_DEFAULT_ASSET_PACK_PATH (see
 * host_assets.cpp and simulator/CMakeLists.txt's KF_ASSET_PACK cache
 * variable for that default).
 *
 * Call before kf_app_init() (which is what actually mounts); has no effect
 * once the pack is up. Two callers today: kamiframe-sim's `--pack <path>`
 * flag (sdl_main.cpp), and kamiframe-headless's run_creature_screen_
 * sprite_check() (headless_main.cpp), which sets the override to its own
 * demo pack, mounts it, and restores the override to null before
 * returning -- see that function's own comment. Every other ctest target,
 * and every other check within kamiframe-headless, never calls this, so
 * they always get the compiled-in default unchanged, which is what keeps
 * their checksummed output stable: it holds because the one caller that
 * touches the override always puts it back, not because nothing else
 * could reach this function.
 *
 * A null or empty path clears any previous override, going back to the
 * compiled-in default -- the same "null/empty means default" contract
 * kf_hal_assets_mount() itself already has. */
void kf_host_assets_set_pack_path(const char *path);

#endif /* KF_HOST_ASSETS_H */
