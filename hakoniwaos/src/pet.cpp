/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * See kf/pet.h for what this is. Three things worth knowing before reading
 * the functions below:
 *
 * All decay/care maths is integer, in millipercent (0..100000), and every
 * intermediate product is computed in uint64_t before being clamped back
 * down -- decay_rate_mp_per_hour (up to a few thousand) times
 * elapsed_seconds (which for offline fast-forward can genuinely be weeks)
 * would overflow uint32_t; it cannot overflow uint64_t for any elapsed
 * time this project will ever see.
 *
 * Stage progression inside kf_pet_advance() is a small loop, but bounded
 * by the number of REMAINING LIFE STAGES (at most 4), never by
 * elapsed_seconds -- see kf/pet.h's comment on kf_pet_advance() for why
 * that is not the per-second simulation ADR 0015 already rejected.
 *
 * The care score that picks a branch (child->teen, teen->adult) is a
 * left-Riemann-sum numerical integral, not a mathematically exact one: each
 * segment of elapsed time is weighted by the need average AT THE START of
 * that segment, before this segment's own decay is applied. This is a
 * deliberate, documented approximation, not a bug -- the same category of
 * accepted trade-off kf_pet_session.cpp's own live-tick batching already
 * makes and documents (see that file's header comment), and for the
 * identical reason: an exact continuous integral needs either float (which
 * this project avoids for exactly the drift reasons kf/pet.h's own header
 * comment gives) or many small steps (which defeats the point of a
 * closed-form calculation). Chris asked for "an average over the whole
 * stage, not a snapshot" -- this delivers that faithfully; it is not
 * claiming lab-grade numerical precision beyond that.
 *
 * The on-disk save format is packed and unpacked byte by byte
 * (put_u32/get_u32/put_i64/get_i64/put_u64/get_u64 below), not written as a
 * raw `kf_store_write(state, sizeof(*state))`. A C++ struct's layout --
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
#include "kf/rng.h"

#include <cstdint>

#include "kf/poison.h"

namespace {

constexpr const char *TAG = "pet";

/* Save format version. Bumped to 2 with ADR 0021 (life stages/evolution),
 * to 3 with ADR 0023 (personality traits), to 4 with the evolution-tree
 * reconciliation (care_actions_taken added; see kf/pet.h's KF_PET_SAVE_BYTES
 * comment), to 5 with mess (poop_count, seconds_until_next_poop and
 * dirtiness_mp added), and to 6 with sickness (neglect_seconds and sick
 * added) -- a version-5 save has no notion of accumulated neglect at all,
 * so there is no honest default to fill in for it (zero would silently
 * un-sicken a creature that was ill the moment it was saved); refusing the
 * load and falling back to a fresh pet is the same accepted cost the
 * version-4/5 bumps already took, not a new one. kf_pet_deserialize()
 * refuses to load anything written by a different version rather than
 * guessing at a layout that changed, see unpack() below. */
constexpr uint8_t kSaveVersion = 6;

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

/* Cut, Hold, Mark, Go -- character bible section 6, in that index order. */
constexpr uint8_t kAdultsInFamily[KF_PET_TEEN_FORM_COUNT] = {2u, 3u, 3u, 1u};

/* -----------------------------------------------------------------------
 * Stage progression.
 * ----------------------------------------------------------------------- */

uint32_t stage_duration_seconds(const kf_pet_config *config,
                                 kf_pet_stage stage) {
    switch (stage) {
    case KF_PET_STAGE_EGG:
        return config->egg_duration_seconds;
    case KF_PET_STAGE_BABY:
        return config->baby_duration_seconds;
    case KF_PET_STAGE_CHILD:
        return config->child_duration_seconds;
    case KF_PET_STAGE_TEEN:
        return config->teen_duration_seconds;
    case KF_PET_STAGE_ADULT:
    default:
        /* Terminal in this slice -- never consulted, since
         * kf_pet_advance()'s stage loop stops once stage == ADULT. */
        return 0u;
    }
}

/* True for exactly the two stages whose care actually feeds a branch
 * decision at their own end, per Chris's design: care during CHILD picks
 * the teen form, care during TEEN picks the adult branch. Egg has no
 * needs at all yet (see apply_stage_segment()); baby's care is real (the
 * pet still needs feeding) but does not feed a branch, since baby always
 * leads to exactly one child, no choice to make. */
bool stage_feeds_a_branch_choice(kf_pet_stage stage) {
    return stage == KF_PET_STAGE_CHILD || stage == KF_PET_STAGE_TEEN;
}

/* Maps an accumulated care score to one of `branch_count` equal-width
 * bands -- band 0 is the neediest/worst-cared-for band, band
 * `branch_count - 1` is the best. Equal bands are the simplest defensible
 * default and, like every other number in this file, a config surface
 * later if equal turns out to be the wrong shape once pets are actually
 * being raised -- see kf/pet.h's header comment on why the TREE shape
 * (branch_count itself) is compile-time but this mapping is not exposed
 * as config yet. */
uint8_t select_branch(uint64_t care_integral_mp_seconds,
                       uint64_t stage_elapsed_seconds, uint32_t branch_count) {
    if (stage_elapsed_seconds == 0u) {
        /* Zero-duration stage (a misconfigured kf_pet_config): nothing to
         * average. Land on band 0 rather than dividing by zero -- a
         * defensive default, not a real gameplay case this project
         * expects to hit. */
        return 0u;
    }
    const uint64_t average_mp = care_integral_mp_seconds / stage_elapsed_seconds;
    uint64_t band = (average_mp * branch_count) /
                     (static_cast<uint64_t>(KF_PET_MILLIPERCENT_MAX) + 1u);
    if (band >= branch_count) {
        band = branch_count - 1u; /* average_mp == MAX lands exactly on the top edge */
    }
    return static_cast<uint8_t>(band);
}

/* Whether the creature is in a neglected condition right now. A pure read
 * of the state, deliberately: nothing is stored, so there is no second copy
 * of "is it neglected" that can drift out of step with the fields it comes
 * from. */
bool is_neglected(const kf_pet_state *state, const kf_pet_config *config) {
    return state->hunger_mp <= config->neglect_need_mp ||
           state->happiness_mp <= config->neglect_need_mp ||
           state->energy_mp <= config->neglect_need_mp ||
           state->poop_count > config->neglect_poop_count ||
           state->dirtiness_mp >= config->neglect_dirtiness_mp;
}

/* Adds `add` to a saturating uint32_t counter. */
uint32_t saturating_add_u32(uint32_t value, uint32_t add) {
    const uint64_t sum = static_cast<uint64_t>(value) + add;
    return sum > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(sum);
}

/* Advances `state` past its current (already-complete) stage into the
 * next one, picking a branch and resetting the per-stage accumulators
 * where that applies. Only ever called once kf_pet_advance()'s loop has
 * confirmed stage_elapsed_seconds has reached the stage's full duration. */
void advance_to_next_stage(kf_pet_state *state) {
    switch (state->stage) {
    case KF_PET_STAGE_EGG:
        state->stage = KF_PET_STAGE_BABY;
        break;
    case KF_PET_STAGE_BABY:
        state->stage = KF_PET_STAGE_CHILD;
        break;
    case KF_PET_STAGE_CHILD:
        /* Never touched -> dust, regardless of what the care average says.
         * Checked BEFORE the ordinary branch selection because it is not a
         * quality judgement: an untouched creature has no care history to
         * grade, and grading it anyway would land it in whichever family
         * zero care happens to map to. See character bible section 8. */
        if (state->care_actions_taken == 0u) {
            state->teen_form = KF_PET_TEEN_FORM_DUST;
        } else {
            state->teen_form = select_branch(state->care_integral_mp_seconds,
                                              state->stage_elapsed_seconds,
                                              KF_PET_TEEN_FORM_COUNT);
        }
        state->stage = KF_PET_STAGE_TEEN;
        break;
    case KF_PET_STAGE_TEEN:
        /* kf_pet_adults_in_family() returns 1 for out-of-range input, and
         * KF_PET_TEEN_FORM_DUST is out-of-range by construction (it equals
         * KF_PET_TEEN_FORM_COUNT) -- relied on deliberately here to give the
         * dust form exactly its one adult (Hokorimaru), not by accident. */
        state->adult_branch = select_branch(
            state->care_integral_mp_seconds, state->stage_elapsed_seconds,
            kf_pet_adults_in_family(state->teen_form));
        state->stage = KF_PET_STAGE_ADULT;
        break;
    case KF_PET_STAGE_ADULT:
    default:
        /* Terminal -- kf_pet_advance()'s loop never calls this once
         * stage == ADULT, but handle it defensively rather than falling
         * through into resetting accumulators for a stage transition
         * that should not happen. */
        return;
    }
    state->stage_elapsed_seconds = 0u;
    state->care_integral_mp_seconds = 0u;
}

/* ADR 0023: updates the three per-need personality accumulators (whole-
 * life, never reset) for exactly `segment` seconds, weighting each need by
 * its value BEFORE this segment's own decay -- the identical "weight by
 * the start-of-segment value" convention care_integral_mp_seconds already
 * uses just above, applied per-need instead of to one blended average.
 *
 * The periodic halving itself is one division and (at most) one shift, not
 * a loop -- closed-form regardless of how large `segment` is, matching
 * this file's existing "bounded by stages, never by elapsed_seconds"
 * discipline (see the file header comment and kf_pet_advance()). `window`
 * is the running total of not-yet-consumed seconds; every half_life
 * seconds' worth that accumulates in it triggers exactly one halving of
 * all three accumulators, and the leftover remainder carries forward in
 * care_recency_window_seconds for the next call to pick up exactly where
 * this one left off -- so many small per-frame segments and one huge
 * offline-fast-forward segment produce the identical halving schedule for
 * the identical total elapsed time.
 *
 * `halvings` can in principle be enormous (a multi-year offline gap against
 * a short configured half-life) -- shifting a 64-bit value by 64 or more is
 * undefined behaviour in C++, so the actual shift amount is capped at 63,
 * which already flushes every accumulator to 0 (any real value right-
 * shifted 63 times is 0), the mathematically correct answer for "so much
 * time passed under this half-life that nothing old survives" without
 * needing to special-case it. */
void accumulate_personality(kf_pet_state *state, const kf_pet_config *config,
                             uint32_t segment, uint64_t hunger_before_mp,
                             uint64_t happiness_before_mp,
                             uint64_t energy_before_mp) {
    const uint32_t half_life = config->personality_recency_half_life_seconds;
    if (half_life > 0u) {
        uint64_t window =
            static_cast<uint64_t>(state->care_recency_window_seconds) + segment;
        const uint64_t halvings = window / half_life;
        if (halvings > 0u) {
            const uint64_t shift = halvings > 63u ? 63u : halvings;
            state->hunger_integral_mp_seconds >>= shift;
            state->happiness_integral_mp_seconds >>= shift;
            state->energy_integral_mp_seconds >>= shift;
            window %= half_life;
        }
        state->care_recency_window_seconds = static_cast<uint32_t>(window);
    }
    /* half_life == 0 (a misconfigured kf_pet_config): treated as "no
     * periodic halving," i.e. plain whole-life accumulation below, rather
     * than dividing by zero -- the same defensive-default spirit as
     * select_branch()'s zero-stage-duration guard above. */

    state->hunger_integral_mp_seconds += hunger_before_mp * segment;
    state->happiness_integral_mp_seconds += happiness_before_mp * segment;
    state->energy_integral_mp_seconds += energy_before_mp * segment;
}

/* Applies decay (and, on the stages where it matters, accumulates the care
 * integral and/or the personality accumulators) for exactly `segment`
 * seconds, all still within the SAME stage -- kf_pet_advance()'s loop
 * never lets a segment cross a stage boundary, so this never needs to know
 * about stages other than the current one. */
void apply_stage_segment(kf_pet_state *state, const kf_pet_config *config,
                          uint32_t segment) {
    if (state->stage == KF_PET_STAGE_EGG) {
        /* No care needed as an egg -- see kf/pet.h's header comment and
         * kf_pet_init(): needs stay at whatever they were (full, for a
         * freshly-initialised pet) until the egg hatches. Personality
         * does not accumulate here either, for the same reason
         * care_integral_mp_seconds does not: nothing has been cared for
         * yet, so there is nothing meaningful to weight by. */
        return;
    }

    /* Indexed directly by stage -- the enum's values are 0..4 by definition
     * (kf/pet.h) and KF_PET_STAGE_COUNT sits next to it to keep them in
     * step. Clamped anyway: a corrupted save that survived the version
     * check should degrade to the gentlest rates, not read off the end of
     * the table. */
    unsigned stage_index = static_cast<unsigned>(state->stage);
    if (stage_index >= KF_PET_STAGE_COUNT) {
        stage_index = KF_PET_STAGE_ADULT;
    }
    const kf_pet_stage_rates &rates = config->stage_rates[stage_index];

    const uint64_t hunger_before_mp = state->hunger_mp;
    const uint64_t happiness_before_mp = state->happiness_mp;
    const uint64_t energy_before_mp = state->energy_mp;
    const bool neglected_before = is_neglected(state, config);

    const bool feeds_branch = stage_feeds_a_branch_choice(state->stage);
    uint64_t average_before_mp = 0u;
    if (feeds_branch) {
        average_before_mp =
            (hunger_before_mp + happiness_before_mp + energy_before_mp) / 3u;
    }

    state->hunger_mp = apply_decay(state->hunger_mp, rates.hunger_mp_per_hour, segment);
    state->happiness_mp =
        apply_decay(state->happiness_mp, rates.happiness_mp_per_hour, segment);
    state->energy_mp = apply_decay(state->energy_mp, rates.energy_mp_per_hour, segment);

    if (feeds_branch) {
        state->care_integral_mp_seconds += average_before_mp * segment;
    }

    /* Mess advances with the needs, inside the same segment loop, so
     * offline fast-forward covers it without a second code path. A pet left
     * in a drawer for a day comes back correctly filthy.
     *
     * Closed-form -- one division, not a while-loop counting down one
     * interval at a time -- for the same reason every other closed-form
     * calculation in this file exists (see the file header comment): a
     * multi-week offline segment must cost the same handful of steps as a
     * one-second frame tick. It also means a misconfigured zero interval
     * cannot hang the way a "subtract the interval every iteration" loop
     * would: that case is handled directly below by simply not counting any
     * poops, not by a guard bolted on in front of a loop that could
     * otherwise spin forever. */
    if (state->seconds_until_next_poop == 0u) {
        state->seconds_until_next_poop = config->poop_interval_seconds;
    }
    if (config->poop_interval_seconds == 0u) {
        /* Misconfigured: no interval to count down, so no mess rather than
         * an infinite loop. kf_pet_default_config() never produces this,
         * but a corrupted or hand-built config must still degrade safely. */
        state->seconds_until_next_poop = 0u;
    } else if (segment >= state->seconds_until_next_poop) {
        const uint32_t after_first = segment - state->seconds_until_next_poop;
        const uint64_t extra_poops =
            static_cast<uint64_t>(after_first) / config->poop_interval_seconds;
        const uint64_t total_poops = 1ull + extra_poops;
        const uint64_t new_poop_count =
            static_cast<uint64_t>(state->poop_count) + total_poops;
        state->poop_count = new_poop_count >= KF_PET_MAX_POOPS
                                 ? static_cast<uint8_t>(KF_PET_MAX_POOPS)
                                 : static_cast<uint8_t>(new_poop_count);
        state->seconds_until_next_poop =
            config->poop_interval_seconds -
            static_cast<uint32_t>(after_first % config->poop_interval_seconds);
    } else {
        state->seconds_until_next_poop -= segment;
    }

    /* Dirtiness rises with time, faster the more mess is waiting -- reads
     * state->poop_count AFTER the block above, so this segment's own new
     * poops (if any) already count towards this segment's dirtying.
     * Saturates at full rather than wrapping. */
    const uint64_t dirtiness_rise_per_hour =
        static_cast<uint64_t>(config->dirtiness_rise_mp_per_hour) +
        (static_cast<uint64_t>(config->dirtiness_rise_per_poop_mp_per_hour) *
         state->poop_count);
    const uint64_t dirtiness_rise =
        (dirtiness_rise_per_hour * segment) / 3600u;
    if (dirtiness_rise >= KF_PET_MILLIPERCENT_MAX - state->dirtiness_mp) {
        state->dirtiness_mp = KF_PET_MILLIPERCENT_MAX;
    } else {
        state->dirtiness_mp = static_cast<kf_pet_millipercent>(
            state->dirtiness_mp + dirtiness_rise);
    }

    accumulate_personality(state, config, segment, hunger_before_mp,
                            happiness_before_mp, energy_before_mp);

    /* Neglect, and the illness it turns into. Evaluated LAST in the
     * segment, and sampled at BOTH ends of it.
     *
     * Both ends, because neither alone survives offline fast-forward: a
     * week in a drawer begins with a healthy creature, so sampling only the
     * start would call that week fine, and sampling only the end would call
     * a fortnight that soured in its final hour a fortnight of neglect.
     * Each end counting for half is the trapezoid rule -- two comparisons,
     * no loop, and an error bounded by the segment rather than growing with
     * it. A segment that starts healthy and ends badly therefore nets
     * exactly zero, which is the correct reading of "half of it was fine".
     *
     * A creature that has never been cared for is exempt from all of it.
     * The character bible's dust form is what total absence of interaction
     * produces, and it takes a full childhood to reach; illness would kill
     * that creature days before it got there. It is the creature that has
     * known care and then lost it that sickens. */
    if (state->care_actions_taken > 0u) {
        const bool neglected_after = is_neglected(state, config);

        uint32_t neglected_for = 0u;
        if (neglected_before) {
            neglected_for += segment / 2u;
        }
        if (neglected_after) {
            neglected_for += segment - (segment / 2u);
        }
        const uint32_t cared_for = segment - neglected_for;

        state->neglect_seconds =
            saturating_add_u32(state->neglect_seconds, neglected_for);
        state->neglect_seconds = state->neglect_seconds > cared_for
                                      ? state->neglect_seconds - cared_for
                                      : 0u;

        if (state->neglect_seconds >= config->sickness_onset_seconds) {
            state->sick = true;
        } else if (state->neglect_seconds == 0u) {
            state->sick = false;
        }
    }
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

void put_u64(uint8_t *buf, size_t &offset, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        buf[offset++] = static_cast<uint8_t>((v >> (8 * i)) & 0xFFu);
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

uint64_t get_u64(const uint8_t *buf, size_t &offset) {
    uint64_t u = 0;
    for (unsigned i = 0; i < 8; ++i) {
        u |= static_cast<uint64_t>(buf[offset + i]) << (8u * i);
    }
    offset += 8;
    return u;
}

void pack(const kf_pet_state *state, uint8_t out[KF_PET_SAVE_BYTES]) {
    size_t off = 0;
    put_u8(out, off, kSaveVersion);
    put_u32(out, off, state->hunger_mp);
    put_u32(out, off, state->happiness_mp);
    put_u32(out, off, state->energy_mp);
    put_u8(out, off, state->poop_count);
    put_u32(out, off, state->seconds_until_next_poop);
    put_u32(out, off, state->dirtiness_mp);
    put_u32(out, off, state->neglect_seconds);
    put_u8(out, off, state->sick ? 1u : 0u);
    put_u8(out, off, state->last_advanced.valid ? 1u : 0u);
    put_i64(out, off, state->last_advanced.epoch_seconds);
    put_u8(out, off, static_cast<uint8_t>(state->stage));
    put_u8(out, off, state->teen_form);
    put_u8(out, off, state->adult_branch);
    put_u64(out, off, state->stage_elapsed_seconds);
    put_u64(out, off, state->care_integral_mp_seconds);
    put_u64(out, off, state->hunger_integral_mp_seconds);
    put_u64(out, off, state->happiness_integral_mp_seconds);
    put_u64(out, off, state->energy_integral_mp_seconds);
    put_u32(out, off, state->care_recency_window_seconds);
    put_u8(out, off, state->base_trait);
    put_u32(out, off, state->care_actions_taken);
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
                "refusing to load (a version-1 save predates life stages; "
                "it resets to a fresh pet, see ADR 0021)",
                version, kSaveVersion);
        return false;
    }
    state->hunger_mp = get_u32(in, off);
    state->happiness_mp = get_u32(in, off);
    state->energy_mp = get_u32(in, off);
    state->poop_count = get_u8(in, off);
    state->seconds_until_next_poop = get_u32(in, off);
    state->dirtiness_mp = get_u32(in, off);
    state->neglect_seconds = get_u32(in, off);
    state->sick = get_u8(in, off) != 0u;
    state->last_advanced.valid = get_u8(in, off) != 0u;
    state->last_advanced.epoch_seconds = get_i64(in, off);
    const uint8_t stage_byte = get_u8(in, off);
    if (stage_byte > static_cast<uint8_t>(KF_PET_STAGE_ADULT)) {
        KF_LOGE(TAG, "save has an invalid stage byte (%u) -- refusing to load",
                stage_byte);
        return false;
    }
    state->stage = static_cast<kf_pet_stage>(stage_byte);
    state->teen_form = get_u8(in, off);
    state->adult_branch = get_u8(in, off);
    state->stage_elapsed_seconds = get_u64(in, off);
    state->care_integral_mp_seconds = get_u64(in, off);
    state->hunger_integral_mp_seconds = get_u64(in, off);
    state->happiness_integral_mp_seconds = get_u64(in, off);
    state->energy_integral_mp_seconds = get_u64(in, off);
    state->care_recency_window_seconds = get_u32(in, off);
    const uint8_t base_trait_byte = get_u8(in, off);
    if (base_trait_byte >= KF_PET_BASE_TRAIT_COUNT) {
        KF_LOGE(TAG,
                "save has an invalid base_trait byte (%u, table has only %u "
                "entries) -- refusing to load",
                base_trait_byte, KF_PET_BASE_TRAIT_COUNT);
        return false;
    }
    state->base_trait = base_trait_byte;
    state->care_actions_taken = get_u32(in, off);
    return true;
}

} // namespace

uint8_t kf_pet_adults_in_family(uint8_t teen_form) {
    if (teen_form >= KF_PET_TEEN_FORM_COUNT) {
        return 1u;
    }
    return kAdultsInFamily[teen_form];
}

kf_pet_config kf_pet_default_config(void) {
    kf_pet_config c{};
    /* Per-stage demand. Derived in
     * docs/superpowers/plans/2026-08-09-demand-curve.md: "needs attention"
     * is dropping to 70%, "critical" is reaching zero, and the three needs
     * keep the 1 : 0.67 : 0.5 ratio the old flat defaults used, so hunger
     * bites first and energy last.
     *
     * These are the numbers most likely to be wrong after a week of living
     * with a real pet. They are here, together, in one table, precisely so
     * that tuning them is editing five rows rather than hunting through
     * logic. */
    c.stage_rates[KF_PET_STAGE_EGG] = {0u, 0u, 0u};          /* never decays */
    c.stage_rates[KF_PET_STAGE_BABY] = {66000u, 44000u, 33000u};  /* ~30 min */
    c.stage_rates[KF_PET_STAGE_CHILD] = {33000u, 22000u, 16000u}; /* ~1 h */
    c.stage_rates[KF_PET_STAGE_TEEN] = {16000u, 11000u, 8000u};   /* ~2 h */
    c.stage_rates[KF_PET_STAGE_ADULT] = {8000u, 5500u, 4000u};    /* ~4 h */

    /* Roughly one poop per half hour, or ten minutes after a meal. Tuning
     * numbers, in the same spirit as the stage rates above: the shape is
     * "eating causes mess sooner", the exact figures are for living with. */
    c.poop_interval_seconds = 1800u;
    c.poop_interval_after_feed_seconds = 600u;

    /* Clean-ish pet reaches flies in about 12 hours; a pet with the full
     * eight poops waiting gets there in about 2. Tuning numbers. */
    c.dirtiness_rise_mp_per_hour = 4000u;
    c.dirtiness_rise_per_poop_mp_per_hour = 2500u;

    /* A need at 10% or below, six poops down, or dirtiness past the stink
     * threshold. The dirtiness figure is deliberately the same place the
     * stink lines appear (KF_PET_DIRTY_STINK_MP): what the player can see
     * is what is hurting the creature, rather than an invisible second
     * threshold they would have to infer. */
    c.neglect_need_mp = 10000u;
    c.neglect_dirtiness_mp = KF_PET_DIRTY_STINK_MP;
    c.neglect_poop_count = 6u;

    /* Three hours of neglect to fall ill. Tuning number: long enough that
     * an afternoon out cannot do it from full bars, short enough that a
     * neglected creature shows it the same day. */
    c.sickness_onset_seconds = 3u * 3600u;

    /* Illustrative stage timing -- adult by about a week. See kf/pet.h's
     * header comment: Chris's own call is "I'll decide exact numbers
     * later, just make it configurable," so these are a starting point
     * to build and demo against, not a tuning recommendation. */
    c.egg_duration_seconds = 3600u;             /* 1 hour */
    c.baby_duration_seconds = 86400u;           /* 1 day */
    c.child_duration_seconds = 2u * 86400u;     /* 2 days */
    c.teen_duration_seconds = 3u * 86400u;      /* 3 days */

    /* ADR 0023: illustrative, same status as every other number in this
     * function -- 24h means a stretch of one care style dominates the
     * personality reading for about a day after it ends before fresher
     * care outweighs it. */
    c.personality_recency_half_life_seconds = 86400u; /* 24 hours */
    return c;
}

void kf_pet_init(kf_pet_state *state) {
    state->hunger_mp = KF_PET_MILLIPERCENT_MAX;
    state->happiness_mp = KF_PET_MILLIPERCENT_MAX;
    state->energy_mp = KF_PET_MILLIPERCENT_MAX;
    state->poop_count = 0u;
    state->seconds_until_next_poop = 0u;  /* set on first advance */
    state->dirtiness_mp = 0u;
    state->neglect_seconds = 0u;
    state->sick = false;
    state->last_advanced.valid = false;
    state->last_advanced.epoch_seconds = 0;
    state->stage = KF_PET_STAGE_EGG;
    state->teen_form = 0u;
    state->adult_branch = 0u;
    state->stage_elapsed_seconds = 0u;
    state->care_integral_mp_seconds = 0u;
    state->hunger_integral_mp_seconds = 0u;
    state->happiness_integral_mp_seconds = 0u;
    state->energy_integral_mp_seconds = 0u;
    state->care_recency_window_seconds = 0u;

    /* ADR 0023: rolled once, here, and never touched again for the rest
     * of the pet's life (see kf/pet.h's header comment on kf_pet_state's
     * base_trait). kf_rng_below(), not kf_entropy() directly -- the
     * game-visible, deterministic RNG kf/rng.h documents as exactly what
     * "anything the pet ... observes" should come from, so this stays
     * repeatable across a save/load and identical run to run under a
     * pinned seed, the same property every other kf_rng_below() call in
     * this codebase already relies on. Requires kf_rng_seed() to already
     * have run before the first pet a process creates -- normally once at
     * boot from the entropy HAL (kf/app.cpp's kf_demo_init() call) -- the
     * same ordering requirement this file now shares with every other
     * kf_rng consumer, not a new one it introduces. */
    state->base_trait =
        static_cast<uint8_t>(kf_rng_below(KF_PET_BASE_TRAIT_COUNT));

    state->care_actions_taken = 0u;
}

void kf_pet_advance(kf_pet_state *state, const kf_pet_config *config,
                     uint32_t elapsed_seconds) {
    uint32_t remaining = elapsed_seconds;

    /* Bounded by the number of remaining life stages (at most 4: egg,
     * baby, child, teen -- adult is terminal and handled after the loop),
     * never by elapsed_seconds itself. See kf/pet.h's header comment on
     * kf_pet_advance() and this file's own header comment. */
    while (remaining > 0u && state->stage != KF_PET_STAGE_ADULT) {
        const uint32_t duration = stage_duration_seconds(config, state->stage);
        const uint32_t elapsed_in_stage =
            state->stage_elapsed_seconds > 0xFFFFFFFFull
                ? 0xFFFFFFFFu
                : static_cast<uint32_t>(state->stage_elapsed_seconds);
        const uint32_t time_left_in_stage =
            duration > elapsed_in_stage ? duration - elapsed_in_stage : 0u;
        const uint32_t segment =
            remaining < time_left_in_stage ? remaining : time_left_in_stage;

        if (segment > 0u) {
            apply_stage_segment(state, config, segment);
            state->stage_elapsed_seconds += segment;
            remaining -= segment;
        }

        if (state->stage_elapsed_seconds >= duration) {
            advance_to_next_stage(state);
        } else {
            /* This segment used up all of `remaining` without finishing
             * the stage -- nothing left to do this call. */
            break;
        }
    }

    /* Adult is terminal: no further stage transition, but needs keep
     * decaying for whatever time is left. apply_stage_segment() already
     * knows Adult does not feed a branch choice (stage_feeds_a_branch_
     * choice() above), so this does not pointlessly grow an accumulator
     * nothing will ever read. */
    if (remaining > 0u) {
        apply_stage_segment(state, config, remaining);
        state->stage_elapsed_seconds += remaining;
    }
}

void kf_pet_feed(kf_pet_state *state, const kf_pet_config *config) {
    if (state->care_actions_taken < UINT32_MAX) {
        state->care_actions_taken++;
    }
    state->hunger_mp = clamp_add(state->hunger_mp, kCareBoostMp);

    /* Eating brings the next mess forward. Only ever shortens the wait --
     * feeding repeatedly should not be able to push mess further away.
     * seconds_until_next_poop == 0 means "no timer started yet" (see
     * kf_pet_init()/apply_stage_segment()'s identical sentinel handling),
     * not "a poop is due this instant", so that case is resolved to the
     * default interval first -- otherwise feeding a never-advanced pet
     * would look like it was already about to poop and skip the shorten
     * below entirely. */
    if (state->seconds_until_next_poop == 0u) {
        state->seconds_until_next_poop = config->poop_interval_seconds;
    }
    if (state->seconds_until_next_poop > config->poop_interval_after_feed_seconds) {
        state->seconds_until_next_poop = config->poop_interval_after_feed_seconds;
    }
}

void kf_pet_play(kf_pet_state *state) {
    if (state->care_actions_taken < UINT32_MAX) {
        state->care_actions_taken++;
    }
    state->happiness_mp = clamp_add(state->happiness_mp, kCareBoostMp);
}

void kf_pet_rest(kf_pet_state *state) {
    if (state->care_actions_taken < UINT32_MAX) {
        state->care_actions_taken++;
    }
    state->energy_mp = clamp_add(state->energy_mp, kCareBoostMp);
}

void kf_pet_clean(kf_pet_state *state) {
    if (state->care_actions_taken < UINT32_MAX) {
        state->care_actions_taken++;
    }
    state->poop_count = 0u;
    state->dirtiness_mp = 0u;
}

/* ADR 0023: a pure query over the three whole-life accumulators, computed
 * fresh every call rather than cached anywhere -- see kf/pet.h's header
 * comment on kf_pet_state for why. Comparing the three raw accumulator
 * totals directly (rather than dividing each by elapsed time first to get
 * a true average) is valid because all three accumulate over the
 * identical span of time, segment for segment, via the same
 * accumulate_personality() call above -- there is no per-need difference
 * in "how many seconds were counted" for this to normalise away. */
uint8_t kf_pet_dominant_care_trait(const kf_pet_state *state) {
    uint64_t best = state->hunger_integral_mp_seconds;
    uint8_t best_index = 0u; /* hunger -- also the tie-break/all-zero default */
    if (state->happiness_integral_mp_seconds > best) {
        best = state->happiness_integral_mp_seconds;
        best_index = 1u;
    }
    if (state->energy_integral_mp_seconds > best) {
        best = state->energy_integral_mp_seconds;
        best_index = 2u;
    }
    return best_index;
}

void apply_stage_segment_for_test(kf_pet_state *state,
                                   const kf_pet_config *config,
                                   uint32_t segment_seconds) {
    apply_stage_segment(state, config, segment_seconds);
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
        KF_LOGI(TAG, "save could not be loaded, starting a fresh pet instead");
        kf_pet_init(state);
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
