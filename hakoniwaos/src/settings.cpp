/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * See kf/settings.h. A deliberately tiny mirror of kf/pet.cpp's own pack()/
 * unpack()/save/load shape, scaled down to the one byte this file actually
 * has to persist so far.
 */

#include "kf/settings.h"

#include "kf/hal/audio.h"
#include "kf/hal/log.h"
#include "kf/hal/storage.h"

namespace {

constexpr const char *TAG = "settings";

/* Bumped whenever the layout below changes -- see kf/pet.h's own kSaveVersion
 * comment for why a version byte, not a struct size guess, is what decides
 * whether a stored blob is trusted. */
constexpr uint8_t kSaveVersion = 1;

void pack(const kf_settings *settings, uint8_t out[KF_SETTINGS_SAVE_BYTES]) {
    out[0] = kSaveVersion;
    out[1] = settings->volume;
}

/* Returns false (and logs why) rather than partially populating `settings`
 * on anything unexpected -- kf_settings_load() falls back to kf_settings_
 * default() on false, exactly kf_pet_load_and_advance()'s own "a rejected
 * load falls back to a safe, well-defined state" contract (kf/pet.cpp). */
bool unpack(const uint8_t *in, size_t in_bytes, kf_settings *settings) {
    if (in_bytes != KF_SETTINGS_SAVE_BYTES) {
        KF_LOGE(TAG,
                "settings save is %zu bytes, expected exactly %u -- "
                "refusing to load a save from an incompatible version",
                in_bytes, KF_SETTINGS_SAVE_BYTES);
        return false;
    }
    if (in[0] != kSaveVersion) {
        KF_LOGE(TAG,
                "settings save version %u, this build understands version "
                "%u -- refusing to load",
                in[0], kSaveVersion);
        return false;
    }
    /* KF_VOLUME_4 (kf/hal/audio.h) is the highest valid value -- a byte
     * outside [0, 4] is a corrupt store, not a volume level, and should
     * fall back to the default the same as any other unpack() rejection
     * here, not be silently clamped into range. */
    if (in[1] > static_cast<uint8_t>(KF_VOLUME_4)) {
        KF_LOGE(TAG, "settings save has an invalid volume byte (%u) -- "
                     "refusing to load",
                in[1]);
        return false;
    }
    settings->volume = in[1];
    return true;
}

} // namespace

kf_settings kf_settings_default(void) {
    kf_settings s{};
    s.volume = static_cast<uint8_t>(KF_SETTINGS_DEFAULT_VOLUME);
    return s;
}

kf_result kf_settings_save(const kf_settings *settings) {
    uint8_t buf[KF_SETTINGS_SAVE_BYTES];
    pack(settings, buf);
    return kf_store_write(KF_SETTINGS_SAVE_KEY, buf, sizeof(buf));
}

kf_result kf_settings_load(kf_settings *settings) {
    uint8_t buf[KF_SETTINGS_SAVE_BYTES];
    size_t out_bytes = 0;
    const kf_result read_result =
        kf_store_read(KF_SETTINGS_SAVE_KEY, buf, sizeof(buf), &out_bytes);

    if (read_result == KF_ERR_UNAVAILABLE) {
        KF_LOGI(TAG, "no settings save found, using defaults");
        *settings = kf_settings_default();
    } else if (read_result != KF_OK) {
        return read_result;
    } else if (!unpack(buf, out_bytes, settings)) {
        KF_LOGI(TAG, "settings save could not be loaded, using defaults");
        *settings = kf_settings_default();
    }
    return KF_OK;
}
