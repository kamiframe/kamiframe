/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * See kf_game_session.h.
 */

#include "kf_game_session.h"

#include "kf/hal/log.h"
#include "kf/hal/storage.h"
#include "kf/hal/time.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

constexpr const char *TAG = "game-session";

/* KF_STORE_MAX_KEY_LEN (15, kf/budget.h) minus the "g_" prefix -- see
 * kf_game_session_begin()'s own header comment in kf_game_session.h. */
constexpr size_t kMaxIdLen = 13u;

/* +1 for "g", +1 for "_", +kMaxIdLen for the id itself, +1 for the NUL --
 * comfortably within KF_STORE_MAX_KEY_LEN either way, this is just the
 * scratch buffer store_key() below builds the key into. */
constexpr size_t kKeyBufBytes = 2u + kMaxIdLen + 1u;

struct Session {
    bool in_flight = false;
    char id[kMaxIdLen + 1] = {};
    uint32_t score = 0u;
    uint16_t perfect_count = 0u;
    kf_pet_need need = KF_PET_NEED_NONE;
    uint32_t need_fraction_percent = 0u;
};
Session g;

/* "g_" + id, into `out` (capacity kKeyBufBytes). `id` is already validated
 * (non-empty, <= kMaxIdLen) by every caller before this is reached. */
void store_key(const char *id, char *out) {
    std::snprintf(out, kKeyBufBytes, "g_%s", id);
}

/* Reads game `id`'s record from storage into `*out`. Returns true only
 * when a genuine, correctly-sized record was actually read and unpacked;
 * false for every other case (never played, wrong size, or a backend
 * error) -- and in every false case, `*out` is left as a fresh, all-zero
 * record rather than whatever it held before, so a caller that ignores
 * the return value (kf_game_session_end() below: a missing record is a
 * fresh start, not a reason to refuse ending the session) still gets a
 * safe, well-defined starting point. Never panics on a corrupt record --
 * see kf_game_session.h's own header comment on why a bad game record
 * must cost one game's high score, never bring the firmware down. */
bool load_record(const char *id, kf_game_record *out) {
    char key[kKeyBufBytes];
    store_key(id, key);

    uint8_t buf[KF_GAME_RECORD_BYTES];
    size_t out_bytes = 0u;
    const kf_result result = kf_store_read(key, buf, sizeof(buf), &out_bytes);

    if (result == KF_ERR_UNAVAILABLE) {
        /* Ordinary "never played this game before" -- not worth a log
         * line, the same way a fresh pet save's KF_ERR_UNAVAILABLE isn't
         * (kf_pet_load_and_advance(), hakoniwaos/src/pet.cpp). */
        *out = kf_game_record{};
        return false;
    }
    if (result != KF_OK) {
        KF_LOGW(TAG,
                "kf_store_read('%s') failed (%d) -- treating as a fresh "
                "record rather than failing the session",
                key, static_cast<int>(result));
        *out = kf_game_record{};
        return false;
    }
    if (!kf_game_unpack(buf, out_bytes, out)) {
        /* kf_game_unpack() already logged why (wrong length). */
        *out = kf_game_record{};
        return false;
    }
    return true;
}

/* Packs and writes `record` under game `id`'s own key. A failed write is
 * logged, not fatal -- matching kf_pet_session_save()'s own treatment of
 * a failed pet-save write (a full store should be visible in the log, not
 * crash the device); the in-memory session state is already correct for
 * the rest of this run regardless of whether the write lands. */
void save_record(const char *id, const kf_game_record *record) {
    char key[kKeyBufBytes];
    store_key(id, key);

    uint8_t buf[KF_GAME_RECORD_BYTES];
    kf_game_pack(record, buf);
    const kf_result result = kf_store_write(key, buf, sizeof(buf));
    if (result != KF_OK) {
        KF_LOGE(TAG,
                "kf_store_write('%s') failed (%d) -- this session's record "
                "did not survive to disk",
                key, static_cast<int>(result));
    }
}

