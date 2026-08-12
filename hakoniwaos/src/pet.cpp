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

#include "kf/clock.h"
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
 * dirtiness_mp added), to 6 with sickness (neglect_seconds and sick
 * added) -- a version-5 save has no notion of accumulated neglect at all,
 * so there is no honest default to fill in for it (zero would silently
 * un-sicken a creature that was ill the moment it was saved); refusing the
 * load and falling back to a fresh pet is the same accepted cost the
 * version-4/5 bumps already took, not a new one -- and to 7 with death
 * (dead added). A second bump in the same branch is correct and cheap: a
 * save written between the sickness and death work is a real file on a
 * real developer's disk with a genuinely different layout, and versions
 * exist precisely so that is refused rather than misread. Bumped again to
 * 8 with care variations (docs/superpowers/plans/2026-08-09-care-
 * variations.md): `last_reaction` and `last_care_action` were added. And to
 * 9 with sleep (docs/superpowers/plans/2026-08-13-screens-clock-sleep.md's
 * Task 6, ADR 0048): `asleep` was added. And to 10 with the 2026-08-11
 * bedtime-behaviour extension (ADR 0052): `tucked_in` was added. And to 11
 * with the 2026-08-11 overnight-floor extension (ADR 0053):
 * `hunger_floor_mp`, `happiness_floor_mp`, `energy_floor_mp` and
 * `dirtiness_cap_mp` were added.
 * kf_pet_deserialize() refuses to load anything written by a different
 * version rather than guessing at a layout that changed, see unpack()
 * below. */
constexpr uint8_t kSaveVersion = 11;

/* Sleep, per docs/superpowers/specs/2026-08-09-core-care-loop-design.md's
 * "Sleep, settled". Night is a FIXED clock-time window, not config -- the
 * spec is explicit that Core does not re-derive kf_clock_seconds_in_daily_
 * window()'s own arithmetic, and a per-pet bedtime is not something either
 * the spec or the plan asked for. */
constexpr uint8_t kNightStartHour = 22u;
constexpr uint8_t kNightEndHour = 7u;

/* The waking-fraction constant the spec calls for by name: "Build it as a
 * single named constant." Nights (22:00-07:00) are a fixed 9 of every 24
 * hours, so the fraction of a day actually available to accrue neglect is
 * (24-9)/24 = 15/24. An integer numerator/denominator pair rather than one
 * value, for the same reason every other rate in this file is computed in
 * integer arithmetic with a uint64_t intermediate (see the file header
 * comment): no float anywhere in hakoniwaos/.
 *
 * WHAT this is used for -- ADR 0048 records the reasoning at length; the
 * short version is docs/superpowers/specs/2026-08-09-core-care-loop-
 * design.md's own: "because nights do not accrue neglect, sickness and
 * death arrive on roughly fifteen hours a day rather than twenty-four...
 * compress it, so a waking day still costs a full day's worth." Applied to
 * the THRESHOLDS (sickness_onset_seconds, sickness_death_seconds), never to
 * the rate neglect_seconds climbs at -- see apply_stage_segment() below for
 * why. */
constexpr uint32_t kWakingFractionNumerator = 15u;
constexpr uint32_t kWakingFractionDenominator = 24u;

/* The drowsy window (ADR 0052, the 2026-08-11 bedtime-behaviour extension):
 * the KF_PET_DROWSY_WINDOW_SECONDS immediately before kNightStartHour --
 * Chris's own words, "extend the 'drowsy' timeframe to start 10 minutes
 * before actual bedtime". Derived from kNightStartHour just above, not a
 * second hardcoded "22" -- see kf_pet_drowsy() below. Ten minutes is the
 * REQUESTED value, not a tuning guess like the config-table figures further
 * down this file, so it is a plain compile-time constant rather than a
 * kf_pet_config field, the same treatment the night window's own hours get. */
constexpr uint32_t kDrowsyWindowSeconds = 10u * 60u;

/* ADR 0053 (the 2026-08-11 overnight-floor extension): Chris, after testing
 * sleep and tuck-in, ruled that a pet's needs decaying to zero -- and poop
 * still piling up -- while it slept defeated the entire point of sleep
 * existing. "The pet's needs can never drop to absolute 0 while it's
 * asleep, not even when it's sick."
 *
 * Four bands, selected by two booleans (tucked in, unwell), each a random
 * value drawn independently per need from a fixed-width range -- Chris's own
 * words give the width for every band as the same 10 percentage points
 * ("a range of 10% of 70%... anywhere between 60% and 70%"), so the width is
 * ONE shared constant and each band supplies only its minimum.
 *
 * COMPILE-TIME CONSTANTS, NOT kf_pet_config, unlike most of this file's
 * other "feel" numbers (care_boost_liked_mp, tuck_in_wake_bonus_mp, etc.) --
 * a deliberate call, flagged here rather than silently made: these four
 * bands are closer in kind to kNightStartHour/kDrowsyWindowSeconds just
 * above (a fixed GLOBAL rule about what a night does) than to a per-pet care
 * number a cartridge author tunes. If Chris wants per-pet tuning later, this
 * is a small, mechanical move into kf_pet_config, not a redesign. */
constexpr kf_pet_millipercent kOvernightFloorBandSpanMp = 10000u; /* 10% */
constexpr kf_pet_millipercent kOvernightFloorWellTuckedInMinMp = 60000u;    /* 60-70% */
constexpr kf_pet_millipercent kOvernightFloorWellNotTuckedInMinMp = 50000u; /* 50-60% */
constexpr kf_pet_millipercent kOvernightFloorUnwellTuckedInMinMp = 40000u;  /* 40-50% */
constexpr kf_pet_millipercent kOvernightFloorUnwellNotTuckedInMinMp = 20000u; /* 20-30% */

/* "Unwell" is Chris's word, undefined by him beyond the two examples ("sick
 * or not doing well") -- this is this task's own inference, a starting
 * definition explicitly open to tuning, not a spec quote: sick, OR any of
 * the three needs already below this threshold, AT THE MOMENT the creature
 * falls asleep. 25% matches the same "starting to get hungry" neighbourhood
 * kf/pet.h's KF_PET_WANT_*_ON_MP constants already use for an unrelated
 * signal (the attention want) -- a separate named constant here rather than
 * reusing theirs, because the two concepts (what the player is nagged
 * about while awake, and what makes a night's sleep protection more
 * generous) are free to diverge later even though they happen to agree on
 * the day this was written. */
constexpr kf_pet_millipercent kUnwellNeedThresholdMp = 25000u; /* 25% */

/* The overnight dirtiness CAP -- ADR 0053's second, explicitly-flagged
 * inference: Chris named hunger/happiness/energy, not dirtiness, but left
 * alone a pet can wake filthy enough to be pushed toward sickness, directly
 * undercutting the point of this whole change. Mirrors the needs bands'
 * shape (same two booleans, well/tucked-in wakes least dirty) but a CEILING
 * or a random RANGE: a fixed value per band was judged sufficient for one
 * visible bar that only ever rises, where the three needs got a rolled
 * range each. Numbers picked, not derived -- see the ADR. */
constexpr kf_pet_millipercent kOvernightDirtinessCapWellTuckedInMp = 25000u;    /* 25% */
constexpr kf_pet_millipercent kOvernightDirtinessCapWellNotTuckedInMp = 40000u; /* 40% */
constexpr kf_pet_millipercent kOvernightDirtinessCapUnwellTuckedInMp = 45000u;  /* 45% */
constexpr kf_pet_millipercent kOvernightDirtinessCapUnwellNotTuckedInMp = 60000u; /* 60% */

/* One independent kf_rng_below() draw -- called three separate times, once
 * per need, so hunger/happiness/energy each get their OWN random floor
 * rather than one roll copied into all three. kf_rng_below(), not the
 * entropy HAL directly, for the identical reason kf_pet_init()'s base_trait
 * roll already gives (kf/rng.h): the game-visible, save/replay-deterministic
 * RNG this codebase already uses everywhere else a pet observes randomness,
 * so a pinned seed reproduces the exact same night twice. */
