/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * HAL backend: assets, host implementation (shared by kamiframe-sim and
 * kamiframe-headless via kamiframe_host_common, same as storage, time,
 * entropy, memory, log and power).
 *
 * Reads the whole pack file into a heap-allocated buffer once, at mount
 * time, and hands core a pointer into it. This file is a HAL backend, so it
 * is allowed to allocate -- kf/poison.h only reaches hakoniwaos/src/, never
 * simulator/src/ -- the same freedom host_storage.cpp and host_memory.cpp
 * already use.
 *
 * That buffer is deliberately NOT arena memory. On the device, an asset
 * pack's pixel bytes live in flash, memory-mapped, and never touch PSRAM;
 * loading the whole file into a plain heap block here, outside
 * KF_ARENA_ASSETS, is desktop's honest equivalent -- a block of bytes the
 * rest of the program only ever reads, addressed directly, that is not
 * charged against the same PSRAM budget the device's decoded-sprite
 * descriptor table is. kf/assets.cpp is what puts descriptors (not pixels)
 * into KF_ARENA_ASSETS, identically on both backends -- see its own header
 * comment.
 *
 * Default path: an ABSOLUTE path to the checked-in test pack in the source
 * tree, baked in at CMake configure time (KF_HOST_DEFAULT_ASSET_PACK_PATH,
 * set in simulator/CMakeLists.txt). This is deliberately NOT the
 * "zero-configuration, relative to wherever you run it" pattern
 * host_storage.cpp uses for kf_save/ -- that pattern fits a directory the
 * program creates and owns; an asset pack is a build-time, read-only,
 * source-tree-relative fixture, and a relative default would break the
 * moment a binary is launched from any working directory other than the
 * repo root, which is not a promise ctest makes (see
 * docs/architecture/adr-0033-asset-pipeline.md's "Verified" section for
 * what was actually checked). Baking the absolute path means every desktop
 * binary finds the same file regardless of where it is launched from --
 * closer to how the Lua proof scripts are baked in as header string
 * constants than to kf_save/'s own zero-config story.
 *
 * No per-test isolation hook, unlike kf_host_storage_set_dir(): loading an
 * asset pack is read-only, so every test sharing the one checked-in file is
 * safe, including running several at once -- there is nothing here for
 * concurrent tests to collide over the way concurrent writers to the same
 * save directory would.
 *
 * Runtime override, unlike the default above: kf_host_assets_set_pack_path()
 * (host_assets.h) lets kamiframe-sim's `--pack <path>` flag point an
 * already-built binary at a different .kfpack without a recompile -- see
 * that header's own comment for why this has to live here rather than
 * behind kf_hal_assets_mount()'s own `pack_path` parameter (Core always
 * calls that with nullptr; it never chooses a path itself). Storage for the
 * override is a fixed buffer, not std::string, so this file's only
 * allocation stays the pack-file read below -- an override is set once,
 * early, from a C-string argv already owns for the life of the process, so
 * there is nothing here that needs dynamic growth.
 */

#include "kf/hal/assets.h"

#include "kf/hal/log.h"
#include "host_assets.h"

#include <cstdio>
#include <cstring>

#ifndef KF_HOST_DEFAULT_ASSET_PACK_PATH
#error "KF_HOST_DEFAULT_ASSET_PACK_PATH must be defined by the build -- see simulator/CMakeLists.txt"
#endif

namespace {

constexpr const char *TAG = "assets";

uint8_t *g_buf = nullptr;
size_t g_size = 0u;
bool g_up = false;

constexpr size_t kOverridePathMax = 1024u;
char g_override_path[kOverridePathMax] = {};

} // namespace

void kf_host_assets_set_pack_path(const char *path) {
    if (path == nullptr || path[0] == '\0') {
        g_override_path[0] = '\0';
        return;
    }
    std::strncpy(g_override_path, path, kOverridePathMax - 1u);
    g_override_path[kOverridePathMax - 1u] = '\0';
}

kf_result kf_hal_assets_mount(const char *pack_path) {
    const char *path = (pack_path != nullptr && pack_path[0] != '\0')
                            ? pack_path
                        : (g_override_path[0] != '\0')
                            ? g_override_path
                            : KF_HOST_DEFAULT_ASSET_PACK_PATH;

    std::FILE *f = std::fopen(path, "rb");
    if (f == nullptr) {
        KF_LOGE(TAG, "could not open asset pack '%s'", path);
        return KF_ERR_IO;
    }

    if (std::fseek(f, 0, SEEK_END) != 0) {
        std::fclose(f);
        KF_LOGE(TAG, "could not seek asset pack '%s'", path);
        return KF_ERR_IO;
    }
    const long size = std::ftell(f);
    if (size < 0) {
        std::fclose(f);
        KF_LOGE(TAG, "could not determine size of asset pack '%s'", path);
        return KF_ERR_IO;
    }
    std::fseek(f, 0, SEEK_SET);

    uint8_t *buf = new uint8_t[static_cast<size_t>(size)];
    const size_t read =
        size > 0 ? std::fread(buf, 1, static_cast<size_t>(size), f) : 0u;
    std::fclose(f);
    if (read != static_cast<size_t>(size)) {
        KF_LOGE(TAG, "short read on asset pack '%s' (%zu of %ld bytes)", path,
                read, size);
        delete[] buf;
        return KF_ERR_IO;
    }

    g_buf = buf;
    g_size = static_cast<size_t>(size);
    g_up = true;
    KF_LOGI(TAG, "asset pack '%s' loaded: %zu bytes", path, g_size);
    return KF_OK;
}

const uint8_t *kf_hal_assets_base(void) { return g_up ? g_buf : nullptr; }

size_t kf_hal_assets_size(void) { return g_up ? g_size : 0u; }

void kf_hal_assets_unmount(void) {
    delete[] g_buf;
    g_buf = nullptr;
    g_size = 0u;
    g_up = false;
}
