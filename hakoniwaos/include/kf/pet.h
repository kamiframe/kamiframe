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
 * Personality traits, care-mistake tracking, and the random event
 * scheduler are still not built, on purpose -- see ADR 0015's "what
 * deliberately is not built," now narrowed: life stages and evolution
 * moved from that list to this file with ADR 0021, the other three
 * remain deferred, real design surfaces of their own.
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

/* Tree shape, per Chris's design: 3 teen types, each branching to 2 adult
 * forms (6 adults total). Both are compile-time constants, not config --
 * unlike stage durations and decay rates, the SHAPE of the evolution tree
 * is a structural decision (how many branch slots exist), not a tuning
 * value; changing these means the save format and the Lua-side branch
 * tables change together, not something to vary per pet at runtime. */
#define KF_PET_TEEN_FORM_COUNT 3u
#define KF_PET_ADULT_BRANCH_COUNT 2u

/* Decay rates (millipercent per hour) and stage durations (seconds).
 * Config, not a constant, because "a dev writes a pet by configuring and
 * skinning this" is the whole point -- see the header comment above.
 * kf_pet_default_config() below returns illustrative values for the demo
 * and the determinism check, not a recommendation for any real pet's
 * tuning -- Chris's own words on stage timing: "I'll decide exact numbers
 * later, just make it configurable." Adult has no duration field: it is
 * terminal in this slice, nothing to time. */
typedef struct {
    uint32_t hunger_decay_mp_per_hour;
    uint32_t happiness_decay_mp_per_hour;
    uint32_t energy_decay_mp_per_hour;

    uint32_t egg_duration_seconds;
    uint32_t baby_duration_seconds;
    uint32_t child_duration_seconds;
    uint32_t teen_duration_seconds;
} kf_pet_config;

/* A reasonable illustrative default: hunger drains fastest (empty from
 * full in a bit over 4 days), energy slowest (a bit over 8), happiness in
 * between. Stage durations: egg 1 hour, baby 1 day, child 2 days, teen 3
 * days -- adult by about a week, sized for showing off a build quickly,
 * not tuned against anything real yet, see the ADR. */
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
    uint8_t adult_branch; /* [0, KF_PET_ADULT_BRANCH_COUNT) once stage == ADULT */

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
} kf_pet_state;

/* A fresh pet: every need full, stage KF_PET_STAGE_EGG, every branch
 * index 0, every accumulator 0, last_advanced invalid. */
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

/* Care actions. Each raises its need by a fixed amount and clamps at
 * KF_PET_MILLIPERCENT_MAX -- feeding an already-full pet does nothing
 * extra, it does not "bank" overfeeding against future decay. */
void kf_pet_feed(kf_pet_state *state);
void kf_pet_play(kf_pet_state *state);
void kf_pet_rest(kf_pet_state *state);

/* Fixed-size, versioned on-disk format. See kf_pet.cpp for exactly why
 * this is hand-packed byte by byte rather than a raw struct written
 * through kf_store_write(&state, sizeof(state)) -- struct layout is not a
 * promise two different compilers (this project builds with both GCC and
 * MSVC) are obliged to keep identically. Bumped to version 2 with ADR
 * 0021: a version-1 save (from before life stages existed) is refused by
 * kf_pet_load_and_advance()'s unpack() step and falls back to a fresh
 * pet, exactly the behaviour ADR 0015 already established for any
 * unrecognised version -- no migration code, an explicit, accepted cost. */
#define KF_PET_SAVE_KEY "pet"
#define KF_PET_SAVE_BYTES 41u /* see kf_pet.cpp's pack()/unpack() for the exact layout */

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
