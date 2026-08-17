/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * Pure record and tier maths for a minigame session -- the plumbing every
 * one of the thirty-odd games this platform ships inherits, not Nibble's
 * own logic (that is examples/creature_demo/nibble.lua). See
 * the Nibble-and-the-game-session design's section 2 for the
 * reasoning: a game and a care action are different EVENTS and must never
 * be confused with each other (kf_pet_session_reward_need(), simulator/
 * src/pet/kf_pet_session.h, is the function that keeps them apart), and
 * per-game high scores live under their OWN store key rather than a
 * shared table, so a corrupt record costs one game's high score, never
 * every game's, and never the pet's own save.
 *
 * Heap-free and float-free like the rest of core (tools/check_no_heap.py,
 * check_no_float.py): functions over a caller-owned struct, no globals, no
 * I/O -- exactly kf/pet.h's own shape, and for the identical reason: a
 * session file beside kf_pet_session.cpp (simulator/src/pet/
 * kf_game_session.cpp) owns storage, rewards and the one in-flight
 * session; this file only ever computes.
 *
 * Valid C, same convention as kf/pet.h and kf/arena.h: nothing here
 * belongs to the HAL boundary kf/hal/ headers are mechanically checked
 * against, but there is no reason for this header to be any less
 * portable than it needs to be.
 */

#ifndef KF_GAME_H
#define KF_GAME_H