/* A stable per-GAME (not per-call) index into KF_PET_CARE_VARIATION_COUNT,
 * derived from the game's own id string rather than a hand-cycled counter
 * the way kf_home_screen_input.cpp's g_play_variation is for a real,
 * repeated button press. A minigame session has no equivalent "which
 * variation this time" concept of its own -- Nibble does not ask the
 * player to pick a flavour of play -- so this is what gives each
 * DIFFERENT game its own stable personality-preference flavour (Nibble
 * always reads as the same "variation" of play to a given pet, every
 * session, so its reaction is consistent from one playthrough to the
 * next) without inventing a second variation-cycling scheme to keep in
 * step with the real one. Deliberately simple (sum of bytes, modulo the
 * variation count): this only needs to be STABLE per id, not
 * well-distributed or collision-resistant. */
uint8_t variation_from_id(const char *id) {
    uint32_t sum = 0u;
    for (const char *p = id; *p != '\0'; ++p) {
        sum += static_cast<uint8_t>(*p);
    }
    return static_cast<uint8_t>(sum % KF_PET_CARE_VARIATION_COUNT);
}

/* Which care action a kf_pet_need corresponds to, for looking up "a full
 * grant" of that need via kf_pet_reaction_to() against config->care_
 * boost_*_mp -- the exact table kf_pet_feed()/_play()/_rest() themselves
 * read (hakoniwaos/src/pet.cpp's apply_care_reaction()). KF_PET_NEED_NONE
 * never reaches here (every call site guards it first); it falls through
 * to PLAY only so the switch has no unhandled case, not because that
 * mapping is ever actually used. */
kf_pet_care_action care_action_for_need(kf_pet_need need) {
    switch (need) {
    case KF_PET_NEED_HUNGER:
        return KF_PET_CARE_FEED;
    case KF_PET_NEED_ENERGY:
        return KF_PET_CARE_REST;
    case KF_PET_NEED_HAPPINESS:
    case KF_PET_NEED_NONE:
    default:
        return KF_PET_CARE_PLAY;
    }
}

/* "A full grant", for whichever reaction kf_pet_reaction_to() returned --
 * the exact three numbers kf_pet_feed()/_play()/_rest() already grant a
 * real care action, reused rather than re-invented (see kf_game_session.h's
 * own header comment on why). */
kf_pet_millipercent full_grant_for_reaction(const kf_pet_config *config,
                                             uint8_t reaction) {
    switch (reaction) {
    case KF_PET_REACTION_LIKED:
        return config->care_boost_liked_mp;
    case KF_PET_REACTION_DISLIKED:
        return config->care_boost_disliked_mp;
    case KF_PET_REACTION_NEUTRAL:
    default:
        return config->care_boost_neutral_mp;
    }
}

/* A plain day-since-epoch count from the wall clock, for kf_game_record_
 * apply()'s `day_index` parameter (kf/game.h) -- Core has no notion of a
 * "day index" of its own (kf/clock.h works in seconds and civil time, not
 * a running day count), so the session layer derives one, exactly as kf/
 * game.h's own header comment says it must. An invalid or non-positive
 * wall clock (never set yet -- the same case kf_pet_load_and_advance()
 * guards) reads as day 0 rather than reaching for a clock this function
 * has no way to trust; a streak that starts at day 0 and only ever moves
 * forward once the clock IS set behaves exactly like any other pet whose
 * clock was unset at first boot. Masked to 16 bits explicitly (`&
 * 0xFFFFull`), not relied on to truncate implicitly -- day_index is
 * uint16_t, and an explicit mask is what keeps this warning-clean under
 * "treat every narrowing conversion as deliberate" on every compiler this
 * project builds with (GCC, Clang, and MSVC on Windows -- see ADR/commit
 * b26775c, the Windows CI build a silent narrowing cast broke once
 * already). Wrapping after roughly 179 years of continuous device
 * operation is not a case worth guarding further. */