kf_pet_millipercent roll_overnight_floor_mp(bool tucked_in, bool unwell) {
    kf_pet_millipercent band_min;
    if (unwell) {
        band_min = tucked_in ? kOvernightFloorUnwellTuckedInMinMp
                              : kOvernightFloorUnwellNotTuckedInMinMp;
    } else {
        band_min = tucked_in ? kOvernightFloorWellTuckedInMinMp
                              : kOvernightFloorWellNotTuckedInMinMp;
    }
    return static_cast<kf_pet_millipercent>(
        band_min + kf_rng_below(kOvernightFloorBandSpanMp + 1u));
}

/* Not randomised -- see kOvernightDirtinessCap*'s own comment above. */
kf_pet_millipercent overnight_dirtiness_cap_mp(bool tucked_in, bool unwell) {
    if (unwell) {
        return tucked_in ? kOvernightDirtinessCapUnwellTuckedInMp
                          : kOvernightDirtinessCapUnwellNotTuckedInMp;
    }
    return tucked_in ? kOvernightDirtinessCapWellTuckedInMp
                      : kOvernightDirtinessCapWellNotTuckedInMp;
}

/* The next epoch, at or after `from`, whose civil time is exactly
 * `hour`:00:00 -- shared, closed-form building block for both the bedtime
 * and the wake crossing tests below (ADR 0053 generalises the "find the
 * next kNightEndHour:00:00" closed-form test ADR 0052 already built for the
 * tuck-in bonus to a second hour, kNightStartHour, rather than duplicating
 * the civil-time arithmetic a second time). */
int64_t next_epoch_at_hour(int64_t from, uint8_t hour) {
    kf_civil c;
    kf_civil_from_epoch(from, &c);
    c.hour = hour;
    c.minute = 0u;
    c.second = 0u;
    int64_t epoch = kf_epoch_from_civil(&c);
    /* Strictly LESS than, not <=: if `from` itself already sits exactly on
     * the target hour, that IS the crossing this call is asking about --
     * ADR 0053's own overnight-floor roll needs this exact instant
     * recognised (a segment that starts precisely at kNightStartHour:00:00
     * with the creature still awake must roll its floor for THIS bedtime,
     * not push a whole day out to the next one and silently skip it for
     * the entire night in between). Every caller that reaches this
     * boundary via genuine continuous simulation never actually lands
     * exactly on it (the SEGMENT that reaches the boundary already updates
     * `state->asleep` for that same instant, so the next segment's own
     * start reads the already-updated flag rather than needing this
     * function's help) -- this only matters for a state built by hand
     * (a test, or a save edited/constructed directly) that starts already
     * sitting exactly on the hour. */
    if (epoch < from) {
        epoch += 24 * 60 * 60;
    }
    return epoch;
}

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

/* The preference table: for each base trait and action, which variation is
 * liked and which is disliked. The third is neutral by elimination, which
 * is why only two numbers are stored -- a full 6x4x3 table of reactions
 * could encode a creature with two favourites, and this cannot.
 *
 * Placeholder values, in the same spirit as the decay rates: the SHAPE is
 * the decision (every trait wants something different, and no two traits
 * want the same set of things), and the specific assignments are for
 * living with. Chris tunes this once there is a creature to tune it
 * against.
 *
 * Rows are base traits, columns are kf_pet_care_action in enum order. */
constexpr uint8_t kLikedVariation[KF_PET_BASE_TRAIT_COUNT]
                                 [KF_PET_CARE_ACTION_COUNT] = {
    {0u, 1u, 2u, 0u},
    {1u, 2u, 0u, 2u},
    {2u, 0u, 1u, 1u},
    {0u, 2u, 1u, 2u},
    {1u, 0u, 2u, 1u},
    {2u, 1u, 0u, 0u},
};

constexpr uint8_t kDislikedVariation[KF_PET_BASE_TRAIT_COUNT]
                                     [KF_PET_CARE_ACTION_COUNT] = {
    {1u, 2u, 0u, 1u},
    {2u, 0u, 1u, 0u},
    {0u, 1u, 2u, 2u},
    {2u, 1u, 0u, 0u},
    {0u, 2u, 1u, 2u},
    {1u, 0u, 2u, 1u},
};

/* Resolves the reaction, records it, and returns what this action is worth.
 * Shared by all four care actions: they differ in which need they raise,
 * not in how preference works. */
kf_pet_millipercent apply_care_reaction(kf_pet_state *state,
                                         const kf_pet_config *config,
                                         kf_pet_care_action action,
                                         uint8_t variation) {
    const uint8_t reaction =
        kf_pet_reaction_to(state->base_trait, action, variation);
    state->last_reaction = reaction;
    state->last_care_action = static_cast<uint8_t>(action);
    switch (reaction) {
    case KF_PET_REACTION_LIKED:
        return config->care_boost_liked_mp;
    case KF_PET_REACTION_DISLIKED:
        return config->care_boost_disliked_mp;
    default:
        return config->care_boost_neutral_mp;
    }
}

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

/* How many seconds into a segment of `segment` seconds this creature first
 * falls below a need threshold -- or `segment` if it never does. Called
 * BEFORE any decay is applied, on the segment's starting values.
 *
 * Exact rather than sampled, because the needs decay linearly at a known
 * rate, so the crossing point is one division instead of a search. That
 * precision is the entire reason this function exists: a device left in a
 * drawer overnight starts its segment with a healthy creature and ends it
 * with an empty one, and anything that merely inspects the two ends
 * concludes half the night was fine when in truth the needs ran out within
 * a few hours. Sampling was tried first and got exactly that wrong.
 *
 * Mess is deliberately NOT handled here -- see the caller. Poop count and
 * dirtiness climb rather than fall, and dirtiness's rate steps up with
 * every poop that lands, so there is no single closed-form crossing to
 * solve for.
 *
 * `effective_rate` is the three needs' rates AFTER the sickness multiplier
 * and drain, in hunger/happiness/energy order -- the rate actually applied
 * to this segment, not the stage's nominal one, or an ill creature's
 * crossing would be computed as later than it really is. */
uint32_t seconds_until_need_neglect(const kf_pet_state *state,
                                     const kf_pet_config *config,
                                     const uint32_t effective_rate[3],
                                     uint32_t segment) {
    const uint32_t need[3] = {state->hunger_mp, state->happiness_mp,
                               state->energy_mp};
    uint32_t earliest = segment;
    for (unsigned i = 0u; i < 3u; ++i) {
        if (need[i] <= config->neglect_need_mp) {
            return 0u; /* already there before this segment started */
        }
        if (effective_rate[i] == 0u) {
            continue; /* a stage that does not decay this need at all */
        }
        const uint64_t crossing =
            (static_cast<uint64_t>(need[i] - config->neglect_need_mp) *
             3600ull) /
            effective_rate[i];
        if (crossing < earliest) {
            earliest = static_cast<uint32_t>(crossing);
        }
    }
    return earliest;
}

/* Adds `add` to a saturating uint32_t counter. */
uint32_t saturating_add_u32(uint32_t value, uint32_t add) {
    const uint64_t sum = static_cast<uint64_t>(value) + add;
    return sum > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(sum);
}

/* Scales a decay rate by the sickness multiplier. uint64_t intermediate for
 * the reason this file's header comment gives: no product here is computed
 * in uint32_t and hoped for. */
uint32_t sick_scaled_rate(uint32_t rate_mp_per_hour, bool sick,
                           const kf_pet_config *config) {
    if (!sick) {
        return rate_mp_per_hour;
    }
    const uint64_t scaled = static_cast<uint64_t>(rate_mp_per_hour) *
                             config->sick_decay_multiplier_percent / 100ull;
    return scaled > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(scaled);
}

/* Advances `state` past its current (already-complete) stage into the
 * next one, picking a branch and resetting the per-stage accumulators
 * where that applies. Only ever called once kf_pet_advance()'s loop has
 * confirmed stage_elapsed_seconds has reached the stage's full duration. */
