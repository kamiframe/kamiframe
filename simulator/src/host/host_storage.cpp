/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * HAL backend: storage, host implementation.
 *
 * One file per key, in a directory created on first use. Atomicity comes
 * from the same trick every reliable config-file writer uses: write the new
 * value to a temp file, flush it to disk, then rename() over the real path.
 * rename() on every desktop OS this project targets (Windows, Linux, macOS)
 * either fully replaces the destination or does not touch it at all -- a
 * reader never observes a half-written file, which is the guarantee
 * kf/hal/storage.h promises.
 *
 * What this does NOT do: survive the host machine's own power being cut
 * mid-write, in the sense of guaranteeing the rename itself reached disk.
 * That would need fsyncing the containing directory too, which has no
 * simple portable equivalent on Windows and is a guarantee about the
 * DESKTOP's disk, not the pet's. NVS gives the real guarantee on the device;
 * this backend's job is to not lie about the part that's easy to get wrong
 * on a dev machine -- a reader seeing a torn write -- not to simulate flash
 * durability.
 */

#include "kf/hal/storage.h"

#include "kf/budget.h"
#include "kf/hal/log.h"
#include "host_storage.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#if defined(_WIN32)
#include <io.h>
#define KF_FILENO _fileno
#else
#include <unistd.h>
#define KF_FILENO fileno
#endif

namespace fs = std::filesystem;

namespace {

constexpr const char *TAG = "storage";

std::string g_dir = "kf_save";
bool g_up = false;

bool key_is_valid(const char *key) {
    if (key == nullptr) {
        return false;
    }
    const size_t len = std::strlen(key);
    if (len == 0u || len > KF_STORE_MAX_KEY_LEN) {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        const char c = key[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_';
        if (!ok) {
            return false;
        }
    }
    return true;
}

fs::path path_for(const char *key) { return fs::path(g_dir) / (std::string(key) + ".kv"); }

/* fflush() only moves data from the C library's buffer to the OS; it does
 * not ask the OS to move it to disk. This does, portably across the three
 * desktop targets. */
bool sync_file(std::FILE *f) {
#if defined(_WIN32)
    return _commit(KF_FILENO(f)) == 0;
#else
    return fsync(KF_FILENO(f)) == 0;
#endif
}

} // namespace

/* Simulator-private: see host_storage.h. */
void kf_host_storage_set_dir(const char *path) {
    if (path != nullptr) {
        g_dir = path;
    }
}

kf_result kf_store_init(void) {
    std::error_code ec;
    fs::create_directories(g_dir, ec);
    if (ec) {
        KF_LOGE(TAG, "could not create save directory '%s': %s", g_dir.c_str(),
                ec.message().c_str());
        return KF_ERR_IO;
    }
    KF_LOGI(TAG, "save directory '%s'", g_dir.c_str());
    g_up = true;
    return KF_OK;
}

kf_result kf_store_write(const char *key, const void *data, size_t bytes) {
    if (!g_up || !key_is_valid(key) || (data == nullptr && bytes > 0u)) {
        return KF_ERR_INVALID;
    }
    if (bytes > KF_STORE_MAX_VALUE_BYTES) {
        return KF_ERR_INVALID;
    }

    const fs::path final_path = path_for(key);
    const fs::path tmp_path = fs::path(g_dir) / (std::string(key) + ".kv.tmp");

    std::FILE *f = std::fopen(tmp_path.string().c_str(), "wb");
    if (f == nullptr) {
        KF_LOGE(TAG, "could not open '%s' for write", tmp_path.string().c_str());
        return KF_ERR_IO;
    }

    bool ok = true;
    if (bytes > 0u) {
        ok = std::fwrite(data, 1, bytes, f) == bytes;
    }
    ok = ok && std::fflush(f) == 0;
    ok = ok && sync_file(f);
    std::fclose(f);

    if (!ok) {
        KF_LOGE(TAG, "write to '%s' failed", tmp_path.string().c_str());
        std::error_code rm_ec;
        fs::remove(tmp_path, rm_ec);
        return KF_ERR_IO;
    }

    std::error_code ec;
    fs::rename(tmp_path, final_path, ec);
    if (ec) {
        KF_LOGE(TAG, "could not commit '%s': %s", final_path.string().c_str(),
                ec.message().c_str());
        return KF_ERR_IO;
    }
    return KF_OK;
}

kf_result kf_store_read(const char *key, void *out, size_t max_bytes,
                         size_t *out_bytes) {
    if (out_bytes != nullptr) {
        *out_bytes = 0u;
    }
    if (!g_up || !key_is_valid(key) || out_bytes == nullptr) {
        return KF_ERR_INVALID;
    }

    const fs::path final_path = path_for(key);
    std::error_code exists_ec;
    if (!fs::exists(final_path, exists_ec)) {
        return KF_ERR_UNAVAILABLE;
    }

    std::error_code size_ec;
    const uintmax_t size = fs::file_size(final_path, size_ec);
    if (size_ec) {
        return KF_ERR_IO;
    }
    *out_bytes = static_cast<size_t>(size);

    if (static_cast<size_t>(size) > max_bytes) {
        return KF_ERR_INVALID; /* caller's buffer is too small; size is set */
    }

    std::FILE *f = std::fopen(final_path.string().c_str(), "rb");
    if (f == nullptr) {
        return KF_ERR_IO;
    }
    const size_t read = size > 0u ? std::fread(out, 1, static_cast<size_t>(size), f) : 0u;
    std::fclose(f);

    if (read != static_cast<size_t>(size)) {
        return KF_ERR_IO;
    }
    return KF_OK;
}

kf_result kf_store_erase(const char *key) {
    if (!g_up || !key_is_valid(key)) {
        return KF_ERR_INVALID;
    }
    std::error_code ec;
    fs::remove(path_for(key), ec);
    /* Not an error if it never existed: fs::remove returning false because
     * the file was already absent is exactly what "not an error" means. */
    return ec ? KF_ERR_IO : KF_OK;
}

void kf_store_shutdown(void) { g_up = false; }