uint16_t day_index_from_wall_clock() {
    const kf_wall_time now = kf_time_wall();
    uint64_t days = 0u;
    if (now.valid && now.epoch_seconds > 0) {
        days = static_cast<uint64_t>(now.epoch_seconds) / 86400ull;
    }
    return static_cast<uint16_t>(days & 0xFFFFull);
}

} // namespace

bool kf_game_session_begin(const char *id, uint32_t energy_cost_mp,
                            kf_pet_need need, uint32_t need_fraction_percent,
                            kf_game_session_context *out) {
    const size_t id_len = (id == nullptr) ? 0u : std::strlen(id);
    if (id == nullptr || id_len == 0u || id_len > kMaxIdLen) {
        KF_LOGE(TAG,
                "kf_game_session_begin: id '%s' is empty or longer than %zu "
                "characters -- refusing to start a session",
                id == nullptr ? "(null)" : id, kMaxIdLen);
        return false;
    }

    const kf_pet_state *pet = kf_pet_session_state();
    if (pet->dead || pet->asleep) {
        KF_LOGI(TAG,
                "kf_game_session_begin('%s') refused: the pet is %s",
                id, pet->dead ? "dead" : "asleep");
        return false;
    }

    /* Snapshot BEFORE spending the energy cost below -- kf_game_session.h's
     * own header comment on kf_game_session_context explains why: a game
     * reads how tired the pet already was going in, not how tired paying
     * for the session just made it. */
    out->energy_mp = pet->energy_mp;
    out->stage = static_cast<uint8_t>(pet->stage);
    out->base_trait = pet->base_trait;
    /* 0 at or above 50% (50000 mp) energy, rising linearly to 100 at 0
     * energy. Integer maths only: `deficit` is 0..50000, so `deficit *
     * 100` fits comfortably in a uint64_t intermediate with room to
     * spare -- the same "uint64_t intermediate for anything that could
     * overflow" rule hakoniwaos/src/pet.cpp's own decay maths follows. */
    constexpr kf_pet_millipercent kHandicapFloorMp = 50000u;
    if (pet->energy_mp >= kHandicapFloorMp) {
        out->handicap_percent = 0u;
    } else {
        const uint64_t deficit = kHandicapFloorMp - pet->energy_mp;
        const uint64_t handicap = deficit * 100ull / kHandicapFloorMp;
        out->handicap_percent = static_cast<uint8_t>(handicap);
    }

    /* Spent unconditionally from here on, win or lose -- see kf_game_
     * session_begin()'s own header comment for why this is the load-
     * bearing line in the whole function. */
    kf_pet_session_spend_need(KF_PET_NEED_ENERGY, energy_cost_mp);

    g.in_flight = true;
    std::snprintf(g.id, sizeof(g.id), "%s", id);
    g.score = 0u;
    g.perfect_count = 0u;
    g.need = need;
    g.need_fraction_percent = need_fraction_percent;
    return true;
}

void kf_game_session_score(uint32_t points) {
    if (!g.in_flight) {
        KF_LOGW(TAG, "kf_game_session_score called with no session in "
                     "flight -- ignoring");
        return;
    }
    const uint64_t sum = static_cast<uint64_t>(g.score) + points;
    g.score = sum > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(sum);
}

void kf_game_session_event(const char *event) {
    if (!g.in_flight || event == nullptr) {
        return;
    }
    /* Only "perfect" does anything -- see kf_game_session.h's own header
     * comment on why every other name, known or not, is silently
     * ignored rather than an error. */
    if (std::strcmp(event, "perfect") == 0) {
        if (g.perfect_count < UINT16_MAX) {
            g.perfect_count++;
        }
    }
}

