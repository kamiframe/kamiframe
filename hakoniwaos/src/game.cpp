/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * See kf/game.h.
 */

#include "kf/game.h"

#include "kf/hal/log.h"

namespace {

constexpr const char *TAG = "game";

/* Byte-at-a-time little-endian packing, matching pet.cpp's own put_u16/
 * put_u32/get_u16/get_u32 helpers exactly (hakoniwaos/src/pet.cpp) --
 * duplicated here rather than shared because pet.cpp's are file-local
 * (anonymous-namespace-equivalent `static` functions, not declared in any
 * header), and this file has no more business reaching into pet.cpp than
 * pet.cpp has reaching into this one. Four lines each; not worth widening
 * either file's surface to share. */
void put_u16(uint8_t *buf, size_t &offset, uint16_t v) {
    buf[offset++] = static_cast<uint8_t>(v & 0xFFu);
    buf[offset++] = static_cast<uint8_t>((v >> 8) & 0xFFu);
}

void put_u32(uint8_t *buf, size_t &offset, uint32_t v) {
    buf[offset++] = static_cast<uint8_t>(v & 0xFFu);
    buf[offset++] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    buf[offset++] = static_cast<uint8_t>((v >> 16) & 0xFFu);
    buf[offset++] = static_cast<uint8_t>((v >> 24) & 0xFFu);
}

uint16_t get_u16(const uint8_t *buf, size_t &offset) {
    const uint16_t v = static_cast<uint16_t>(
        static_cast<uint16_t>(buf[offset]) |
        (static_cast<uint16_t>(buf[offset + 1]) << 8));
    offset += 2;
    return v;
}

uint32_t get_u32(const uint8_t *buf, size_t &offset) {
    const uint32_t v = static_cast<uint32_t>(buf[offset]) |
                        (static_cast<uint32_t>(buf[offset + 1]) << 8) |
                        (static_cast<uint32_t>(buf[offset + 2]) << 16) |
                        (static_cast<uint32_t>(buf[offset + 3]) << 24);
    offset += 4;
    return v;
}

} // namespace

void kf_game_pack(const kf_game_record *r, uint8_t out[KF_GAME_RECORD_BYTES]) {
    size_t off = 0;
    put_u16(out, off, r->plays);
    put_u32(out, off, r->best_score);
    put_u32(out, off, r->last_score);
    put_u32(out, off, r->total_score);
    put_u32(out, off, r->best_at);
    put_u16(out, off, r->last_played_day);
    out[off++] = r->streak_days;
    put_u16(out, off, r->perfect_count);
    KF_ASSERT(off == KF_GAME_RECORD_BYTES,
              "kf_game: pack() wrote %zu bytes, KF_GAME_RECORD_BYTES says "
              "%u -- the field list and the byte count drifted apart, fix "
              "kf/game.h",
              off, KF_GAME_RECORD_BYTES);
}

bool kf_game_unpack(const uint8_t *in, size_t in_bytes, kf_game_record *out) {
    if (in_bytes != KF_GAME_RECORD_BYTES) {
        KF_LOGE(TAG,
                "game record is %zu bytes, expected exactly %u -- refusing "
                "to load a record from an incompatible or corrupt source",
                in_bytes, KF_GAME_RECORD_BYTES);
        return false;
    }
    size_t off = 0;
    kf_game_record r{};
    r.plays = get_u16(in, off);
    r.best_score = get_u32(in, off);
    r.last_score = get_u32(in, off);
    r.total_score = get_u32(in, off);
    r.best_at = get_u32(in, off);
    r.last_played_day = get_u16(in, off);
    r.streak_days = in[off++];
    r.perfect_count = get_u16(in, off);
    *out = r;
    return true;
}

kf_game_tier kf_game_tier_for(uint32_t score, uint32_t good_threshold,
                               uint32_t great_threshold) {
    /* GREAT checked first: both thresholds are "at or above" (see kf/
     * game.h's own header comment), and a score that reaches great also
     * reaches good by construction whenever a manifest sets great >=
     * good, so checking good first would still give the right answer for
     * a well-formed manifest -- but checking great first means a
     * malformed one (great < good, never intended but not rejected
     * either) fails toward the MORE generous reading rather than the
     * less, which is the safer direction for a content bug to fail in. */
    if (score >= great_threshold) {
        return KF_GAME_TIER_GREAT;
    }
    if (score >= good_threshold) {
        return KF_GAME_TIER_GOOD;
    }
    return KF_GAME_TIER_MISS;
}

void kf_game_record_apply(kf_game_record *r, uint32_t score,
                           uint32_t now_epoch, uint16_t day_index) {
    /* Captured before ANY field below changes -- this is "was this the
     * very first play", which only the pre-increment value of `plays` can
     * answer. See kf/game.h's own header comment on why plays == 0 is a
     * special streak case rather than falling out of the day-gap maths
     * below (there is no previous day_index to compare against yet). */
    const bool first_play = (r->plays == 0u);
    const uint16_t previous_day = r->last_played_day;

    if (r->plays < UINT16_MAX) {
        r->plays++;
    }

    r->last_score = score;

    if (score > r->best_score) {
        r->best_score = score;
        r->best_at = now_epoch;
    }

    /* uint64_t intermediate, saturating at UINT32_MAX rather than
     * wrapping -- hakoniwaos's own "integer arithmetic with uint64_t
     * intermediates for anything that could overflow" rule, and the same
     * shape kf_pet_feed()'s clamp_add() already uses for a need's
     * millipercent, just against a wider ceiling. */
    const uint64_t total = static_cast<uint64_t>(r->total_score) +
                            static_cast<uint64_t>(score);
    r->total_score =
        total > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(total);

    if (first_play) {
        r->streak_days = 1u;
    } else {
        /* int32_t, not uint16_t subtraction: `day_index` landing BEFORE
         * `previous_day` (kf/game.h's own header comment -- a stale or
         * corrected day index, nothing this function can sanity-check) has
         * to compare as "not exactly one day forward" without underflowing
         * an unsigned difference into a huge positive number that would
         * otherwise misread as "a larger gap" by accident rather than by
         * the explicit `else` branch below actually being reached on
         * purpose. */
        const int32_t day_delta =
            static_cast<int32_t>(day_index) - static_cast<int32_t>(previous_day);
        if (day_delta == 0) {
            /* Same day: streak_days unchanged. */
        } else if (day_delta == 1) {
            if (r->streak_days < 255u) {
                r->streak_days++;
            }
        } else {
            r->streak_days = 1u;
        }
    }
    r->last_played_day = day_index;
}
