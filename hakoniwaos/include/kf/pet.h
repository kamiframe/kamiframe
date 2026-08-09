/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * The pet simulation framework: needs, decay, life stages, evolution, and
 * offline fast-forward. See docs/architecture/adr-0015-pet-simulation-
 * framework.md and docs/architecture/adr-0021-life-stages-and-evolution.md.
 *
 * "This is your secret weapon and the thing no generic console has" --
 * 02-min-spec-sheet.md, item 4. A dev writes a pet by configuring and
 * skinning this before ever writing custom logic, which is why decay
 * rates AND stage durations are a kf_pet_config a caller supplies, not a
 * constant baked in here.
 *
 * Pure Core logic: no HAL calls inside kf_pet_advance(), kf_pet_feed(),
 * kf_pet_play() or kf_pet_rest(), by design -- see
 * 08-phase1-slice1-decisions.md's HAL boundary table: "Pet simulation,
 * needs, decay, evolution | Core | Pure logic. Should be unit-testable
 * with no HAL at all." Only kf_pet_load_and_advance() and kf_pet_save()
 * touch the HAL (storage and time), and they are thin wrappers around the
 * pure functions above, not where the actual maths lives.
 *
 * WHAT A STAGE OR FORM NUMBER MEANS IS NOT THIS FILE'S BUSINESS. `stage`
 * is a life-cycle position (egg/baby/child/teen/adult); `teen_form` and
 * `adult_branch` are plain 0-based indices identifying WHICH branch of the
 * evolution tree was taken, nothing more. This file does not know or care
 * what a "teen_form == 1" pet looks like, is called, or acts like -- that
 * is real creative content, and it lives in the Lua cartridge layer (see
 * kf_lua_port.cpp's pet.* binding) or above, not in Core. Building the
 * generic branching mechanism here and keeping every actual character
 * decision out of it is what lets real character work start whenever it's
 * ready without touching this file again.
 *
 * Care-mistake tracking and the random event scheduler are still not
 * built, on purpose -- see ADR 0015's "what deliberately is not built,"
 * now narrowed twice: life stages and evolution moved from that list to
 * this file with ADR 0021, and personality traits moved off it with
 * ADR 0023 (`base_trait` and the three `*_integral_mp_seconds`
 * accumulators below). The other two remain deferred, real design
 * surfaces of their own.
 *
 * PERSONALITY, LIKE EVOLUTION ABOVE, IS NUMBERS ONLY HERE. `base_trait`
 * and `kf_pet_dominant_care_trait()`'s return value are opaque 0-based
 * indices, nothing more -- see ADR 0023 and kf_lua_port.cpp's
 * pet.base_trait()/pet.dominant_care_trait() binding for where the actual
 * names and flavour text live.
 *
 * Valid C, same convention as kf/arena.h and kf/app.h: nothing here
 * belongs to the HAL boundary that every header under kf/hal/ is
 * mechanically checked against, but there is no reason for Core's own
 * headers to be any less portable than they need to be.
 */

#ifndef KF_PET_H
#define KF_PET_H

#include "kf/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Millipercent: 0..100000 represents 0.000%..100.000%. Integer, not
 * float: exact and deterministic, and offline fast-forward multiplies a
 * decay rate by an elapsed time that can be a frame or three real days in
 * one closed-form step (kf_pet_advance()) -- floating-point drift
 * accumulating across months of real device uptime is exactly the kind
 * of bug that would not show up in any test run short enough to notice.
 */
typedef uint32_t kf_pet_millipercent;
#define KF_PET_MILLIPERCENT_MAX ((kf_pet_millipercent)100000u)

/* The life-cycle position, per Chris's design: egg (no care, pure timer)
 * -> baby -> child (care during THIS stage decides the teen branch) ->
 * teen (care during THIS stage decides the adult branch) -> adult
 * (terminal in this slice -- no further evolution yet). Numeric order
 * matches life-cycle order deliberately, so "stage >= KF_PET_STAGE_TEEN"
 * reads naturally wherever that comparison is useful. */
typedef enum {
    KF_PET_STAGE_EGG = 0,
    KF_PET_STAGE_BABY = 1,
    KF_PET_STAGE_CHILD = 2,
    KF_PET_STAGE_TEEN = 3,
    KF_PET_STAGE_ADULT = 4,
} kf_pet_stage;

/* Number of life stages, for sizing per-stage tables. Kept next to the enum
 * so the two cannot drift: adding a stage without widening the tables that
 * index by it would read past the end of every one of them. */
#define KF_PET_STAGE_COUNT 5u

/* Tree shape, per the character bible (14-character-bible-v1.md, sections
 * 6/7) as resolved in
 * docs/superpowers/specs/2026-08-09-core-care-loop-design.md: 4 verb
 * families, each branching to an UNEVEN number of adult forms. Compile-time
 * constants, not config -- unlike stage durations and decay rates, the
 * SHAPE of the evolution tree is a structural decision (how many branch
 * slots exist), not a tuning value; changing these means the save format
 * and the Lua-side branch tables change together, not something to vary
 * per pet at runtime.
 *
 * Four verb families -- Cut, Hold, Mark, Go -- per the character bible's
 * section 6. Indices only: the bible's names are unverified placeholders and
 * deliberately do not appear in Core, exactly like base_trait above. */
#define KF_PET_TEEN_FORM_COUNT 4u

/* The largest number of adults any one family has. Sizes arrays that must
 * hold a row per family; the ACTUAL count per family is uneven and comes
 * from kf_pet_adults_in_family(). */
#define KF_PET_ADULT_BRANCH_MAX 3u

/* How many adult forms a given verb family leads to.
 *
 * Uneven on purpose, and expected to change: the bible's confirmed roster is
 * Cut 2, Hold 3, Mark 3, Go 1, and its own section 11 says Go and Cut still
 * need creatures to balance at three each. A per-family lookup rather than
 * one constant means filling those gaps is a one-line data edit instead of a
 * change to the tree's shape.
 *
 * Out-of-range input returns 1 rather than reading past the table -- a
 * corrupted save that survived the version check should land on a valid
 * creature, not undefined behaviour. */
uint8_t kf_pet_adults_in_family(uint8_t teen_form);

/* Base-trait table size (ADR 0023), same compile-time-constant treatment
 * as the two above and for the same reason: this is the SHAPE of the
 * table (how many slots exist), not a tuning value. Six placeholder base
 * traits as of this slice (Chris: "those look good as starting traits" --
 * see 16-personality-traits-concrete-plan.md); the actual six names live
 * in the Lua cartridge layer, not here. */
#define KF_PET_BASE_TRAIT_COUNT 6u

/* Decay rates (millipercent per hour) and stage durations (seconds).
 * Config, not a constant, because "a dev writes a pet by configuring and
 * skinning this" is the whole point -- see the header comment above.
 * kf_pet_default_config() below returns illustrative values for the demo
 * and the determinism check, not a recommendation for any real pet's
 * tuning -- Chris's own words on stage timing: "I'll decide exact numbers
 * later, just make it configurable." Adult has no duration field: it is
 * terminal in this slice, nothing to time. */
/* Decay rates for one life stage, in millipercent per hour.
 *
 * Per-stage rather than one set for the whole life because demand IS the
 * game: a baby that needs attention every half hour and an adult you check
 * on a few times a day are the same creature at different points, and a
 * single rate cannot express both. See
 * docs/superpowers/specs/2026-08-09-core-care-loop-design.md section 2. */
typedef struct {
    uint32_t hunger_mp_per_hour;
    uint32_t happiness_mp_per_hour;
    uint32_t energy_mp_per_hour;
} kf_pet_stage_rates;

typedef struct {
    /* Indexed by kf_pet_stage. The EGG row is all zeroes and is never read
     * -- apply_stage_segment() returns early for eggs -- but it is present
     * so the table can be indexed by stage without an offset, which is one
     * fewer thing to get wrong. */
    kf_pet_stage_rates stage_rates[KF_PET_STAGE_COUNT];

    uint32_t egg_duration_seconds;
    uint32_t baby_duration_seconds;
    uint32_t child_duration_seconds;
    uint32_t teen_duration_seconds;

    /* ADR 0023: the half-life, in seconds, of the periodic-halving decay
     * applied to the three personality accumulators below (kf_pet_state's
     * `*_integral_mp_seconds`) -- every time this many seconds' worth of
     * segments have been credited, whatever is accumulated so far is
     * halved before the newest segment is added at full weight. This is
     * what makes personality "a genuine swinger" (Chris's own care-derived
     * requirement) rather than a flat whole-life average: a long stretch
     * of one care style dominates the reading for roughly this many
     * seconds after it ends, then fades as fresher care outweighs it,
     * without ever fully resetting the way `care_integral_mp_seconds`
     * does at a stage transition. Zero is treated as "no periodic
     * halving" (pure whole-life accumulation) rather than a divide-by-zero
     * -- see pet.cpp's accumulate_personality(). */
    uint32_t personality_recency_half_life_seconds;
} kf_pet_config;

/* A reasonable illustrative default: hunger drains fastest (empty from
 * full in a bit over 4 days), energy slowest (a bit over 8), happiness in
 * between. Stage durations: egg 1 hour, baby 1 day, child 2 days, teen 3
 * days -- adult by about a week, sized for showing off a build quickly,
 * not tuned against anything real yet, see the ADR.
 * personality_recency_half_life_seconds defaults to 86400 (24h) -- also
 * illustrative, per Chris's ADR 0023 answer ("recent care should be
 * weighted high enough that it can swing things later if needed") rather
 * than any tuned value. */
kf_pet_config kf_pet_default_config(void);

typedef struct {
    kf_pet_millipercent hunger_mp;
    kf_pet_millipercent happiness_mp;
    kf_pet_millipercent energy_mp;

    /* The wall-clock time this state was last advanced to. Saved
     * alongside the needs (see kf_pet_save()) so a reload can compute
     * exactly how long the device was off and fast-forward by that much
     * -- see kf_pet_load_and_advance(). Invalid (kf_wall_time.valid ==
     * false) until the first successful advance. */
    kf_wall_time last_advanced;

    kf_pet_stage stage;

    /* Which branch was taken. Meaningless (always 0) before the branch
     * point that sets it: teen_form is set at the child->teen transition
     * and read from then on; adult_branch is set at the teen->adult
     * transition. See the header comment above for why these are opaque
     * indices, not names. */
    uint8_t teen_form;   /* [0, KF_PET_TEEN_FORM_COUNT) once stage >= TEEN */
    uint8_t adult_branch; /* [0, kf_pet_adults_in_family(teen_form)) once
                            * stage == ADULT -- the per-family count, not one
                            * shared constant; see kf_pet_adults_in_family()
                            * above. */

    /* How many seconds have been credited to the CURRENT stage so far.
     * Resets to 0 at every stage transition. This is what
     * kf_pet_advance() measures "is this stage over yet" against --
     * deliberately a plain counter fed by elapsed_seconds, not a
     * kf_wall_time baseline, so this file never needs to read a clock to
     * know where a stage boundary falls. */
    uint64_t stage_elapsed_seconds;

    /* Running numerator of the average "how well was it cared for during
     * this stage" score: a sum of (hunger+happiness+energy)/3 samples,
     * each weighted by how many seconds it applied for. Divide by
     * stage_elapsed_seconds to get the average millipercent score
     * kf_pet_advance() uses to pick a branch at the NEXT transition.
     * Resets to 0 at every stage transition, same as the counter above.
     * Only meaningfully accumulated during the child and teen stages --
     * the two stages whose average actually feeds a branch decision, per
     * Chris's design; see kf_pet.cpp. uint64_t: millipercent (up to
     * 100000) times seconds (offline fast-forward can be weeks, ~10^7)
     * is up to roughly 10^12, comfortably inside uint64_t without
     * overflow, the identical reasoning kf_pet_advance()'s own decay math
     * already relies on. */
    uint64_t care_integral_mp_seconds;

    /* ADR 0023: personality state. `base_trait` is rolled once, via
     * kf_rng_below() (kf/rng.h -- the same game-visible, save/replay-
     * deterministic RNG the rest of Core already uses, seeded once from
     * the entropy HAL at boot), inside kf_pet_init() below, and never
     * touched again for the rest of the pet's life -- mostly flavour, per
     * Chris's design (doc 15/16). [0, KF_PET_BASE_TRAIT_COUNT).
     *
     * The three `*_integral_mp_seconds` accumulators are the "major,"
     * care-derived trait: each works exactly like care_integral_mp_seconds
     * above (a need's own millipercent value, weighted by how many seconds
     * it applied for, summed) except three separate running totals instead
     * of one blended average, and periodic-halving (see kf_pet_config's
     * personality_recency_half_life_seconds) instead of a hard reset --
     * whole-life, not per-stage: unlike care_integral_mp_seconds, these are
     * NEVER reset at a stage transition, because personality is meant to
     * read as an accumulated identity, not a per-stage vote (see doc 16).
     * Accumulated whenever stage != EGG (an egg has no needs to weight by
     * at all -- see apply_stage_segment() in pet.cpp), starting from Baby,
     * unlike care_integral_mp_seconds which only accumulates during
     * Child/Teen specifically. Which of the three is "dominant" right now
     * is a pure function of these three numbers, computed on demand by
     * kf_pet_dominant_care_trait() below, never stored. */
    uint64_t hunger_integral_mp_seconds;
    uint64_t happiness_integral_mp_seconds;
    uint64_t energy_integral_mp_seconds;

    /* Remainder seconds not yet consumed by a periodic halving -- see
     * accumulate_personality() in pet.cpp. Always in
     * [0, personality_recency_half_life_seconds) after any call; exists
     * so the halving math is exact across many small kf_pet_advance()
     * calls (e.g. ordinary live play, one small segment per frame-batch)
     * as well as one huge offline-fast-forward jump, without needing a
     * kf_wall_time baseline of its own. */
    uint32_t care_recency_window_seconds;

    uint8_t base_trait; /* [0, KF_PET_BASE_TRAIT_COUNT), set once at init */
} kf_pet_state;

/* A fresh pet: every need full, stage KF_PET_STAGE_EGG, every branch
 * index 0, every accumulator 0 (including the three new personality
 * accumulators and care_recency_window_seconds), last_advanced invalid --
 * EXCEPT base_trait, which is rolled fresh via kf_rng_below() (ADR 0023),
 * not zeroed. Still pure Core logic, no HAL calls: kf_rng.h has none
 * either (see its own header comment), so this keeps kf_pet_init()'s
 * existing no-HAL, trivially-unit-testable contract intact -- the caller
 * is responsible for having seeded kf_rng (kf_rng_seed(), normally once
 * at boot from the entropy HAL, see kf/app.cpp) before the first pet this
 * process creates, the same ordering requirement every other kf_rng_*
 * consumer already has. */
void kf_pet_init(kf_pet_state *state);

/* Applies decay AND stage progression for exactly `elapsed_seconds`, in a
 * closed-form calculation bounded by the number of REMAINING LIFE STAGES
 * (at most 4), never by elapsed_seconds itself -- an offline gap of three
 * days or three years advancing through every remaining stage costs the
 * same handful of internal steps, which is what keeps "the device was off
 * for a week and hatched, grew up, and became a teen while it was off" a
 * closed-form calculation rather than the per-second simulation ADR 0015
 * already rejected for plain decay. If `elapsed_seconds` spans more than
 * one stage boundary, each stage's own branch (if it has one) is decided
 * from the care actually accumulated during exactly that stage's real
 * duration, not blurred across stages.
 *
 * Clamps every need to [0, KF_PET_MILLIPERCENT_MAX]. Deliberately does NOT
 * read a clock or touch `last_advanced`: the caller (kf_pet_load_and_
 * advance() below, or a future frame-loop caller passing a per-frame
 * delta) decides what "elapsed" means and updates the timestamp itself,
 * which is what keeps this function trivially unit-testable with an
 * arbitrary elapsed value and no HAL in the picture at all. */
void kf_pet_advance(kf_pet_state *state, const kf_pet_config *config,
                     uint32_t elapsed_seconds);

/* Test seam: applies exactly one decay segment at the pet's CURRENT stage,
 * without the stage-transition logic kf_pet_advance() wraps around it.
 * Exists so a test can compare two stages over identical elapsed time
 * without constructing two whole life histories. Not for gameplay use --
 * kf_pet_advance() is the real entry point. */
void apply_stage_segment_for_test(kf_pet_state *state,
                                   const kf_pet_config *config,
                                   uint32_t segment_seconds);

/* Care actions. Each raises its need by a fixed amount and clamps at
 * KF_PET_MILLIPERCENT_MAX -- feeding an already-full pet does nothing
 * extra, it does not "bank" overfeeding against future decay. */
void kf_pet_feed(kf_pet_state *state);
void kf_pet_play(kf_pet_state *state);
void kf_pet_rest(kf_pet_state *state);

/* Which of the three care-derived traits is currently dominant --
 * 0 = hunger, 1 = happiness, 2 = energy -- computed fresh from the three
 * running accumulators above every call, never stored (see kf_pet_state's
 * comment). Ties, including the all-zero case (a pet still in EGG, which
 * never accumulates), resolve to 0 (hunger) deliberately: a defined,
 * boring default rather than an unspecified one. See ADR 0023. */
uint8_t kf_pet_dominant_care_trait(const kf_pet_state *state);

/* Fixed-size, versioned on-disk format. See kf_pet.cpp for exactly why
 * this is hand-packed byte by byte rather than a raw struct written
 * through kf_store_write(&state, sizeof(state)) -- struct layout is not a
 * promise two different compilers (this project builds with both GCC and
 * MSVC) are obliged to keep identically. Bumped to version 2 with ADR
 * 0021 (life stages/evolution) and to version 3 with ADR 0023
 * (personality traits): a save from an earlier version is refused by
 * kf_pet_load_and_advance()'s unpack() step and falls back to a fresh
 * pet, exactly the behaviour ADR 0015 already established for any
 * unrecognised version -- no migration code, an explicit, accepted cost. */
#define KF_PET_SAVE_KEY "pet"
#define KF_PET_SAVE_BYTES 70u /* see kf_pet.cpp's pack()/unpack() for the exact layout */

/* Packs `state` and writes it to kf_store (kf/hal/storage.h) under
 * KF_PET_SAVE_KEY. Call after any change worth surviving a power cycle --
 * typically after every care action and, on the device, before
 * kf_power_deep_sleep_until() (see kf/hal/power.h's own warning: on real
 * hardware, deep sleep may not return to the line after the call, so
 * anything needed afterward must already be saved before it, not held in
 * a local variable across it). */
kf_result kf_pet_save(const kf_pet_state *state);

/* Reads the save under KF_PET_SAVE_KEY, if any, unpacks it into `state`,
 * then advances it for exactly the wall-clock time that passed since
 * `last_advanced` -- the actual offline-fast-forward mechanism, and the
 * thing the hardware-purchase trigger names explicitly. Updates
 * `last_advanced` to the current wall-clock reading before returning.
 *
 * If there is no save yet (KF_ERR_UNAVAILABLE from kf_store_read()),
 * initialises `state` via kf_pet_init() instead: a fresh pet, not an
 * error. The same fallback applies to a save written by an incompatible
 * version (see KF_PET_SAVE_BYTES above).
 *
 * Two adversarial-wall-clock cases handled without erroring or ageing
 * the pet incorrectly, per kf/hal/time.h's own warning that this clock
 * "can be unset... can jump forward... can go backwards":
 *   - current wall clock invalid (RTC not yet set): no ageing happens
 *     this call; `last_advanced` is left invalid so the next call with a
 *     valid clock tries again from a real baseline, never from zero.
 *   - current wall clock reads EARLIER than the saved `last_advanced`
 *     (coin cell died and the RTC reset, or a user set the date
 *     backwards): elapsed is clamped to zero rather than underflowing --
 *     the pet does not get YOUNGER, it just does not age this call. See
 *     kf_power_deep_sleep_until()'s identical "already in the past is a
 *     no-op, not an error" handling for the same reasoning applied the
 *     other direction. */
kf_result kf_pet_load_and_advance(kf_pet_state *state,
                                   const kf_pet_config *config);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KF_PET_H */