kf_game_tier kf_game_session_end(uint32_t good_threshold,
                                  uint32_t great_threshold) {
    if (!g.in_flight) {
        KF_LOGW(TAG, "kf_game_session_end called with no session in "
                     "flight -- ignoring");
        return KF_GAME_TIER_MISS;
    }

    const kf_game_tier tier =
        kf_game_tier_for(g.score, good_threshold, great_threshold);

    if (tier != KF_GAME_TIER_MISS) {
        const kf_pet_state *pet = kf_pet_session_state();
        const kf_pet_config *config = kf_pet_session_config();
        const uint8_t variation = variation_from_id(g.id);

        /* Every game pays happiness first (the design's decision 7) --
         * scaled by this pet's reaction to THIS game, at the stable
         * per-id variation above, against KF_PET_CARE_PLAY's own boost
         * table: the same magnitude a real pet.play() would have
         * granted for that reaction. */
        const uint8_t happiness_reaction =
            kf_pet_reaction_to(pet->base_trait, KF_PET_CARE_PLAY, variation);
        kf_pet_session_reward_need(
            KF_PET_NEED_HAPPINESS,
            full_grant_for_reaction(config, happiness_reaction));

        /* A game MAY also feed a specific need, at LESS than a full
         * grant's worth (need_fraction_percent) -- the design's decision
         * 7: "a game may also feed a specific need... but at less than a
         * full meal's worth. [It] is not a substitute for feeding." */
        if (g.need != KF_PET_NEED_NONE && g.need_fraction_percent > 0u) {
            const kf_pet_care_action need_action =
                care_action_for_need(g.need);
            const uint8_t need_reaction =
                kf_pet_reaction_to(pet->base_trait, need_action, variation);
            const kf_pet_millipercent full_grant =
                full_grant_for_reaction(config, need_reaction);
            const uint64_t scaled = static_cast<uint64_t>(full_grant) *
                                     g.need_fraction_percent / 100ull;
            const kf_pet_millipercent scaled_mp =
                scaled > KF_PET_MILLIPERCENT_MAX
                    ? KF_PET_MILLIPERCENT_MAX
                    : static_cast<kf_pet_millipercent>(scaled);
            kf_pet_session_reward_need(g.need, scaled_mp);
        }
    }
    /* MISS: pays nothing, subtracts nothing beyond the energy already
     * spent at kf_game_session_begin() -- the design's decision 5. */

    kf_game_record record{};
    load_record(g.id, &record); /* missing/corrupt -> a fresh record;
                                  * return value deliberately unused here,
                                  * see load_record()'s own comment. */
    const kf_wall_time now = kf_time_wall();
    const uint32_t now_epoch =
        now.valid && now.epoch_seconds > 0
            ? static_cast<uint32_t>(now.epoch_seconds)
            : 0u;
    kf_game_record_apply(&record, g.score, now_epoch,
                          day_index_from_wall_clock());

    /* kf_game_record_apply() (kf/game.h) knows nothing about perfects --
     * it takes exactly the four values (score, now_epoch, day_index) its
     * own signature lists, and perfect_count is not one of them. This is
     * the LIFETIME count (matching total_score's own "accumulates across
     * every session ever played" shape, not last_score's "this session
     * only" one -- game.record()'s `perfects` field, Task 3), so this
     * session's g.perfect_count is ADDED to whatever the record already
     * held, saturating at UINT16_MAX rather than wrapping, the same
     * defensive ceiling every other accumulating field in this record
     * already uses. */
    const uint32_t perfect_total =
        static_cast<uint32_t>(record.perfect_count) + g.perfect_count;
    record.perfect_count = perfect_total > UINT16_MAX
                                ? UINT16_MAX
                                : static_cast<uint16_t>(perfect_total);

    save_record(g.id, &record);

    g.in_flight = false;
    return tier;
}

bool kf_game_session_record(const char *id, kf_game_record *out) {
    if (id == nullptr || std::strlen(id) == 0u ||
        std::strlen(id) > kMaxIdLen) {
        return false;
    }
    kf_game_record record{};
    if (!load_record(id, &record)) {
        return false;
    }
    *out = record;
    return true;
}
