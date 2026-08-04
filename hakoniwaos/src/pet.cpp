/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * See kf/pet.h for what this is. Two things worth knowing before reading
 * the functions below:
 *
 * All decay/care maths is integer, in millipercent (0..100000), and every
 * intermediate product is computed in uint64_t before being clamped back
 * down -- decay_rate_mp_per_hour (up to a few thousand) times
 * elapsed_seconds (which for offline fast-forward can genuinely be weeks)
 * would overflow uint32_t; it cannot overflow uint64_t for any elapsed
 * time this project will ever see.
 *
 * The on-disk save format is packed and unpacked byte by byte
 * (put_u32/get_u32/put_i64/get_i64 below), not written as a raw
 * `kf_store_write(state, sizeof(*state))`. A C++ struct's layout --
 * padding, member order in memory -- is not something two different
 * compilers (this project builds with both GCC and MSVC, see the CI
 * matrix) are obliged to agree on, and a save file that only round-trips
 * on the compiler that wrote it is exactly the kind of lie
 * kf/budget.h's own header comment warns about, just in a different
 * place. Same reasoning app.cpp's hand-rolled HUD string building
 * already applies to a different problem.
 */

#include "kf/pet.h"

#include "kf/hal/log.h"
#include "kf/hal/storage.h"
#include "kf/hal/time.h"

#include <cstdint>

#include "kf/poison.h"

namespace {

constexpr const char *TAG = "pet";

/* Save format version. Bump this and kf_pet_deserialize() refuses to load
 * anything written by a different version rather than guessing at a
 * layout that changed -- see unpack() below. */
constexpr uint8_t kSaveVersion = 1;

/* +25.000% per care action. Illustrative, like kf_pet_default_config()'s
 * decay rates -- there is no real pet yet to tune either against. */
constexpr kf_pet_millipercent kCareBoostMp = 25000u;

kf_pet_millipercent clamp_add(kf_pet_millipercent value,
                               kf_pet_millipercent add) {
    const uint64_t sum = static_cast<uint64_t>(value) + add;
    return sum > KF_PET_MILLIPERCENT_MAX
               ? KF_PET_MILLIPERCENT_MAX
               : static_cast<kf_pet_millipercent>(sum);
}

kf_pet_millipercent apply_decay(kf_pet_millipercent value,
                                 uint32_t rate_mp_per_hour,
                                 uint32_t elapsed_seconds) {
    const uint64_t delta = static_cast<uint64_t>(rate_mp_per_hour) *
                            static_cast<uint64_t>(elapsed_seconds) / 3600ull;
    if (delta >= value) {
        return 0u;
    }
    return static_cast<kf_pet_millipercent>(value - delta);
}

/* -----------------------------------------------------------------------
 * Hand-packed serialisation. See the file header comment for why.
 * ----------------------------------------------------------------------- */

void put_u8(uint8_t *buf, size_t &offset, uint8_t v) { buf[offset++] = v; }

void put_u32(uint8_t *buf, size_t &offset, uint32_t v) {
    buf[offset++] = static_cast<uint8_t>(v & 0xFFu);
    buf[offset++] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    buf[offset++] = static_cast<uint8_t>((v >> 16) & 0xFFu);
    buf[offset++] = static_cast<uint8_t>((v >> 24) & 0xFFu);
}

void put_i64(uint8_t *buf, size_t &offset, int64_t v) {
    const uint64_t u = static_cast<uint64_t>(v);
    for (int i = 0; i < 8; ++i) {
        buf[offset++] = static_cast<uint8_t>((u >> (8 * i)) & 0xFFu);
    }
}

uint8_t get_u8(const uint8_t *buf, size_t &offset) { return buf[offset++]; }

uint32_t get_u32(const uint8_t *buf, size_t &offset) {
    const uint32_t v = static_cast<uint32_t>(buf[offset]) |
                        (static_cast<uint32_t>(buf[offset + 1]) << 8) |
                        (static_cast<uint32_t>(buf[offset + 2]) << 16) |
                        (static_cast<uint32_t>(buf[offset + 3]) << 24);
    offset += 4;
    return v;
}

int64_t get_i64(const uint8_t *buf, size_t &offset) {
    uint64_t u = 0;
    for (unsigned i = 0; i < 8; ++i) {
        u |= static_cast<uint64_t>(buf[offset + i]) << (8u * i);
    }
    offset += 8;
    return static_cast<int64_t>(u);
}

void pack(const kf_pet_state *state, uint8_t out[KF_PET_SAVE_BYTES]) {
    size_t off = 0;
    put_u8(out, off, kSaveVersion);
    put_u32(out, off, state->hunger_mp);
    put_u32(out, off, state->happiness_mp);
    put_u32(out, off, state->energy_mp);
    put_u8(out, off, state->last_advanced.valid ? 1u : 0u);
    put_i64(out, off, state->last_advanced.epoch_seconds);
    KF_ASSERT(off == KF_PET_SAVE_BYTES,
              "kf_pet: pack() wrote %zu bytes, KF_PET_SAVE_BYTES says %u -- "
              "the two drifted apart, fix kf/pet.h",
              off, KF_PET_SAVE_BYTES);
}

/* Returns false (and logs why) rather than partially populating `state`
 * on anything unexpected: a rejected load falls back to
 * kf_pet_load_and_advance()'s "no save" path, a fresh pet, which is
 * always a safe, well-defined state to be in. Silently accepting a
 * corrupt or foreign-version save and running with whatever garbage came
 * out would not be. */
bool unpack(const uint8_t *in, size_t in_bytes, kf_pet_state *state) {
    if (in_bytes != KF_PET_SAVE_BYTES) {
        KF_LOGE(TAG,
                "save is %zu bytes, expected exactly %u -- refusing to "
                "load a save from an incompatible version",
                in_bytes, KF_PET_SAVE_BYTES);
        return false;
    }
    size_t off = 0;
    const uint8_t version = get_u8(in, off);
    if (version != kSaveVersion) {
        KF_LOGE(TAG,
                "save version %u, this build understands version %u -- "
                "refusing to load",
                version, kSaveVersion);
        return false;
    }
    state->hunger_mp = get_u32(in, off);
    state->happiness_mp = get_u32(in, off);
    state->energy_mp = get_u32(in, off);
    state->last_advanced.valid = get_u8(in, off) != 0u;
    state->last_advanced.epoch_seconds = get_i64(in, off);
    return true;
}

} // namespace