void advance_to_next_stage(kf_pet_state *state,
                            const kf_pet_config *config) {
    switch (state->stage) {
    case KF_PET_STAGE_EGG:
        state->stage = KF_PET_STAGE_BABY;
        break;
    case KF_PET_STAGE_BABY:
        state->stage = KF_PET_STAGE_CHILD;
        break;
    case KF_PET_STAGE_CHILD: {
        /* Barely tended -> dust, regardless of which family the ordinary
         * bands would have picked. Checked BEFORE the ordinary branch
         * selection because it is not a quality judgement on the same
         * scale: the other four families are "how well was it raised", and
         * this is "was it raised at all".
         *
         * This used to be `care_actions_taken == 0` -- literally never
         * touched, which is what the character bible's section 8 describes.
         * That stopped being reachable when Chris ruled that a creature
         * left alone dies like a Tamagotchi in a drawer: an untouched
         * creature now dies during CHILD, days before this branch point,
         * so the dust form would have been a documented character that no
         * player could ever obtain.
         *
         * So the condition became the nearest thing that survives: kept
         * alive and nothing more. A creature fed only when it was about to
         * die and otherwise ignored still averages almost nothing across
         * its whole childhood, which is the same story the bible tells --
         * neglect made visible -- reached by a route that does not require
         * the creature to be dead. */
        const uint64_t child_care_average_mp =
            state->stage_elapsed_seconds > 0u
                ? state->care_integral_mp_seconds / state->stage_elapsed_seconds
                : 0u;
        if (child_care_average_mp < config->dust_care_average_mp) {
            state->teen_form = KF_PET_TEEN_FORM_DUST;
        } else {
            state->teen_form = select_branch(state->care_integral_mp_seconds,
                                              state->stage_elapsed_seconds,
                                              KF_PET_TEEN_FORM_COUNT);
        }
        state->stage = KF_PET_STAGE_TEEN;
        break;
    }
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

/* ADR 0052: pays out the tuck-in bonus, once, if one is owed -- called only
 * from the exact instant apply_stage_segment() below observes `asleep` flip
 * from true to false (the asleep -> awake transition kf_pet_tuck_in()'s own
 * header comment promises the bonus at). A no-op if the creature was never
 * tucked in (state->tucked_in already false), which is what makes a plain
 * self-slept creature -- one that fell asleep and woke on its own, nobody
 * ever pressed B during its drowsy window -- unaffected: this function does
 * nothing at all for it, not "add zero", so there is no observable
 * difference in behaviour for the common case.
 *
 * Applied to all THREE needs, not just one -- Chris's own words describe a
 * general "boost in care needs", not a single bar -- via the same clamp_add()
 * every care action already uses, so a creature that is already near-full on
 * one need simply cannot bank the bonus past KF_PET_MILLIPERCENT_MAX, same
 * as feeding a full creature does nothing extra.
 *
 * Clears `tucked_in` unconditionally on the way out (even though the early
 * return above already guarantees it is true whenever this reaches here) --
 * this is what makes the bonus a one-time payout: the very next asleep ->
 * awake transition, with the flag now false, is this function's own early
 * return, not a second bonus. */
void apply_tuck_in_bonus_if_due(kf_pet_state *state,
                                 const kf_pet_config *config) {
    if (!state->tucked_in) {
        return;
    }
    state->hunger_mp = clamp_add(state->hunger_mp, config->tuck_in_wake_bonus_mp);
    state->happiness_mp =
        clamp_add(state->happiness_mp, config->tuck_in_wake_bonus_mp);
    state->energy_mp = clamp_add(state->energy_mp, config->tuck_in_wake_bonus_mp);
    state->tucked_in = false;
}

/* Applies decay (and, on the stages where it matters, accumulates the care
 * integral and/or the personality accumulators) for exactly `segment`
 * seconds, all still within the SAME stage -- kf_pet_advance()'s loop
 * never lets a segment cross a stage boundary, so this never needs to know
 * about stages other than the current one.
 *
 * `have_clock`/`segment_start_epoch`: the wall-clock instant this segment
 * begins at, IF the wall clock is known -- see kf_pet_advance()'s own
 * comment for where `segment_start_epoch` comes from (a cursor mirroring
 * `state->last_advanced`) and kf_pet_state::last_advanced's comment for why
 * "known" means `last_advanced.valid`. When `have_clock` is false,
 * `segment_start_epoch` is meaningless and every sleep-related computation
 * below is skipped entirely -- state->asleep stays whatever it already
 * was (false, for any state that has never gone through kf_pet_load_and_
 * advance() or apply_stage_segment_for_test()'s deliberately clock-less
 * call), neglect accrual is untouched, and the sickness/death thresholds
 * are the raw config values. This is what keeps every pre-sleep check in
 * this codebase (hokorimaru_check among them -- see docs/superpowers/
 * plans/2026-08-13-screens-clock-sleep.md's Task 6 requirement 7) exactly
 * as it was: none of them ever establish a wall clock, so sleep is
 * completely inert for them, not approximately inert. */
void apply_stage_segment(kf_pet_state *state, const kf_pet_config *config,
                          uint32_t segment, bool have_clock,
                          int64_t segment_start_epoch) {
    if (state->dead) {
        /* Nothing decays, nothing accumulates, no mess arrives. The same
         * shape as the egg exemption below and for a comparable reason:
         * there is no simulation left to run. */
        return;
    }

    if (state->stage == KF_PET_STAGE_EGG) {
        /* No care needed as an egg -- see kf/pet.h's header comment and
         * kf_pet_init(): needs stay at whatever they were (full, for a
         * freshly-initialised pet) until the egg hatches. Personality
         * does not accumulate here either, for the same reason
         * care_integral_mp_seconds does not: nothing has been cared for
         * yet, so there is nothing meaningful to weight by.
         *
         * EGGS DO NOT SLEEP (docs/superpowers/plans/2026-08-13-screens-
         * clock-sleep.md's Task 6 requirement 9, decided in ADR 0048):
         * there is no egg_sleeping art in the shipped pack, and an egg has
         * nothing to be tired from in the first place -- it is "no care
         * needed as an egg" taken to its logical end. Returning here
         * before ever reaching the sleep computation below is what makes
         * that true: state->asleep simply never gets set for an egg, so it
         * stays at whatever kf_pet_init() left it (false). */
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
    const uint64_t dirtiness_before_mp = state->dirtiness_mp;
    const bool neglected_before = is_neglected(state, config);

    /* ADR 0053: how many of this segment's seconds fall inside the night
     * window, closed-form over the WHOLE [segment_start, segment_start +
     * segment) span regardless of how many nights it spans -- the same
     * kf_clock_seconds_in_daily_window() building block the neglect-pause
     * above already relies on for the identical "correct for a fortnight,
     * not just one night" property. Drives two things below: no NEW poops
     * generate during asleep seconds (seconds_until_next_poop simply does
     * not count them), and dirtiness rises at the reduced, no-per-poop-term
     * rate for exactly those seconds. Zero without a clock, so both of
     * those stay completely inert for it -- the same "inert without a
     * clock" rule every sleep-derived computation in this function
     * follows. */
    uint32_t asleep_seconds_in_segment = 0u;
    if (have_clock) {
        const int64_t range_end =
            segment_start_epoch + static_cast<int64_t>(segment);
        const int64_t overlap = kf_clock_seconds_in_daily_window(
            segment_start_epoch, range_end, kNightStartHour, kNightEndHour);
        asleep_seconds_in_segment =
            overlap > 0 ? static_cast<uint32_t>(overlap) : 0u;
    }
    const uint32_t awake_seconds_in_segment =
        segment - asleep_seconds_in_segment;

    /* Where in this segment the needs run out, computed from the rates
     * actually applied below (sickness included) and from the values as
     * they are right now, before any of this segment's decay. */
    const uint32_t effective_rate[3] = {
        sick_scaled_rate(rates.hunger_mp_per_hour, state->sick, config),
        saturating_add_u32(
            sick_scaled_rate(rates.happiness_mp_per_hour, state->sick, config),
            state->sick ? config->sick_happiness_drain_mp_per_hour : 0u),
        sick_scaled_rate(rates.energy_mp_per_hour, state->sick, config)};
    const uint32_t need_neglect_at =
        seconds_until_need_neglect(state, config, effective_rate, segment);

    const bool feeds_branch = stage_feeds_a_branch_choice(state->stage);
    uint64_t average_before_mp = 0u;
    if (feeds_branch) {
        average_before_mp =
            (hunger_before_mp + happiness_before_mp + energy_before_mp) / 3u;
    }

    state->hunger_mp = apply_decay(
        state->hunger_mp,
        sick_scaled_rate(rates.hunger_mp_per_hour, state->sick, config),
        segment);
    state->happiness_mp = apply_decay(
        state->happiness_mp,
        sick_scaled_rate(rates.happiness_mp_per_hour, state->sick, config),
        segment);
    state->energy_mp = apply_decay(
        state->energy_mp,
        sick_scaled_rate(rates.energy_mp_per_hour, state->sick, config),
        segment);

    /* The extra happiness drain, on top of the scaled decay above. Routed
     * through apply_decay() rather than done by hand so it clamps at zero
     * the same way everything else does. */
    if (state->sick) {
        state->happiness_mp =
            apply_decay(state->happiness_mp,
                        config->sick_happiness_drain_mp_per_hour, segment);
    }

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
     * otherwise spin forever.
     *
     * ADR 0053: "the pet WILL NOT poop while it's asleep" -- and, just as
     * important, waking must not dump a whole night's worth of suppressed
     * interval at once. Both fall out of one substitution: every use of
     * `segment` below becomes `awake_seconds_in_segment`, so the counter
     * counts down (and the interval math resolves) as if the asleep seconds
     * simply were never on the clock at all -- the identical "pause, do not
     * reduce" treatment neglect_seconds already gets in the block below.
     * Because a plain countdown only cares how many awake seconds elapsed,
     * not WHEN within the segment they fell, this is exact, not an
     * approximation, and it is correct for a segment spanning any number of
     * nights, not just one -- unlike the tuck-in bonus and the overnight
     * floor/cap below, which only resolve the FIRST night-crossing exactly
     * (see this function's own "Sleep:" comment). */
    if (state->seconds_until_next_poop == 0u) {
        state->seconds_until_next_poop = config->poop_interval_seconds;
    }
    if (config->poop_interval_seconds == 0u) {
        /* Misconfigured: no interval to count down, so no mess rather than
         * an infinite loop. kf_pet_default_config() never produces this,
         * but a corrupted or hand-built config must still degrade safely. */
        state->seconds_until_next_poop = 0u;
    } else if (awake_seconds_in_segment >= state->seconds_until_next_poop) {
        const uint32_t after_first =
            awake_seconds_in_segment - state->seconds_until_next_poop;
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
        state->seconds_until_next_poop -= awake_seconds_in_segment;
    }

    /* Dirtiness rises with time, faster the more mess is waiting -- reads
     * state->poop_count AFTER the block above, so this segment's own new
     * poops (if any) already count towards this segment's dirtying.
     * Saturates at full rather than wrapping.
     *
     * ADR 0053: "existing poop has no effect while asleep... it would just
     * make dirtiness decay faster until cleaned up after it wakes." Two
     * rates rather than one: the per-poop term is dropped for exactly the
     * asleep seconds (weighted by kf_clock_seconds_in_daily_window()'s
     * overlap, the same closed-form, any-number-of-nights technique the
     * neglect pause below already uses -- a genuine rate reduction, correct
     * regardless of how many nights this segment spans, unlike the
     * set-point cap applied below it). The base rate (dirtiness_rise_mp_
     * per_hour) is UNCHANGED while asleep -- only the per-poop multiplier is
     * suppressed, so a pet with poop waiting at bedtime still dirties, just
     * without the acceleration, exactly as Chris described. */
    const uint64_t dirtiness_rise_per_hour_awake =
        static_cast<uint64_t>(config->dirtiness_rise_mp_per_hour) +
        (static_cast<uint64_t>(config->dirtiness_rise_per_poop_mp_per_hour) *
         state->poop_count);
    const uint64_t dirtiness_rise_per_hour_asleep =
        static_cast<uint64_t>(config->dirtiness_rise_mp_per_hour);
    const uint64_t dirtiness_rise =
        (dirtiness_rise_per_hour_awake *
         static_cast<uint64_t>(awake_seconds_in_segment)) /
            3600u +
        (dirtiness_rise_per_hour_asleep *
         static_cast<uint64_t>(asleep_seconds_in_segment)) /
            3600u;
    if (dirtiness_rise >= KF_PET_MILLIPERCENT_MAX - state->dirtiness_mp) {
        state->dirtiness_mp = KF_PET_MILLIPERCENT_MAX;
    } else {
        state->dirtiness_mp = static_cast<kf_pet_millipercent>(
            state->dirtiness_mp + dirtiness_rise);
    }

    accumulate_personality(state, config, segment, hunger_before_mp,
                            happiness_before_mp, energy_before_mp);

    /* Sleep: is the creature asleep AS OF THE END of this segment -- i.e.
     * right now, once this call returns. A single point-in-time query
     * against kf_clock_seconds_in_daily_window() (a one-second probe
     * starting at the instant in question) rather than a second, separately
     * written "is this hour a night hour" check: Core does not re-derive
     * the window (docs/superpowers/specs/2026-08-09-core-care-loop-
     * design.md's "Sleep, settled"), including for a yes/no membership
     * test, not just the seconds-accounting below. Skipped (state->asleep
     * left at whatever it already was) without a clock -- see this
     * function's own header comment. */
    if (have_clock) {
        const int64_t now_epoch =
            segment_start_epoch + static_cast<int64_t>(segment);
        const bool was_asleep_at_start = state->asleep;
        state->asleep = kf_clock_seconds_in_daily_window(
                             now_epoch, now_epoch + 1, kNightStartHour,
                             kNightEndHour) > 0;

        /* ADR 0052/0053: two closed-form crossing tests, both built on the
         * same next_epoch_at_hour() helper -- "did THIS segment contain the
         * instant the night begins" and "...the instant it ends". Neither is
         * "was state->asleep true before this call and false/true after":
         * that before/after comparison only catches a transition landing
         * exactly at a segment's own boundary, but kf_pet_advance()'s
         * segments are bounded by STAGE transitions, not night-window
         * crossings, so a single call can span an entire night start to
         * finish -- exactly what a real offline fast-forward does (see this
         * file's header comment, kf/pet.h's kf_pet_advance() comment, and
         * ADR 0052's own account of the bug this caught for the tuck-in
         * bonus). Both tests are correct for exactly the FIRST such crossing
         * inside the segment; a segment spanning several nights at once (a
         * pet away for a week) only resolves the first one precisely --
         * documented, not hidden, in ADR 0053. */
        const int64_t bedtime_epoch =
            next_epoch_at_hour(segment_start_epoch, kNightStartHour);
        const int64_t morning_epoch =
            next_epoch_at_hour(segment_start_epoch, kNightEndHour);
        const uint32_t seconds_to_bedtime =
            static_cast<uint32_t>(bedtime_epoch - segment_start_epoch);
        const uint32_t seconds_to_wake =
            static_cast<uint32_t>(morning_epoch - segment_start_epoch);

        const bool falls_asleep_this_segment =
            !was_asleep_at_start && bedtime_epoch <= now_epoch;
        const bool sleeping_this_segment =
            was_asleep_at_start || falls_asleep_this_segment;
        const bool wakes_this_segment =
            sleeping_this_segment && morning_epoch <= now_epoch;

        /* ADR 0053: roll fresh overnight floors/cap exactly once, at THIS
         * segment's own bedtime crossing -- one independent kf_rng_below()
         * draw per need (never one value copied into all three), judged
         * from the needs AS OF THE BEDTIME INSTANT specifically (not the
         * segment's start, which for an offline gap can be hours before
         * bedtime, and not its end, which has already decayed past it). A
         * creature already asleep at this segment's start
         * (was_asleep_at_start) reuses whatever was rolled at ITS bedtime,
         * in an earlier call -- these four fields are saved specifically so
         * that reuse survives a reload (the device being switched off
         * overnight is the whole case that matters). */
        if (falls_asleep_this_segment) {
            const kf_pet_millipercent hunger_at_bedtime = apply_decay(
                static_cast<kf_pet_millipercent>(hunger_before_mp),
                effective_rate[0], seconds_to_bedtime);
            const kf_pet_millipercent happiness_at_bedtime = apply_decay(
                static_cast<kf_pet_millipercent>(happiness_before_mp),
                effective_rate[1], seconds_to_bedtime);
            const kf_pet_millipercent energy_at_bedtime = apply_decay(
                static_cast<kf_pet_millipercent>(energy_before_mp),
                effective_rate[2], seconds_to_bedtime);
            /* "Unwell": sick, or any need already below kUnwellNeedThresholdMp,
             * AT BEDTIME -- see that constant's own comment for why this
             * predicate's exact shape is this task's inference, not a spec
             * quote. state->sick here is deliberately the value carried INTO
             * this call: this segment's own neglect/sickness verdict is not
             * decided until the block below, and bedtime already happened
             * earlier in THIS same segment for an offline jump that starts
             * the evening before and falls asleep partway through. */
            const bool unwell = state->sick ||
                                 hunger_at_bedtime < kUnwellNeedThresholdMp ||
                                 happiness_at_bedtime < kUnwellNeedThresholdMp ||
                                 energy_at_bedtime < kUnwellNeedThresholdMp;
            const bool tucked = state->tucked_in;

            state->hunger_floor_mp = roll_overnight_floor_mp(tucked, unwell);
            state->happiness_floor_mp = roll_overnight_floor_mp(tucked, unwell);
            state->energy_floor_mp = roll_overnight_floor_mp(tucked, unwell);
            state->dirtiness_cap_mp = overnight_dirtiness_cap_mp(tucked, unwell);
        }

        if (sleeping_this_segment) {
            if (wakes_this_segment) {
                /* This segment carries the creature past kNightEndHour:00 --
                 * compute what each need/dirtiness would be EXACTLY AT the
                 * wake instant, apply the floor/cap there (a SET-POINT:
                 * max(decayed, floor) for the needs, min(risen, cap) for
                 * dirtiness -- see kf_pet_state::hunger_floor_mp's own
                 * comment on why this is a set-point and not a pure floor),
                 * then let ordinary, UNPROTECTED decay continue for whatever
                 * of the segment remains after waking. Clamping the
                 * segment's FINAL value instead (skipping this split) would
                 * incorrectly protect hours of ordinary DAYTIME decay that
                 * happened after the creature woke -- the identical reason
                 * ADR 0052 could not use a before/after comparison for the
                 * tuck-in bonus either. */
                const uint32_t remaining = segment - seconds_to_wake;

                const kf_pet_millipercent hunger_at_wake = apply_decay(
                    static_cast<kf_pet_millipercent>(hunger_before_mp),
                    effective_rate[0], seconds_to_wake);
                const kf_pet_millipercent happiness_at_wake = apply_decay(
                    static_cast<kf_pet_millipercent>(happiness_before_mp),
                    effective_rate[1], seconds_to_wake);
                const kf_pet_millipercent energy_at_wake = apply_decay(
                    static_cast<kf_pet_millipercent>(energy_before_mp),
                    effective_rate[2], seconds_to_wake);

                const kf_pet_millipercent hunger_floored =
                    hunger_at_wake > state->hunger_floor_mp
                        ? hunger_at_wake
                        : state->hunger_floor_mp;
                const kf_pet_millipercent happiness_floored =
                    happiness_at_wake > state->happiness_floor_mp
                        ? happiness_at_wake
                        : state->happiness_floor_mp;
                const kf_pet_millipercent energy_floored =
                    energy_at_wake > state->energy_floor_mp
                        ? energy_at_wake
                        : state->energy_floor_mp;

                state->hunger_mp =
                    apply_decay(hunger_floored, effective_rate[0], remaining);
                state->happiness_mp = apply_decay(
                    happiness_floored, effective_rate[1], remaining);
                state->energy_mp =
                    apply_decay(energy_floored, effective_rate[2], remaining);

                /* Dirtiness at the wake instant needs its own two-rate split
                 * over [segment_start, wake): the asleep portion of THAT
                 * sub-range runs from bedtime (or the segment's own start,
                 * if it began already asleep) up to wake. */
                const uint32_t sub_asleep_seconds =
                    falls_asleep_this_segment
                        ? seconds_to_wake - seconds_to_bedtime
                        : seconds_to_wake;
                const uint32_t sub_awake_seconds =
                    seconds_to_wake - sub_asleep_seconds;
                const uint64_t dirtiness_rise_to_wake =
                    (dirtiness_rise_per_hour_awake *
                     static_cast<uint64_t>(sub_awake_seconds)) /
                        3600u +
                    (dirtiness_rise_per_hour_asleep *
                     static_cast<uint64_t>(sub_asleep_seconds)) /
                        3600u;
                const kf_pet_millipercent dirtiness_at_wake = clamp_add(
                    static_cast<kf_pet_millipercent>(dirtiness_before_mp),
                    static_cast<kf_pet_millipercent>(dirtiness_rise_to_wake));
                const kf_pet_millipercent dirtiness_capped_at_wake =
                    dirtiness_at_wake < state->dirtiness_cap_mp
                        ? dirtiness_at_wake
                        : state->dirtiness_cap_mp;
                /* Awake rate resumes for the remainder -- poop generation
                 * and the per-poop dirtiness term are both back in play the
                 * moment the creature wakes (state->poop_count already
                 * reflects the whole segment's awake-only generation, see
                 * the mess block above). */
                const uint64_t dirtiness_rise_after_wake =
                    (dirtiness_rise_per_hour_awake *
                     static_cast<uint64_t>(remaining)) /
                    3600u;
                state->dirtiness_mp = clamp_add(
                    dirtiness_capped_at_wake,
                    static_cast<kf_pet_millipercent>(
                        dirtiness_rise_after_wake));

                /* Clear on waking (kf_pet_state::hunger_floor_mp's own
                 * comment): 0 is never a real floor/cap, so this doubles as
                 * "no floor currently active" without a separate flag. */
                state->hunger_floor_mp = 0u;
                state->happiness_floor_mp = 0u;
                state->energy_floor_mp = 0u;
                state->dirtiness_cap_mp = 0u;
            } else {
                /* Still asleep at the end of this segment: DELIBERATELY NO
                 * CLAMP HERE -- this is the fix for the defect ADR 0053
                 * itself now documents (docs/reviews/2026-08-12-sleep-
                 * stack-audit.md finding 1). The floor/cap is a WAKE-INSTANT
                 * set-point ("what the creature wakes up with"), not a
                 * level held continuously through the night, and clamping
                 * it here every segment applied it as the latter by
                 * accident. `kf_pet_session_frame()` flushes live play
                 * every KF_PET_SESSION_FLUSH_SECONDS (30s), so a real
                 * overnight session used to hit this branch roughly 1,080
                 * times a night -- re-clamping state->hunger_mp etc. up to
                 * the floor on EVERY one of those segments, not just once.
                 * Because care_integral_mp_seconds (above) accumulates each
                 * segment's *start-of-segment* value, every one of those
                 * re-clamps fed a floor-level number into the CHILD-stage
                 * care average on the very next segment -- inflating it by
                 * as much as ~2.85x for an identical, completely unattended
                 * pet purely depending on whether a wall clock happened to
                 * be established (the audit's finding 1 numbers). The needs
                 * (and dirtiness, for symmetry -- Chris did not distinguish
                 * the cap from the floors here) are simply left to decay/
                 * rise unprotected for the rest of the night; the ONE
                 * legitimate clamp is the wake-instant one in the
                 * `wakes_this_segment` branch above, which is unchanged and
                 * still correct. A real consequence, not hidden: the bars
                 * now visibly drain overnight on a device left running, and
                 * jump back up to the floor/cap only at the wake instant. */
            }
        }

        /* ADR 0052: pay the tuck-in bonus if THIS SEGMENT contains the
         * asleep -> awake transition -- reuses wakes_this_segment computed
         * above rather than a second, separate epoch calculation. AFTER the
         * floor/cap block above, deliberately: the bonus is an ADDITIONAL
         * reward on top of whatever the floor already restored, not a
         * replacement for it -- a tucked-in creature gets BOTH the more
         * generous tucked-in floor band AND this bonus, which is exactly
         * Chris's "needs less care in the morning if you tucked it in"
         * stacking with the floor's own "never zero" guarantee for every
         * creature regardless of tucked-in status.
         *
         * Deliberate direction, matching kf_pet_tuck_in()'s own promise:
         * ONLY the asleep -> awake edge pays, never awake -> asleep. A
         * deliberate early wake (kf_pet_wake()) sets state->asleep = false
         * directly and does not come through here at all -- see that
         * function's own header comment for why a creature woken early
         * keeps its tucked_in flag until whichever night it actually sees
         * this transition, rather than being paid immediately. */
        if (wakes_this_segment) {
            apply_tuck_in_bonus_if_due(state, config);
        }
    }

    /* Neglect, and the illness it turns into. Evaluated LAST in the
     * segment, because a segment may be a fortnight and what matters is
     * what happened DURING it, not what was true when it began.
     *
     * How much of the segment counts as neglect, in three cases:
     *
     *   Already neglected when the segment began -- all of it. Exact, not
     *   an approximation: nothing recovers inside a segment. Needs only
     *   decay here and mess only grows, so a creature that starts a segment
     *   in trouble is in trouble for the whole of it. Care happens between
     *   segments, never within one.
     *
     *   The needs ran out partway through -- from that moment on. The
     *   crossing point is solved for exactly (seconds_until_need_neglect()
     *   above) rather than sampled, because sampling is what gets the
     *   overnight-in-a-drawer case wrong: full at one end, empty at the
     *   other, and any end-sampling rule calls that half a night of neglect
     *   when the needs actually ran out in the first few hours.
     *
     *   Only the mess went critical -- half the segment. The one estimate
     *   left, because dirtiness has no closed-form crossing (its rate steps
     *   up with every poop that lands). It is also the least consequential:
     *   by the time filth alone is critical, the needs are long gone and
     *   the branch above has already claimed the segment.
     *
     * Whatever is left over counts as care, and works the accumulator back
     * down at the same rate it went up.
     *
     * NOTHING is exempt. An earlier version spared creatures that had
     * never been touched at all, to keep the character bible's dust form
     * reachable -- Chris overruled it: a creature left in a drawer runs its
     * needs down, calls out, and dies, exactly as the original Tamagotchi
     * did. That is the end state here too, and it is what makes the
     * babysitter hand-off worth building later.
     *
     * See kf_pet_advance()'s teen-branch selection for where the dust form
     * went instead.
     *
     * SLEEP, per docs/superpowers/specs/2026-08-09-core-care-loop-design.md's
     * "Sleep, settled": "the neglect clock pauses while asleep, but the
     * needs do not." The needs already decayed above, unconditionally --
     * that half needed no change at all. This half is the accrual pause:
     * in EVERY one of the three cases above, the neglected range this
     * segment identifies is always the LAST `neglected_for` seconds of the
     * segment (branch 1 is the whole segment; branch 2 is
     * [need_neglect_at, segment); branch 3's segment/2 estimate is, by the
     * same arithmetic, also a suffix) -- which is exactly why
     * `cared_for == segment - neglected_for` already equals that range's
     * own start offset, reused below rather than recomputed. Whatever part
     * of THAT specific range falls inside the night window
     * (kf_clock_seconds_in_daily_window() again -- never a second,
     * hand-rolled overlap test) is subtracted before it ever reaches the
     * accumulator: the neglect clock is frozen for exactly the seconds the
     * creature spent both neglected AND asleep, no more and no less. A
     * creature that is neglected only in the DAYTIME portion of a segment
     * that also spans part of a night is unaffected; one that is neglected
     * only overnight loses the whole thing. */
    {
        const bool neglected_after = is_neglected(state, config);

        uint32_t neglected_for = 0u;
        if (neglected_before) {
            neglected_for = segment;
        } else if (need_neglect_at < segment) {
            neglected_for = segment - need_neglect_at;
        } else if (neglected_after) {
            neglected_for = segment / 2u;
        }
        const uint32_t cared_for = segment - neglected_for;

        uint32_t neglected_for_awake = neglected_for;
        if (have_clock && neglected_for > 0u) {
            const int64_t range_start =
                segment_start_epoch + static_cast<int64_t>(cared_for);
            const int64_t range_end =
                segment_start_epoch + static_cast<int64_t>(segment);
            const int64_t asleep_overlap = kf_clock_seconds_in_daily_window(
                range_start, range_end, kNightStartHour, kNightEndHour);
            neglected_for_awake = asleep_overlap >= neglected_for
                                       ? 0u
                                       : neglected_for -
                                             static_cast<uint32_t>(
                                                 asleep_overlap);
        }

        state->neglect_seconds =
            saturating_add_u32(state->neglect_seconds, neglected_for_awake);
        state->neglect_seconds = state->neglect_seconds > cared_for
                                      ? state->neglect_seconds - cared_for
                                      : 0u;

        /* Compression: only alongside the pause above, and inert without a
         * clock for the identical reason. config->sickness_onset_seconds
         * and config->sickness_death_seconds themselves are UNTOUCHED --
         * only this READING of them is scaled, into a local, so the config
         * struct stays exactly what the caller configured. The
         * zero-sentinel check ("sickness_death_seconds == 0 means never")
         * is against the RAW config value, not the compressed one, so a
         * misconfigured but nonzero death threshold cannot round down to
         * an accidental zero and flip the sentinel's own meaning. */
        uint32_t effective_onset_seconds = config->sickness_onset_seconds;
        uint32_t effective_death_seconds = config->sickness_death_seconds;
        if (have_clock) {
            effective_onset_seconds = static_cast<uint32_t>(
                (static_cast<uint64_t>(config->sickness_onset_seconds) *
                 kWakingFractionNumerator) /
                kWakingFractionDenominator);
            effective_death_seconds = static_cast<uint32_t>(
                (static_cast<uint64_t>(config->sickness_death_seconds) *
                 kWakingFractionNumerator) /
                kWakingFractionDenominator);
        }

        if (state->neglect_seconds >= effective_onset_seconds) {
            state->sick = true;
        } else if (state->neglect_seconds == 0u) {
            state->sick = false;
        }

        if (config->sickness_death_seconds > 0u) {
            if (state->neglect_seconds > effective_death_seconds) {
                /* Capped, so an abandoned creature's counter cannot drift
                 * off toward saturation and take a correspondingly absurd
                 * amount of care to walk back if it is somehow revived by a
                 * future feature. */
                state->neglect_seconds = effective_death_seconds;
            }
            if (state->neglect_seconds >= effective_death_seconds) {
                state->dead = true;
            }
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
    put_u8(out, off, state->dead ? 1u : 0u);
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
    put_u8(out, off, state->last_reaction);
    put_u8(out, off, state->last_care_action);
    put_u8(out, off, state->asleep ? 1u : 0u);
    put_u8(out, off, state->tucked_in ? 1u : 0u);
    put_u32(out, off, state->hunger_floor_mp);
    put_u32(out, off, state->happiness_floor_mp);
    put_u32(out, off, state->energy_floor_mp);
    put_u32(out, off, state->dirtiness_cap_mp);
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
    state->dead = get_u8(in, off) != 0u;
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
    const uint8_t last_reaction_byte = get_u8(in, off);
    if (last_reaction_byte > static_cast<uint8_t>(KF_PET_REACTION_DISLIKED)) {
        KF_LOGE(TAG,
                "save has an invalid last_reaction byte (%u) -- refusing to "
                "load",
                last_reaction_byte);
        return false;
    }
    state->last_reaction = last_reaction_byte;
    const uint8_t last_care_action_byte = get_u8(in, off);
    if (last_care_action_byte >= KF_PET_CARE_ACTION_COUNT) {
        KF_LOGE(TAG,
                "save has an invalid last_care_action byte (%u, only %u "
                "actions exist) -- refusing to load",
                last_care_action_byte, KF_PET_CARE_ACTION_COUNT);
        return false;
    }
    state->last_care_action = last_care_action_byte;
    state->asleep = get_u8(in, off) != 0u;
    state->tucked_in = get_u8(in, off) != 0u;
    state->hunger_floor_mp = get_u32(in, off);
    state->happiness_floor_mp = get_u32(in, off);
    state->energy_floor_mp = get_u32(in, off);
    state->dirtiness_cap_mp = get_u32(in, off);
    return true;
}

} // namespace

uint8_t kf_pet_adults_in_family(uint8_t teen_form) {
    if (teen_form >= KF_PET_TEEN_FORM_COUNT) {
        return 1u;
    }
    return kAdultsInFamily[teen_form];
}

uint8_t kf_pet_reaction_to(uint8_t base_trait, kf_pet_care_action action,
                            uint8_t variation) {
    const unsigned a = static_cast<unsigned>(action);
    if (base_trait >= KF_PET_BASE_TRAIT_COUNT ||
        a >= KF_PET_CARE_ACTION_COUNT ||
        variation >= KF_PET_CARE_VARIATION_COUNT) {
        return KF_PET_REACTION_NEUTRAL;
    }
    if (variation == kLikedVariation[base_trait][a]) {
        return KF_PET_REACTION_LIKED;
    }
    if (variation == kDislikedVariation[base_trait][a]) {
        return KF_PET_REACTION_DISLIKED;
    }
    return KF_PET_REACTION_NEUTRAL;
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

    /* Twice the decay, and about a fifth of the happiness bar an hour on
     * top. Tuning numbers. */
    c.sick_decay_multiplier_percent = 200u;
    c.sick_happiness_drain_mp_per_hour = 20000u;

    /* A full day of accumulated neglect, on top of the three hours that
     * made it ill -- twenty-one hours of visible, escalating distress
     * before the end. Tuning number, and the one here most worth being
     * generous with: every death should feel deserved. */
    c.sickness_death_seconds = 86400u;

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

    /* Care variations (docs/superpowers/plans/2026-08-09-care-variations.md):
     * what a care action restores, by how the creature took it. Illustrative,
     * same status as every other number in this function -- liked more than
     * triples disliked, which is the discovery signal: the gap has to be big
     * enough to notice on a bar, not so big that guessing wrong feels like a
     * punishment. */
    c.care_boost_liked_mp = 35000u;
    c.care_boost_neutral_mp = 25000u;
    c.care_boost_disliked_mp = 10000u;

    /* A bath's happiness bonus: noticeable for the way it likes, barely
     * there for a way it tolerates, nothing for the way it hates. Small
     * against the 25000 a real play session gives -- this is a reward for
     * paying attention, not a second way to entertain the creature.
     * Tuning numbers. */
    c.bath_happiness_liked_mp = 6000u;
    c.bath_happiness_neutral_mp = 1500u;

    /* Below a 20% average across the whole of CHILD, the creature was kept
     * alive and nothing more, and grows into the dust form rather than one
     * of the four families.
     *
     * PROVISIONAL, and the one number here that most needs playing rather
     * than reasoning about. It is squeezed between two hard constraints:
     * too high and it swallows the worst verb-family band, so a badly
     * raised creature never gets a real form; too low and it is
     * unreachable by anything still alive, because staying alive already
     * requires keeping every need above neglect_need_mp most of the time,
     * which drags the average up on its own. 20% sits between them by
     * argument, not by evidence -- it is the only route to a character the
     * bible describes, so it wants testing with a real creature. */
    c.dust_care_average_mp = 20000u;

    /* Waking a sleeping creature deliberately costs 5% happiness -- "kept
     * small" (the spec's own words), a fraction of what a single disliked
     * care action already costs relative to a liked one
     * (care_boost_disliked_mp vs care_boost_liked_mp above), illustrative
     * like every other figure in this function, not a tuned value. */
    c.wake_happiness_cost_mp = 5000u;

    /* Tucking in while drowsy is worth 10% on EACH of hunger/happiness/
     * energy at next wake -- ADR 0052, 2026-08-11 bedtime-behaviour
     * extension. Sized against wake_happiness_cost_mp just above rather
     * than picked in isolation: this is double that cost, on three needs
     * instead of one, so the bonus reads as clearly worth the one extra
     * button press without coming close to a free top-up (a full night's
     * ordinary decay, e.g. an adult's ~4000-8000 mp/hour rates over nine
     * hours, still dwarfs it). FEEL, NOT ENGINEERING, exactly like every
     * other figure in this function -- flagged for Chris to tune on the
     * board. */
    c.tuck_in_wake_bonus_mp = 10000u;
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
    state->dead = false;
    state->asleep = false;
    state->tucked_in = false;
    state->hunger_floor_mp = 0u;
    state->happiness_floor_mp = 0u;
    state->energy_floor_mp = 0u;
    state->dirtiness_cap_mp = 0u;
    state->last_reaction = KF_PET_REACTION_NEUTRAL;
    state->last_care_action = KF_PET_CARE_FEED;
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
    if (state->dead) {
        return;
    }

    /* Requirement 1 of docs/superpowers/plans/2026-08-13-screens-clock-
     * sleep.md's Task 6, landed and checked before any sleep logic exists
     * (see run_pet_check()'s new "last_advanced tracks live play" case):
     * `cursor` mirrors state->last_advanced.epoch_seconds and is what lets
     * every apply_stage_segment() call below know the wall-clock instant
     * ITS OWN segment starts at, without this function ever calling into
     * the HAL -- see kf_pet_state::last_advanced's own comment for the
     * full reasoning. `have_clock` is snapshotted once, here, rather than
     * re-read as `state->last_advanced.valid` after each segment: nothing
     * below ever sets last_advanced.valid true from false or the reverse,
     * only advances epoch_seconds when it was already valid, so the value
     * cannot change mid-call and re-reading it would just be the same
     * flag, spelled out repeatedly. */
    const bool have_clock = state->last_advanced.valid;
    int64_t cursor = state->last_advanced.epoch_seconds;

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
            apply_stage_segment(state, config, segment, have_clock, cursor);
            state->stage_elapsed_seconds += segment;
            remaining -= segment;
            if (have_clock) {
                cursor += segment;
                state->last_advanced.epoch_seconds = cursor;
            }

            if (state->dead) {
                /* Died inside that segment. Stop here rather than let the
                 * loop carry on spending the remaining time and walk the
                 * creature through a stage transition it did not live to
                 * see -- the branch it would be handed comes from a care
                 * average over time that never happened. The segment it
                 * died in is already fully credited above, including the
                 * last_advanced bump just above: the wall clock genuinely
                 * did reach that point before the creature died, whatever
                 * time was left in `elapsed_seconds` is simply never
                 * applied to anything again. */
                return;
            }
        }

        if (state->stage_elapsed_seconds >= duration) {
            advance_to_next_stage(state, config);
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
        apply_stage_segment(state, config, remaining, have_clock, cursor);
        state->stage_elapsed_seconds += remaining;
        if (have_clock) {
            cursor += remaining;
            state->last_advanced.epoch_seconds = cursor;
        }
    }
}

void kf_pet_feed(kf_pet_state *state, const kf_pet_config *config,
                  uint8_t variation) {
    if (state->dead) {
        return;
    }
    if (state->care_actions_taken < UINT32_MAX) {
        state->care_actions_taken++;
    }
    const kf_pet_millipercent boost =
        apply_care_reaction(state, config, KF_PET_CARE_FEED, variation);
    state->hunger_mp = clamp_add(state->hunger_mp, boost);

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

void kf_pet_play(kf_pet_state *state, const kf_pet_config *config,
                  uint8_t variation) {
    if (state->dead) {
        return;
    }
    if (state->care_actions_taken < UINT32_MAX) {
        state->care_actions_taken++;
    }
    const kf_pet_millipercent boost =
        apply_care_reaction(state, config, KF_PET_CARE_PLAY, variation);
    state->happiness_mp = clamp_add(state->happiness_mp, boost);
}

void kf_pet_rest(kf_pet_state *state, const kf_pet_config *config,
                  uint8_t variation) {
    if (state->dead) {
        return;
    }
    if (state->care_actions_taken < UINT32_MAX) {
        state->care_actions_taken++;
    }
    const kf_pet_millipercent boost =
        apply_care_reaction(state, config, KF_PET_CARE_REST, variation);
    state->energy_mp = clamp_add(state->energy_mp, boost);
}

void kf_pet_bath(kf_pet_state *state, const kf_pet_config *config,
                  uint8_t variation) {
    if (state->dead) {
        return;
    }
    if (state->care_actions_taken < UINT32_MAX) {
        state->care_actions_taken++;
    }

    /* Clean is clean, whichever way it was done. Being washed is a NEED,
     * and a need met badly is still a need met -- a creature left dirty
     * because it disliked the flannel would punish the player for doing
     * the right thing in the wrong style, which is the opposite of what
     * preference is for.
     *
     * Poops are untouched here: they are kf_pet_flush()'s job. */
    state->dirtiness_mp = 0u;

    /* What preference buys is a little happiness on top. apply_care_
     * reaction() records the reaction (which is what the screen shows) and
     * hands back the need-restore figure the other three actions use --
     * that figure is not what a bath is worth, so it is deliberately
     * discarded and the two bath-specific values used instead. */
    (void)apply_care_reaction(state, config, KF_PET_CARE_BATH, variation);
    switch (state->last_reaction) {
    case KF_PET_REACTION_LIKED:
        state->happiness_mp =
            clamp_add(state->happiness_mp, config->bath_happiness_liked_mp);
        break;
    case KF_PET_REACTION_NEUTRAL:
        state->happiness_mp =
            clamp_add(state->happiness_mp, config->bath_happiness_neutral_mp);
        break;
    default:
        /* Disliked: nothing. Not a penalty -- see kf/pet.h's comment on
         * why there is no third config value to make one possible. */
        break;
    }
}

void kf_pet_flush(kf_pet_state *state) {
    if (state->dead) {
        return;
    }
    if (state->care_actions_taken < UINT32_MAX) {
        state->care_actions_taken++;
    }

    /* All of them, always. There are no degrees of flushing: either the
     * mess is gone or the player did not press the button.
     *
     * Nothing else moves. No need is restored, no reaction is recorded,
     * and last_reaction/last_care_action are left exactly as the previous
     * real care action set them -- a chore should not overwrite the
     * creature's response to the last thing that was actually done TO it,
     * which is what the screen is showing. */
    state->poop_count = 0u;
}

void kf_pet_wake(kf_pet_state *state, const kf_pet_config *config) {
    if (state->dead || !state->asleep) {
        return;
    }
    /* Deliberately does NOT touch care_actions_taken -- unlike feed/play/
     * rest/bath, waking a creature is not something it has an OPINION
     * about (kf_pet_reaction_to() is never consulted), so there is no
     * reaction to record and no reason to start the presenter's reaction-
     * hold window (kf_creature_presenter.cpp's own care_actions_taken
     * watch). Pose precedence already puts asleep above the held reaction
     * (kf_creature_pose_for(), ADR 0048) for the identical reason: nothing
     * about waking should make the creature flash a happy/objecting pose
     * it never actually performed.
     *
     * Same underflow-safe subtraction apply_decay() above already uses:
     * floor at zero rather than wrap. */
    const kf_pet_millipercent cost = config->wake_happiness_cost_mp;
    state->happiness_mp =
        (cost >= state->happiness_mp) ? 0u : state->happiness_mp - cost;
    state->asleep = false;

    /* ADR 0052: deliberately does NOT pay a due tuck-in bonus here, even
     * though this also crosses asleep=true -> asleep=false. Paying it on
     * every deliberate early wake would let a tucked-in creature be woken
     * again within the same minute for a net-positive trade (a small
     * happiness cost against a bonus on three needs) -- the bonus is
     * specifically the reward for waiting out the WHOLE night, per Chris's
     * "wakes up" framing, so it stays gated to apply_stage_segment()'s own
     * natural transition. The flag itself is untouched here: a creature
     * woken early keeps tucked_in set and is simply paid whenever the clock
     * genuinely does carry it past the night window next -- see apply_
     * stage_segment()'s own comment on this split. */
}

/* ADR 0052: see kf/pet.h's own header comment on this function for what it
 * returns and why. `kNightStartHour`/`kDrowsyWindowSeconds` above are the
 * only two constants consulted -- no second, independently-hardcoded
 * bedtime hour anywhere in this function. */
bool kf_pet_drowsy(const kf_pet_state *state) {
    if (state->dead || state->asleep) {
        return false;
    }
    if (state->stage == KF_PET_STAGE_EGG) {
        /* Eggs never sleep (ADR 0048) -- and cannot even naturally reach
         * this point via a real asleep=true->false edge, since apply_stage_
         * segment() returns before ever touching `asleep` for one -- so an
         * egg has no bedtime to be drowsy before either. Checked explicitly
         * here (rather than relying on that unreachability) so kf_pet_tuck_
         * in() cannot be tricked into setting `tucked_in` on an egg by a
         * caller that hand-builds a state with last_advanced sitting inside
         * the window: the bonus this flag pays out is only ever meant to
         * be claimed at a real asleep -> awake transition, which an egg
         * will never generate. */
        return false;
    }
    if (!state->last_advanced.valid) {
        return false; /* no clock, no notion of "right now" to test */
    }

    kf_civil now;
    kf_civil_from_epoch(state->last_advanced.epoch_seconds, &now);
    const uint32_t seconds_since_midnight =
        static_cast<uint32_t>(now.hour) * 3600u +
        static_cast<uint32_t>(now.minute) * 60u +
        static_cast<uint32_t>(now.second);
    const uint32_t night_start_seconds =
        static_cast<uint32_t>(kNightStartHour) * 3600u;

    /* [window_start, night_start_seconds), where window_start is
     * kDrowsyWindowSeconds before night_start_seconds -- wrapping across
     * midnight with plain modular arithmetic rather than assuming
     * kDrowsyWindowSeconds < night_start_seconds, the same "handle the wrap
     * generally, do not special-case the values that happen not to need it"
     * discipline kf_clock_seconds_in_daily_window() itself follows for the
     * (much larger) night window. With kNightStartHour == 22 and a 10-minute
     * window this never actually wraps -- but the arithmetic below is
     * correct either way, so a future change to either constant cannot
     * silently break it. */
    constexpr uint32_t kSecondsPerDay = 24u * 3600u;
    const uint32_t window_start =
        (night_start_seconds + kSecondsPerDay - kDrowsyWindowSeconds) %
        kSecondsPerDay;
    if (window_start < night_start_seconds) {
        return seconds_since_midnight >= window_start &&
               seconds_since_midnight < night_start_seconds;
    }
    /* Wraps midnight (only reachable if kDrowsyWindowSeconds >
     * night_start_seconds, e.g. a bedtime inside the first ten minutes of
     * the day). */
    return seconds_since_midnight >= window_start ||
           seconds_since_midnight < night_start_seconds;
}

void kf_pet_tuck_in(kf_pet_state *state) {
    if (!kf_pet_drowsy(state)) {
        return;
    }
    state->tucked_in = true;
}

/* Task 8 (docs/superpowers/plans/2026-08-13-screens-clock-sleep.md): see
 * kf/pet.h's own long comment on kf_pet_wants() for why `previous` exists
 * at all and what each threshold means -- not repeated here. */
kf_pet_want kf_pet_wants(const kf_pet_state *state, kf_pet_want previous) {
    if (state->dead || state->asleep) {
        return KF_PET_WANT_NONE;
    }

    const bool food = (state->hunger_mp <= KF_PET_WANT_FOOD_ON_MP) ||
                       (previous == KF_PET_WANT_FOOD &&
                        state->hunger_mp < KF_PET_WANT_FOOD_OFF_MP);
    const bool rest = (state->energy_mp <= KF_PET_WANT_REST_ON_MP) ||
                       (previous == KF_PET_WANT_REST &&
                        state->energy_mp < KF_PET_WANT_REST_OFF_MP);
    const bool bath = (state->dirtiness_mp >= KF_PET_WANT_BATH_ON_MP) ||
                       (previous == KF_PET_WANT_BATH &&
                        state->dirtiness_mp > KF_PET_WANT_BATH_OFF_MP);
    const bool flush =
        (state->poop_count >= KF_PET_WANT_FLUSH_ON_POOPS) ||
        (previous == KF_PET_WANT_FLUSH &&
         state->poop_count > KF_PET_WANT_FLUSH_OFF_POOPS);
    const bool play = (state->happiness_mp <= KF_PET_WANT_PLAY_ON_MP) ||
                       (previous == KF_PET_WANT_PLAY &&
                        state->happiness_mp < KF_PET_WANT_PLAY_OFF_MP);

    /* Priority order -- see kf_pet_wants()'s own header comment in kf/
     * pet.h for the reasoning. */
    if (food) return KF_PET_WANT_FOOD;
    if (rest) return KF_PET_WANT_REST;
    if (bath) return KF_PET_WANT_BATH;
    if (flush) return KF_PET_WANT_FLUSH;
    if (play) return KF_PET_WANT_PLAY;
    return KF_PET_WANT_NONE;
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
    /* Deliberately clock-less (have_clock=false): this seam predates sleep
     * and every existing caller relies on it comparing stages with no wall
     * clock in the picture at all (see kf/pet.h's own header comment on
     * this function). A test that wants sleep's night-window behaviour
     * calls kf_pet_advance() directly instead -- the real entry point, and
     * the only one that threads a wall-clock cursor through. */
    apply_stage_segment(state, config, segment_seconds, false, 0);
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
