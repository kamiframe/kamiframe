/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * HAL backend: storage, ESP32 implementation.
 *
 * The real thing kf/hal/storage.h's own header comment names as the device
 * backend: ESP-IDF's NVS (non-volatile storage, a wear-levelled key-value
 * store living in a dedicated flash partition). NVS is where kf/budget.h's
 * KF_STORE_MAX_KEY_LEN (15, NVS_KEY_NAME_MAX_SIZE - 1 for the NUL) and
 * KF_STORE_MAX_VALUE_BYTES actually come from -- this file does not invent
 * those limits, it is the reason they exist.
 *
 * Atomicity is NVS's own guarantee, not something this file builds: every
 * nvs_set_blob() + nvs_commit() pair either lands in full or (on power loss
 * mid-write) leaves the previous value readable, which is exactly the
 * contract kf_store_write() documents. host_storage.cpp earns this same
 * guarantee by hand (temp file + fsync + rename) because a desktop OS gives
 * it nothing for free; here it is simply true.
 */

#include "kf/hal/storage.h"

#include "kf/budget.h"
#include "kf/hal/log.h"

#include "nvs.h"
#include "nvs_flash.h"

#include <cstring>

namespace {

constexpr const char *TAG = "storage";
constexpr const char *kNamespace = "kf";

nvs_handle_t g_handle = 0;
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

} // namespace

kf_result kf_store_init(void) {
    /* The standard ESP-IDF idiom: if the NVS partition was formatted by an
     * older/incompatible layout, or is simply out of free pages after
     * enough wear, erase and reinitialise once rather than failing forever.
     * A save-state store that permanently refuses to come up after a single
     * flash hiccup is a worse failure than losing whatever was in it. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        KF_LOGW(TAG, "NVS partition needs erase (err=%d), reinitialising", err);
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        KF_LOGE(TAG, "nvs_flash_init failed: %d", err);
        return KF_ERR_IO;
    }

    err = nvs_open(kNamespace, NVS_READWRITE, &g_handle);
    if (err != ESP_OK) {
        KF_LOGE(TAG, "nvs_open('%s') failed: %d", kNamespace, err);
        return KF_ERR_IO;
    }

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

    esp_err_t err = nvs_set_blob(g_handle, key, data, bytes);
    if (err == ESP_ERR_NVS_NOT_ENOUGH_SPACE) {
        return KF_ERR_EXHAUSTED;
    }
    if (err != ESP_OK) {
        KF_LOGE(TAG, "nvs_set_blob('%s') failed: %d", key, err);
        return KF_ERR_IO;
    }

    /* nvs_set_blob() alone only stages the write; nvs_commit() is what
     * actually earns the power-loss-safe guarantee kf_store_write()
     * documents. */
    err = nvs_commit(g_handle);
    if (err != ESP_OK) {
        KF_LOGE(TAG, "nvs_commit() after write to '%s' failed: %d", key, err);
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

    /* Query the real stored length first (out=NULL is nvs_get_blob's
     * documented way of asking "how big"), so a too-small caller buffer
     * still gets told the true size, matching kf_store_read()'s own
     * documented contract. */
    size_t length = 0;
    esp_err_t err = nvs_get_blob(g_handle, key, nullptr, &length);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return KF_ERR_UNAVAILABLE;
    }
    if (err != ESP_OK) {
        KF_LOGE(TAG, "nvs_get_blob('%s') length query failed: %d", key, err);
        return KF_ERR_IO;
    }

    *out_bytes = length;
    if (length > max_bytes) {
        return KF_ERR_INVALID;
    }
    if (length == 0u) {
        return KF_OK;
    }

    err = nvs_get_blob(g_handle, key, out, &length);
    if (err != ESP_OK) {
        KF_LOGE(TAG, "nvs_get_blob('%s') read failed: %d", key, err);
        return KF_ERR_IO;
    }
    return KF_OK;
}

kf_result kf_store_erase(const char *key) {
    if (!g_up || !key_is_valid(key)) {
        return KF_ERR_INVALID;
    }

    const esp_err_t err = nvs_erase_key(g_handle, key);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        KF_LOGE(TAG, "nvs_erase_key('%s') failed: %d", key, err);
        return KF_ERR_IO;
    }
    /* Commit even on ERR_NVS_NOT_FOUND -- there is nothing to commit in that
     * case, but calling it unconditionally on the success path keeps this
     * function simple and commit on a no-op erase is harmless. */
    nvs_commit(g_handle);
    return KF_OK;
}

void kf_store_shutdown(void) {
    if (g_up) {
        nvs_close(g_handle);
        g_handle = 0;
        g_up = false;
    }
}