kf_pet_config kf_pet_default_config(void) {
    kf_pet_config c{};
    c.hunger_decay_mp_per_hour = 1042u;    /* ~4.0 days, full to empty */
    c.happiness_decay_mp_per_hour = 694u;  /* ~6.0 days */
    c.energy_decay_mp_per_hour = 521u;     /* ~8.0 days */
    return c;
}

void kf_pet_init(kf_pet_state *state) {
    state->hunger_mp = KF_PET_MILLIPERCENT_MAX;
    state->happiness_mp = KF_PET_MILLIPERCENT_MAX;
    state->energy_mp = KF_PET_MILLIPERCENT_MAX;
    state->last_advanced.valid = false;
    state->last_advanced.epoch_seconds = 0;
}

void kf_pet_advance(kf_pet_state *state, const kf_pet_config *config,
                     uint32_t elapsed_seconds) {
    state->hunger_mp = apply_decay(state->hunger_mp,
                                    config->hunger_decay_mp_per_hour,
                                    elapsed_seconds);
    state->happiness_mp = apply_decay(state->happiness_mp,
                                       config->happiness_decay_mp_per_hour,
                                       elapsed_seconds);
    state->energy_mp = apply_decay(state->energy_mp,
                                    config->energy_decay_mp_per_hour,
                                    elapsed_seconds);
}

void kf_pet_feed(kf_pet_state *state) {
    state->hunger_mp = clamp_add(state->hunger_mp, kCareBoostMp);
}

void kf_pet_play(kf_pet_state *state) {
    state->happiness_mp = clamp_add(state->happiness_mp, kCareBoostMp);
}

void kf_pet_rest(kf_pet_state *state) {
    state->energy_mp = clamp_add(state->energy_mp, kCareBoostMp);
}

kf_result kf_pet_save(const kf_pet_state *state) {
    uint8_t buf[KF_PET_SAVE_BYTES];
    pack(state, buf);
    return kf_store_write(KF_PET_SAVE_KEY, buf, sizeof(buf));
}

kf_result kf_pet_load_and_advance(kf_pet_state *state,
                                   const kf_pet_config *config) {
    uint8_t buf[KF_PET_SAVE_BYTES];
    size_t out_bytes = 0;
    const kf_result read_result =
        kf_store_read(KF_PET_SAVE_KEY, buf, sizeof(buf), &out_bytes);

    if (read_result == KF_ERR_UNAVAILABLE) {
        KF_LOGI(TAG, "no save found, starting a fresh pet");
        kf_pet_init(state);
    } else if (read_result != KF_OK) {
        return read_result;
    } else if (!unpack(buf, out_bytes, state)) {
        return KF_ERR_INVALID;
    }

    const kf_wall_time now = kf_time_wall();
    if (!now.valid) {
        KF_LOGW(TAG, "wall clock not set yet -- skipping offline "
                     "fast-forward this call, will try again once it is");
        return KF_OK;
    }

    if (state->last_advanced.valid) {
        int64_t elapsed =
            now.epoch_seconds - state->last_advanced.epoch_seconds;
        if (elapsed < 0) {
            KF_LOGW(TAG,
                    "wall clock moved backwards by %lld seconds since the "
                    "last save (RTC reset, or clock set back?) -- not "
                    "ageing the pet negatively",
                    static_cast<long long>(-elapsed));
            elapsed = 0;
        }
        /* Defensive cap, not a realistic one: 0xFFFFFFFF seconds is about
         * 136 years. Exists so the cast below is never undefined
         * behaviour, not because this project expects to see it hit. */
        constexpr int64_t kMaxElapsedSeconds = 0xFFFFFFFFLL;
        const uint32_t elapsed_seconds = static_cast<uint32_t>(
            elapsed > kMaxElapsedSeconds ? kMaxElapsedSeconds : elapsed);
        kf_pet_advance(state, config, elapsed_seconds);
    }
    /* else: nothing to fast-forward FROM (a fresh pet, or a save whose
     * last_advanced was never valid) -- just adopt `now` as the new
     * baseline below. */

    state->last_advanced = now;
    return KF_OK;
}