#include "kf/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* How a session's total score compared to the two thresholds a game's own
 * manifest declares (examples/creature_demo/nibble.lua's `reward.tiers`).
 * Numeric order matches "how well it went" deliberately, the same reason
 * kf_pet_stage's order matches life-cycle order -- "tier >= KF_GAME_TIER_
 * GOOD" reads naturally wherever that comparison is useful. MISS is 0, not
 * a sentinel past the real values: it is a real, common outcome (a tired
 * pet losing every round), not an error case. */
typedef enum {
    KF_GAME_TIER_MISS = 0,
    KF_GAME_TIER_GOOD = 1,
    KF_GAME_TIER_GREAT = 2,
} kf_game_tier;

/* One game's whole history, keyed by the game's own id (kf_game_session.h's
 * "g." + id storage key) -- NOT part of kf_pet_state and never touches the
 * pet's own save. See kf_game_pack()/kf_game_unpack() below for the exact
 * on-disk layout this struct's fields are packed into; keep the two in
 * lock-step by hand, the same discipline kf/pet.h's kf_pet_state and
 * pet.cpp's pack()/unpack() already require, for the identical reason
 * (compiler padding must never leak into a save format).
 *
 * `plays`, `best_score`, `last_score`, `total_score` and `best_at` are the
 * obvious per-game statistics a picker screen wants to show (game.record()
 * in kf/game.h's Lua binding). `last_played_day` and `streak_days` are
 * what let a game reward showing up again tomorrow without a real
 * calendar widget anywhere in Core -- `day_index` is a plain, caller-
 * supplied day number (kf/clock.h has no notion of "day index" of its
 * own; the session layer derives one from the wall clock), not a second
 * clock this file has to reason about. `perfect_count` is a coarser signal
 * than the score itself -- "how many times did this player nail the
 * timing outright", which a session score alone cannot answer once two
 * different scoring curves exist across two different games. */
typedef struct {
    uint16_t plays;
    uint32_t best_score;
    uint32_t last_score;
    uint32_t total_score;
    uint32_t best_at;         /* wall-clock epoch seconds of best_score */
    uint16_t last_played_day; /* caller-supplied day index, not epoch days */
    uint8_t streak_days;
    uint16_t perfect_count;
} kf_game_record;

/* The record's on-disk size, byte for byte: 2 (plays) + 4 (best_score) + 4
 * (last_score) + 4 (total_score) + 4 (best_at) + 2 (last_played_day) + 1
 * (streak_days) + 2 (perfect_count) = 23.
 *
 * THE FIELD LIST AND THIS NUMBER MUST AGREE -- the exact failure mode
 * kf_game_pack()'s own KF_ASSERT below exists to catch. Worth spelling out
 * because it already happened once, on paper: an earlier draft of the
 * design this header implements said 21 bytes, having listed
 * `perfect_count` in the struct without ever adding its 2 bytes to the
 * total. A save size that disagrees with its own field list is exactly
 * how a save format silently truncates -- kf_game_unpack() below would
 * read `perfect_count` off the end of a 21-byte buffer, or kf_store_write()
 * would clip it, and either way the corruption would not show up until
 * someone went looking for a specific creature's perfect count and found
 * garbage. Fixed here at 23, matching every field actually declared above. */
#define KF_GAME_RECORD_BYTES 23u

/* Packs `r` into `out` little-endian, byte by byte, NOT a struct memcpy --
 * the on-disk layout must not depend on the compiler's padding, the same
 * reason pet.cpp's pack() (hakoniwaos/src/pet.cpp:1253) packs by hand
 * rather than reinterpret-casting kf_pet_state. No version byte, unlike
 * the pet's own save: a corrupt or foreign-format game record costs one
 * game's high score (kf_game_unpack() below refuses it and the session
 * layer falls back to a fresh record), never the pet's own save or any
 * other game's, so there is nothing here worth a migration story over. */
void kf_game_pack(const kf_game_record *r, uint8_t out[KF_GAME_RECORD_BYTES]);

/* Unpacks `in` (exactly `in_bytes` long) into `out`. Returns false --
 * leaving `out` completely untouched -- when `in_bytes` is not exactly
 * KF_GAME_RECORD_BYTES, the same "refuse rather than misread" contract
 * kf_pet_load_and_advance()'s own unpack() uses for the pet's save: a
 * wrong-length buffer is not a partial record worth salvaging, it is a
 * different format entirely (an older or newer build, or a store key that
 * collided with something else), and reading past or short of it would be
 * undefined in the caller's own struct. */
bool kf_game_unpack(const uint8_t *in, size_t in_bytes, kf_game_record *out);

/* Which tier `score` reaches against a game's own two thresholds (its
 * manifest's `reward.tiers`, e.g. Nibble's `{good = 40, great = 75}`).
 * Both thresholds are "at or above": a score exactly equal to
 * `good_threshold` is GOOD, not MISS; a score exactly equal to
 * `great_threshold` is GREAT, not GOOD -- a threshold a player can never
 * actually reach by hitting it exactly would be a threshold in name only.
 * `great_threshold` is expected to be >= `good_threshold`; a manifest that
 * gets this backwards is a content bug this function does not try to
 * detect or correct for -- it just evaluates the two comparisons in order,
 * GREAT first. */
kf_game_tier kf_game_tier_for(uint32_t score, uint32_t good_threshold,
                               uint32_t great_threshold);

/* Updates `r` in place for one completed session that scored `score`,
 * ended at wall-clock `now_epoch`, on caller-supplied day `day_index`.
 * Pure: the caller (kf_game_session_end(), simulator/src/pet/
 * kf_game_session.cpp) is the one that persists the result -- this
 * function never touches storage, the same division kf_pet_advance()
 * keeps from kf_pet_save().
 *
 * `plays` increments, saturating at UINT16_MAX rather than wrapping back
 * to 0 -- the same defensive saturate kf_pet_feed() already applies to
 * `care_actions_taken` (hakoniwaos/src/pet.cpp), on the identical
 * reasoning: a wraparound thirty-odd thousand plays into a game's life
 * would silently un-count every play so far, and there is no real
 * scenario this many sessions is a bug rather than just a very well-loved
 * game.
 *
 * `last_score` is always overwritten. `best_score`/`best_at` update ONLY
 * on a STRICT improvement (score > current best) -- a tying score is not
 * a new best, it is the same best played again, so `best_at` must not
 * move to a later time for a score that did not actually beat it.
 *
 * `total_score` accumulates with a uint64_t intermediate and saturates at
 * UINT32_MAX rather than wrapping -- matching hakoniwaos/src/pet.cpp's own
 * "integer arithmetic with uint64_t intermediates for anything that could
 * overflow" rule. A wrapped total would make a well-loved game's lifetime
 * score look small instead of merely capped, which is a worse lie to show
 * a player than a number that stops climbing.
 *
 * Streak: if `r->plays` is 0 going into this call (checked BEFORE the
 * increment above), this is the very first play ever recorded, and
 * `streak_days` is set to 1 regardless of `day_index` -- there is no
 * previous day to compare against, and "a streak of zero" is not a state
 * a first-time player should ever see. Otherwise, comparing `day_index`
 * against the PREVIOUS `last_played_day`: the same day leaves
 * `streak_days` unchanged (playing five times today does not grow a
 * streak five times); exactly one day later increments it, saturating at
 * 255; any other gap -- two or more days forward, or `day_index` landing
 * BEFORE `last_played_day` (a caller passing a stale or corrected day
 * index; this function has no calendar of its own to sanity-check
 * against) -- resets it to 1. `last_played_day` is then set to
 * `day_index` unconditionally, so the next call always compares against
 * the day this one actually landed on. */
void kf_game_record_apply(kf_game_record *r, uint32_t score,
                           uint32_t now_epoch, uint16_t day_index);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KF_GAME_H */
