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

/* The dust form. NOT one of the four verb families -- deliberately equal to
 * KF_PET_TEEN_FORM_COUNT so it sits just past them, and so any loop over the
 * families skips it. Reached by keeping a creature alive and doing nothing
 * else with it -- a care average across the whole of CHILD below
 * kf_pet_config::dust_care_average_mp. (It used to be "never interacted with
 * at all", which stopped being reachable once an untouched creature started
 * dying in childhood; see advance_to_next_stage().) */
#define KF_PET_TEEN_FORM_DUST KF_PET_TEEN_FORM_COUNT

/* How many poops can be waiting at once.
 *
 * Bounded on purpose. An unbounded count would overflow any screen that
 * tries to draw them, and past a handful it stops communicating anything a
 * player did not already know -- "you have not cleaned up" is the whole
 * message, and eight says it as clearly as eighty. */
#define KF_PET_MAX_POOPS 8u

/* Where dirtiness becomes visible. Flies first, stink lines later -- two
 * stages of "this is getting bad" that a player can read across a room
 * without looking at a number, which is the entire reason dirtiness is not
 * a fourth bar. */
#define KF_PET_DIRTY_FLIES_MP 50000u   /* 50% */
#define KF_PET_DIRTY_STINK_MP 80000u   /* 80% */

/* Base-trait table size (ADR 0023), same compile-time-constant treatment
 * as the two above and for the same reason: this is the SHAPE of the
 * table (how many slots exist), not a tuning value. Six placeholder base
 * traits as of this slice (Chris: "those look good as starting traits" --
 * see 16-personality-traits-concrete-plan.md); the actual six names live
 * in the Lua cartridge layer, not here. */
#define KF_PET_BASE_TRAIT_COUNT 6u

/* The four care actions a creature has an OPINION about, as an index.
 * Order is arbitrary but fixed: it is the column order of the preference
 * table and the argument to kf_pet_reaction_to(), so reordering it silently
 * rewrites every creature's preferences.
 *
 * Flushing is not here, and that is the point. Clearing up poops is a chore
 * with one right way to do it -- see kf_pet_flush(). A creature having
 * feelings about how its mess was disposed of would be three more variations
 * to draw and tune for no discovery worth having. */
typedef enum {
    KF_PET_CARE_FEED = 0,
    KF_PET_CARE_PLAY = 1,
    KF_PET_CARE_REST = 2,
    KF_PET_CARE_BATH = 3,
} kf_pet_care_action;

#define KF_PET_CARE_ACTION_COUNT 4u

/* How many ways there are to perform each action. Three, per the care-loop
 * spec's section 8: enough to prove the shape, where five is five times the
 * art and tuning before the loop is known to be fun.
 *
 * WHAT each variation IS -- a snack or a proper meal, a bath or a wipe --
 * is creative content and lives in the cartridge layer, exactly like the
 * trait and evolution names. Core only ever knows there are three. */
#define KF_PET_CARE_VARIATION_COUNT 3u

/* How a creature took it. The player reads this, not the bars: with six
 * traits, four actions and three variations there is no discovering
 * anything from a bar that moved slightly less than expected. */
typedef enum {
    KF_PET_REACTION_LIKED = 0,
    KF_PET_REACTION_NEUTRAL = 1,
    KF_PET_REACTION_DISLIKED = 2,
} kf_pet_reaction;

/* How a creature with `base_trait` reacts to `variation` of `action`.
 *
 * A pure function of the table, taking no creature: the screen can preview
 * it, a script can explain it, and a test can walk all seventy-two
 * combinations without constructing anything. Preferences live on the BASE
 * trait, which is rolled once and never changes, so this answer is stable
 * for a creature's whole life and transfers to every future creature that
 * shares the trait -- which is what makes learning it worth the player's
 * time. See the care-loop spec's section 5.
 *
 * Out-of-range input returns KF_PET_REACTION_NEUTRAL rather than reading
 * past the table: a corrupted save that survived the version check should
 * land somewhere boring and defined. */
uint8_t kf_pet_reaction_to(uint8_t base_trait, kf_pet_care_action action,
                            uint8_t variation);

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

    /* How long between poops normally, and how long after a meal. Feeding
     * shortens the wait, which is what ties the mess mechanic to the care
     * loop rather than leaving it on an unrelated timer. */
    uint32_t poop_interval_seconds;
    uint32_t poop_interval_after_feed_seconds;

    /* Baseline dirtying, and the extra per waiting poop. The second is what
     * couples the two halves of mess together: ignoring poops does not just
     * leave poops, it makes the creature filthy faster. */
    uint32_t dirtiness_rise_mp_per_hour;
    uint32_t dirtiness_rise_per_poop_mp_per_hour;

    /* What counts as neglect. A need at or below neglect_need_mp, more than
     * neglect_poop_count poops waiting, or dirtiness at or above
     * neglect_dirtiness_mp -- any one of the three is enough.
     *
     * Three separate channels rather than one blended score, because the
     * player has to be able to work out WHICH thing they are getting wrong,
     * and a blend of "somewhat hungry and somewhat filthy" tells them
     * nothing they can act on. */
    kf_pet_millipercent neglect_need_mp;
    kf_pet_millipercent neglect_dirtiness_mp;
    uint8_t neglect_poop_count;

    /* How much accumulated neglect turns into sickness. The accumulator
     * falls again at the same rate while the creature is looked after, so
     * this doubles as how long attentive care takes to cure it. */
    uint32_t sickness_onset_seconds;

    /* What being ill costs. The multiplier is a percentage applied to every
     * decay rate (100 = unchanged); the drain is happiness lost on top of
     * it, independent of the stage's own happiness rate.
     *
     * Two levers rather than one, because they do different jobs: the
     * multiplier makes an ill creature harder to keep out of the neglected
     * condition at all, and the drain makes it visibly miserable even while
     * the player is keeping every bar topped up. Together they turn
     * "ignored for an afternoon" into a spiral instead of a plateau. */
    uint32_t sick_decay_multiplier_percent;
    uint32_t sick_happiness_drain_mp_per_hour;

    /* Accumulated neglect at which the creature dies. ZERO MEANS IT NEVER
     * DOES -- the same sentinel shape poop_interval_seconds == 0 uses for
     * "no mess". That is an off switch for permanent death, a product
     * decision Chris may want for a gentler mode, and it is also what lets
     * a check raise a badly-treated creature to adulthood without modelling
     * the token care it would otherwise need to survive. */
    uint32_t sickness_death_seconds;

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

    /* What a care action restores, by how the creature took it. The gap
     * between liked and disliked is the discovery signal: too small and the
     * player cannot tell which is which without a spreadsheet, too large
     * and getting it wrong is punishing rather than informative. */
    kf_pet_millipercent care_boost_liked_mp;
    kf_pet_millipercent care_boost_neutral_mp;
    kf_pet_millipercent care_boost_disliked_mp;

    /* What a bath adds to happiness on top of getting the creature clean.
     * Small on purpose: this is a bonus for paying attention, not a second
     * way to play with it.
     *
     * TWO values, not three. There is deliberately no disliked figure,
     * because the answer is always zero and a config field would invite
     * someone to make it negative -- and taking happiness away for washing
     * a creature that needed washing teaches the player to stop washing it. */
    kf_pet_millipercent bath_happiness_liked_mp;
    kf_pet_millipercent bath_happiness_neutral_mp;

    /* The care average, across the whole of CHILD, below which a creature
     * grows into the dust form instead of one of the four verb families.
     * See advance_to_next_stage() for why this is a threshold on the
     * average rather than "was it ever touched". */
    kf_pet_millipercent dust_care_average_mp;

    /* What waking a sleeping creature deliberately costs it, in happiness --
     * kf_pet_wake(), Task 7 of docs/superpowers/plans/2026-08-13-screens-
     * clock-sleep.md. The spec's own words: "kept small" -- this is a
     * config field, not a hardcoded constant, for the identical reason
     * every other care number in this struct is: a dev writes a pet by
     * configuring and skinning this, and there is no reason waking should
     * be the one number that cannot be tuned. */
    kf_pet_millipercent wake_happiness_cost_mp;

    /* The tuck-in payoff (2026-08-11 bedtime-behaviour extension, ADR 0052,
     * kf_pet_tuck_in() below): applied once to EACH of hunger/happiness/
     * energy at the asleep -> awake transition, but only for a creature
     * that was tucked in first -- see kf_pet_state::tucked_in. Chris's own
     * requirement: "more fulfilled when it wakes up compared to if it puts
     * itself to bed automatically", so this has to be big enough to notice
     * against a night's worth of ordinary decay without being a second way
     * to fully refill every need for free. FEEL, NOT ENGINEERING, same
     * status as wake_happiness_cost_mp just above and every other
     * illustrative figure kf_pet_default_config() sets -- flagged for Chris
     * to tune on the board. */
    kf_pet_millipercent tuck_in_wake_bonus_mp;
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

    /* How many poops are waiting to be cleaned, 0..KF_PET_MAX_POOPS.
     *
     * A COUNT, not a list. Where each one sits on screen is presentation and
     * belongs to the screen, which can place them from this number however
     * it likes. Keeping it a count is also what lets offline fast-forward
     * work without storing anything per-poop across a save. */
    uint8_t poop_count;

    /* Seconds remaining until the next poop appears. Counts down inside
     * apply_stage_segment(), so a device that was switched off for a day
     * comes back to a correctly messy pet rather than a clean one. */
    uint32_t seconds_until_next_poop;

    /* How dirty the creature itself is, 0..100000 mp. Rises with time and
     * faster while poops are waiting. Unlike the three needs this has no
     * decay direction of its own -- it only goes up, and only cleaning
     * brings it down. */
    kf_pet_millipercent dirtiness_mp;

    /* Seconds of accumulated neglect. Rises while the creature is in a
     * neglected condition and falls at the same rate while it is not -- so
     * one press of every button does not wipe out a day of damage, and a
     * creature that has been badly treated takes proportionally longer to
     * nurse back.
     *
     * ONE accumulator drives all three states: illness at
     * sickness_onset_seconds, death at sickness_death_seconds, and the
     * escalating distress the screen shows between them. Three separate
     * timers would have to be kept consistent with each other; this cannot
     * disagree with itself. */
    uint32_t neglect_seconds;

    /* Whether the creature is currently ill. Stored rather than derived
     * from neglect_seconds, because the thresholds are asymmetric on
     * purpose: it falls ill at sickness_onset_seconds but only recovers at
     * zero. That hysteresis is what stops a creature hovering at the
     * threshold flickering in and out of illness every frame, and it
     * cannot be recomputed from the accumulator alone. */
    bool sick;

    /* Whether this creature has died. Terminal within THIS file: nothing
     * in kf/pet.h or hakoniwaos/src/pet.cpp ever clears it, and the normal
     * care actions (feed/play/rest/bath/flush) do not touch it either. It
     * is not unreachable across the codebase, though --
     * kf_pet_session_debug_reset() and kf_pet_session_debug_jump_to_stage()
     * (simulator/src/pet/kf_pet_session.cpp) both call kf_pet_init() on the
     * live pet, which clears `dead` along with everything else. Those are
     * debug-only levers (KF_PET_SESSION_ENABLE_DEBUG_CONTROLS, on by
     * default including in the ESP32 build), reachable as `KFDBG RESET` /
     * `KFDBG JUMP` and from the SDL debug window -- not a hidden backdoor,
     * but real and worth knowing about before assuming dead is one-way. */
    bool dead;

    /* Whether the creature is currently asleep -- docs/superpowers/specs/
     * 2026-08-09-core-care-loop-design.md's "Sleep, settled": night is
     * 22:00-07:00 local (kf/clock.h's kf_clock_seconds_in_daily_window(),
     * Core does not re-derive the window), and falling asleep is entirely
     * automatic and entirely a function of the wall clock. Recomputed
     * inside apply_stage_segment() (kf_pet.cpp) every time the wall clock
     * is known, from the epoch that segment ends at; stays false for the
     * whole of EGG (apply_stage_segment() returns before ever reaching
     * the sleep computation for an egg -- eggs do not sleep, a deliberate
     * choice recorded there and in ADR 0048, not an oversight) and stays
     * false whenever last_advanced has never been valid (no clock, no
     * sleep -- the same "inert without a clock" rule the night-window
     * accounting itself follows). Saved, so a reload shows the pose the
     * creature actually ended the session in rather than guessing from
     * nothing.
     *
     * UPDATED by the 2026-08-11 bedtime-behaviour extension (ADR 0052):
     * "drowsy" (the ten minutes immediately before this flips true) is
     * still not a separate SAVED sub-state -- kf_pet_drowsy() below is a
     * pure, on-demand query exactly like kf_pet_wants(), computed fresh
     * every call from last_advanced, never stored -- but it is no longer
     * true that nothing in this file depends on it: kf_pet_tuck_in()'s
     * whole gate is "is kf_pet_drowsy() true right now". */
    bool asleep;

    /* Set by kf_pet_tuck_in() while the creature is drowsy, cleared the
     * instant the tuck-in bonus is actually paid out at the next asleep ->
     * awake transition (apply_stage_segment(), kf_pet.cpp) -- see that
     * function's own header comment and kf_pet_config::tuck_in_wake_bonus_mp
     * for what the bonus is. A plain bool, not folded into `asleep`, because
     * it has to survive THROUGH the whole of asleep=true (a creature can be
     * asleep AND tucked-in at once, for the whole night) and only stops
     * mattering once asleep flips back to false -- two different lifetimes,
     * two different fields, the same reasoning `sick` gets its own field
     * instead of being derived solely from `neglect_seconds` at read time.
     * Saved (ADR 0052, kSaveVersion 9->10): the whole point is that this
     * survives the device being switched off overnight, so the bonus still
     * lands when the offline fast-forward carries the creature through the
     * night while the device was off (kf_pet_load_and_advance()). Starts
     * false (kf_pet_init()) and stays false for a creature that never gets
     * tucked in -- exactly the "self-slept" case the request compares
     * against. */
    bool tucked_in;

    /* The 2026-08-11 OVERNIGHT-FLOOR extension (docs/architecture/
     * adr-0053-overnight-floors-poop-suppression.md): Chris, after testing
     * sleep and tuck-in on the board, ruled that needs decaying to zero (and
     * poop still piling up) while a pet slept defeated the whole point of
     * sleep existing. Rolled ONCE, at the awake -> asleep transition
     * (apply_stage_segment(), kf_pet.cpp), one independent kf_rng_below()
     * draw per need -- NOT the same value copied into all three, which is
     * exactly what "independent" is checked for. The band the roll is drawn
     * from depends on two things read AT THAT INSTANT: state->tucked_in (was
     * the player there to tuck it in, or did it put itself to bed?) and
     * whether the creature counts as "unwell" right then (sick, or any of
     * the three needs already below the unwell threshold) -- see pet.cpp's
     * kOvernightFloor* constants for the four bands' exact ranges.
     *
     * Applied EXACTLY ONCE, at the wake instant -- live play (continuously,
     * every apply_stage_segment() call, right up until the segment where
     * `asleep` flips back to false) and offline (at the closed-form-detected
     * wake instant for a jump that carries a segment past morning) both
     * resolve to that same single application, as `max(decayed, floor)` --
     * a SET-POINT, not a rate change: a pet already below its band when it
     * fell asleep is pulled UP into the band by morning, not merely stopped
     * from falling further. That is a deliberate reading of Chris's own
     * "would be a random value between 20 and 30%" phrasing (a MORNING
     * value), and it is a real design choice with a real consequence (sleep
     * partially rescues a neglected pet) -- see the ADR for the one-line
     * reversal to a pure floor if that turns out to be wrong.
     *
     * NOT re-applied on every segment that ends still asleep -- that was a
     * real defect (docs/architecture/adr-0053-overnight-floors-poop-
     * suppression.md's amendment, docs/reviews/2026-08-12-sleep-stack-
     * audit.md finding 1), fixed 2026-08-12: a live session flushes every
     * KF_PET_SESSION_FLUSH_SECONDS (30s), so re-clamping on every one of
     * those segments turned a wake-instant set-point into a value held all
     * night, feeding an inflated pre-sleep number into
     * care_integral_mp_seconds on every flush. The needs (and dirtiness)
     * are left to decay/rise UNPROTECTED for the rest of the night once the
     * floor is rolled; only the wake instant clamps.
     *
     * Cleared (set to 0) the moment the creature wakes -- 0 is never a real
     * floor (every band's minimum is >= 20000), so it doubles as "no floor
     * currently active" without a separate flag: apply_stage_segment() only
     * ever CONSULTS these fields while `asleep` is true (or was, earlier in
     * the same segment), never while genuinely awake.
     *
     * Saved (kSaveVersion 10->11, ADR 0053) for the identical reason
     * tucked_in is: the whole scenario that matters is the device switched
     * off overnight, and a floor that evaporates on reload would protect a
     * live session but not the common case. */
    kf_pet_millipercent hunger_floor_mp;
    kf_pet_millipercent happiness_floor_mp;
    kf_pet_millipercent energy_floor_mp;

    /* The overnight dirtiness CAP -- same rolling instant and same
     * tucked-in/unwell classification as the three floors above, but a
     * ceiling (`min(value, cap)`) rather than a floor, because dirtiness
     * RISES rather than falls and the three floors above do nothing for it.
     * Not randomised (a single value per band, not a range) -- see the ADR
     * for why a fixed value was judged sufficient here where the needs got a
     * rolled range. A separate stored field from the three floors above
     * because it is a different axis with a different direction, cleared
     * the same way (0 means "no cap active"; every real band value is
     * >= 25000, so 0 cannot be mistaken for one). */
    kf_pet_millipercent dirtiness_cap_mp;

    /* How the creature took the last care action, and which action it was.
     * Saved, so a creature reloaded mid-sulk is still sulking. This is the
     * feedback channel the spec's section 6 puts first: the reaction leads
     * and the bars confirm, because a reaction is readable at a glance on a
     * two-inch screen and a three-thousand-millipercent difference is not. */
    uint8_t last_reaction;    /* kf_pet_reaction */
    uint8_t last_care_action; /* kf_pet_care_action */

    /* The wall-clock time this state was last advanced to. Saved
     * alongside the needs (see kf_pet_save()) so a reload can compute
     * exactly how long the device was off and fast-forward by that much
     * -- see kf_pet_load_and_advance(). Invalid (kf_wall_time.valid ==
     * false) until the first successful advance.
     *
     * kf_pet_advance() ALSO carries this forward now (docs/superpowers/
     * specs/2026-08-09-core-care-loop-design.md's "Sleep, settled"), by
     * exactly the elapsed_seconds it was just handed, whenever it is
     * already valid -- so a long-running live session (many small
     * kf_pet_advance() calls, one per frame-batch flush) keeps this
     * tracking real time exactly as an offline jump does, without
     * kf_pet_advance() ever calling into the HAL itself. That is what
     * lets sleep's night-window test (kf/clock.h's
     * kf_clock_seconds_in_daily_window()) run identically whether the
     * elapsed time came from one offline gap or a thousand live frames.
     * Still left untouched (stays invalid) when it starts invalid --
     * every check in this codebase that pokes a fresh kf_pet_state
     * directly and never goes through kf_pet_load_and_advance() relies on
     * that, and it is also the honest answer: with no clock reading ever
     * established, there is no baseline epoch to carry forward. */
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

    /* How many care actions this creature has EVER received. Not a rate, not
     * decayed, never reset: the only question it answers is "has anyone ever
     * touched this at all", which is what separates a neglected creature
     * (cared for badly) from an abandoned one (never cared for). Saturates
     * rather than wrapping -- the difference between 0 and 1 is the only one
     * that matters. */
    uint32_t care_actions_taken;
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
 * Clamps every need to [0, KF_PET_MILLIPERCENT_MAX]. Still never reads a
 * clock -- no HAL call anywhere in this function, matching this file's own
 * header comment -- but it DOES now carry `state->last_advanced` forward by
 * exactly `elapsed_seconds`, whenever it was already valid coming in (see
 * that field's own comment in kf_pet_state above for why). That is a plain
 * arithmetic update on a value already sitting in `state`, not a clock
 * read: the caller (kf_pet_load_and_advance() below, establishing the very
 * first baseline from a real wall-clock reading, or a live frame-loop
 * caller passing a per-frame-batch delta) is still the only place an
 * actual HAL time reading ever enters this file. This is what lets
 * sleep's night-window accounting (kf/clock.h) evaluate correctly during
 * LIVE play, not only immediately after a reload -- see docs/superpowers/
 * specs/2026-08-09-core-care-loop-design.md's "Sleep, settled" and this
 * file's own apply_stage_segment(). */
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

/* Care actions. Each raises its need and clamps at KF_PET_MILLIPERCENT_MAX
 * -- feeding an already-full creature does nothing extra, it does not bank
 * overfeeding against future decay.
 *
 * `variation` is which of the KF_PET_CARE_VARIATION_COUNT ways of doing it
 * was chosen. How much it restores depends on how this creature's base
 * trait feels about that variation (kf_pet_reaction_to()), and the reaction
 * is recorded on the state for the screen to show. Out-of-range is treated
 * as neutral, not rejected: a cartridge passing a bad index should get a
 * dull creature, not a dead one. */
void kf_pet_feed(kf_pet_state *state, const kf_pet_config *config,
                  uint8_t variation);
void kf_pet_play(kf_pet_state *state, const kf_pet_config *config,
                  uint8_t variation);
void kf_pet_rest(kf_pet_state *state, const kf_pet_config *config,
                  uint8_t variation);

/* Washes the creature: dirtiness to zero, always, whichever variation was
 * chosen. Being clean is a need, not a treat, so a creature never ends up
 * dirty because it disliked the flannel.
 *
 * What preference buys is a little happiness on top -- a noticeable lift
 * for the way it likes, a barely-there one for a way it merely tolerates,
 * and nothing at all for the way it hates. Nothing NEGATIVE either: being
 * washed in a way it dislikes is still being clean, and punishing the
 * player for meeting a need would teach them not to meet it.
 *
 * Leaves poops alone entirely. That is kf_pet_flush(). */
void kf_pet_bath(kf_pet_state *state, const kf_pet_config *config,
                  uint8_t variation);

/* Clears every waiting poop. One way to do it, no variations, no opinion,
 * no effect on any need -- a chore, not care.
 *
 * It does change how fast the creature gets dirty, though not by touching
 * dirtiness itself: waiting poops accelerate dirtying (see
 * kf_pet_config::dirtiness_rise_per_poop_mp_per_hour), so clearing them
 * returns that rise to its baseline. A player who flushes but never baths
 * has slowed the problem without solving it, which is the right shape for
 * two actions that both live under "keep it clean". */
void kf_pet_flush(kf_pet_state *state);

/* Wakes a sleeping creature deliberately -- Task 7 of docs/superpowers/
 * plans/2026-08-13-screens-clock-sleep.md, the care-loop spec's own words:
 * "Waking it deliberately is allowed and costs happiness. The original
 * punished you for leaving the light on; this is the same idea, kept
 * small." A no-op if the creature is already awake, or dead: there is
 * nothing to wake either way, and a dead creature's happiness is not this
 * function's business (kf_pet_feed()/etc. all take the identical
 * `if (state->dead) return;` guard for the same reason).
 *
 * Sets `state->asleep = false` directly -- Core does not need to know WHY
 * the creature is awake, only that it is (ADR 0048's own reasoning for why
 * `asleep` has no richer sub-state). The next segment
 * apply_stage_segment() runs will simply recompute `asleep` fresh against
 * the wall clock, same as always; nothing here needs to suppress that. */
void kf_pet_wake(kf_pet_state *state, const kf_pet_config *config);

/* Whether the creature is drowsy right now -- the 2026-08-11 bedtime-
 * behaviour extension (ADR 0052), Chris's own request: "extend the
 * 'drowsy' timeframe to start 10 minutes before actual bedtime". A pure
 * query, exactly the same shape as kf_pet_wants() just below (reads
 * `state`, touches nothing, decides nothing, computed fresh every call
 * from state->last_advanced rather than stored -- see kf_pet_state::asleep
 * for why there is still no separate SAVED drowsy sub-state). The window
 * is derived from the SAME night-window constants apply_stage_segment()
 * evaluates `asleep` against (kNightStartHour, pet.cpp) -- there is no
 * second, independently-hardcoded bedtime hour anywhere in this file.
 *
 * False whenever there is nothing to be drowsy ABOUT: dead, already
 * asleep, an egg (eggs do not sleep -- ADR 0048 -- so they have no bedtime
 * to be drowsy before), or the wall clock has never been established
 * (last_advanced.valid == false, the same "inert without a clock" rule
 * every other clock-driven query in this file follows). Otherwise true for
 * exactly the KF_PET_DROWSY_WINDOW_SECONDS immediately before the night
 * window begins. */
bool kf_pet_drowsy(const kf_pet_state *state);

/* Tucks the creature in -- the 2026-08-11 bedtime-behaviour extension (ADR
 * 0052), Chris's own words: "if you notice while it's drowsy and hit the b
 * button, the command tucks it in for a boost in care needs the next day
 * when it wakes up". A NO-OP unless kf_pet_drowsy(state) is true right now
 * -- which already rules out a dead, sleeping, wide-awake, or egg-stage
 * creature, so this function does not repeat those checks itself, only
 * defers to kf_pet_drowsy(). Idempotent: tucking in an already-tucked-in
 * creature just sets the same flag again, harmless, not a double bonus (see
 * kf_pet_state::tucked_in and apply_stage_segment()'s own comment on where
 * the bonus is actually paid and the flag cleared, exactly once, at the
 * asleep -> awake transition). Presentation (the futon appearing early) is
 * the game layer's decoration, same as ever -- this function only ever
 * sets the one saved bit the offline path needs to still know about it the
 * next time this pet's save is loaded. */
void kf_pet_tuck_in(kf_pet_state *state);

/* The attention signal -- Task 8 of docs/superpowers/plans/2026-08-13-
 * screens-clock-sleep.md. What the creature wants right now, if anything,
 * from the FIVE things a player can actually do to it: feed, play, rest,
 * bath, flush. There is deliberately no MEDICINE -- nothing in this file
 * cures sickness directly, so there is no action for a creature to want
 * that would do that. NONE is 0, the same "falsy default" convention every
 * other enum in this file uses.
 *
 * Order matches kf_pet_wants()'s own priority order, purely for
 * readability -- nothing reads these as a magnitude. */
typedef enum {
    KF_PET_WANT_NONE = 0,
    KF_PET_WANT_FOOD = 1,
    KF_PET_WANT_REST = 2,
    KF_PET_WANT_BATH = 3,
    KF_PET_WANT_FLUSH = 4,
    KF_PET_WANT_PLAY = 5,
} kf_pet_want;

/* Hysteresis thresholds for kf_pet_wants() below -- one ON/OFF pair per
 * want, named constants rather than kf_pet_config fields, because unlike
 * the decay rates and durations in that struct, WHEN a creature starts
 * asking for something is not something a cartridge author is expected to
 * re-tune per pet; it is closer to KF_PET_DIRTY_FLIES_MP/STINK_MP just
 * above, which get the identical compile-time-constant treatment for the
 * identical reason.
 *
 * FOOD/REST/PLAY read the ON value as "at or below" (these three needs get
 * WORSE going down) and the OFF value as "at or above" -- a strictly
 * higher, more-recovered value the need must climb back past before the
 * want clears. The 15000 mp gap (25% -> 40%) is chosen against
 * kf_pet_default_config()'s own fastest rate (BABY hunger, 66000 mp/hour,
 * roughly 18 mp/sec) so that ordinary decay -- which only ever moves a need
 * in ONE direction between care actions, see kf_pet_state's own fields --
 * cannot cross a 15000 mp gap inside a single second, let alone cross it
 * twice. 25% itself matches creature.lua's own `classify()` "low" band
 * (its "starting to get hungry"/"could use some playtime"/"getting a
 * little tired" messages already fire in that neighbourhood), not the
 * demand-curve doc's separate "needs attention at 70%" language -- that
 * 70% figure describes how fast the decay RATES were tuned, not when a
 * disruptive stop-and-pose signal should interrupt the wander, and firing
 * this signal that early (every ~27 real minutes for a baby) would make it
 * background noise instead of a genuine attention signal.
 *
 * BATH is inverted (dirtiness gets WORSE going UP), so it reuses
 * KF_PET_DIRTY_STINK_MP/FLIES_MP directly rather than inventing a second
 * pair of numbers for the same need.
 *
 * FLUSH is a plain count, not millipercent: poop_count only ever rises by
 * one at a time (kf_pet_config::poop_interval_seconds) or resets straight
 * to 0 (kf_pet_flush()), so a 1-poop gap already cannot flap -- there is no
 * decay path that adds a fractional poop. Kept as a real ON/OFF pair
 * anyway, for the same reason every other want has one: consistency the
 * next want added to this list should not have to break.
 *
 * ALL SIX ARE A STARTING POINT, NOT A SETTLED DESIGN -- Task 8's own note
 * in the plan. Feel, for Chris to judge on the board. */
#define KF_PET_WANT_FOOD_ON_MP 25000u   /* 25% */
#define KF_PET_WANT_FOOD_OFF_MP 40000u  /* 40% */
#define KF_PET_WANT_REST_ON_MP 25000u
#define KF_PET_WANT_REST_OFF_MP 40000u
#define KF_PET_WANT_PLAY_ON_MP 25000u
#define KF_PET_WANT_PLAY_OFF_MP 40000u
#define KF_PET_WANT_BATH_ON_MP KF_PET_DIRTY_STINK_MP  /* 80% */
#define KF_PET_WANT_BATH_OFF_MP KF_PET_DIRTY_FLIES_MP /* 50% */
#define KF_PET_WANT_FLUSH_ON_POOPS 3u
#define KF_PET_WANT_FLUSH_OFF_POOPS 1u

/* A pure query: reads `state` and returns, touches nothing, decides
 * nothing -- exactly kf_pet_dominant_care_trait()'s own shape just below,
 * and for the same reason (see that function's header comment). NONE while
 * dead or asleep, unconditionally -- a corpse or a sleeping creature has
 * nothing to ask for, and Task 6 (ADR 0048) landed specifically so this
 * second half is expressible without Core re-deriving anything about sleep
 * here.
 *
 * `previous` is the ONLY thing that makes this genuinely pure AND
 * genuinely hysteretic at the same time, and it deserves explaining: real
 * hysteresis needs memory of what was true a moment ago -- an ON/OFF pair
 * cannot be evaluated from the instantaneous need alone, because a need
 * sitting between the two thresholds means something different depending
 * on which direction it arrived from. Every other stateful signal in this
 * file (kf_pet_state::sick is the clearest example -- see its own comment)
 * keeps that memory as a STORED, SAVED bit. This one deliberately does not:
 * the plan's own requirement is "no new save field, no new state to
 * migrate", so instead of Core remembering what it last reported, the
 * CALLER hands the last answer back in as an argument, the same "previous
 * value in, fresh value out" shape a UI edge-detector uses (see
 * sdl_debug_window.cpp's own `previous_pressed`) -- except here the caller
 * is kf_pet_session_wants() (simulator/src/pet/kf_pet_session.h), which
 * holds that one kf_pet_want in ordinary (non-Core, non-saved) session
 * memory. This function itself never stores anything and never mutates
 * `state`; call it twice with the same two arguments and it returns the
 * same answer both times, which is what "pure" means here.
 *
 * Only `previous` gets the generous OFF threshold -- whichever want was
 * reported last time is the one at risk of flapping, so it alone is
 * checked against the OFF value; every other want is evaluated against its
 * plain ON value, since a fresh crossing into "wanting" needs no history to
 * be trusted.
 *
 * Priority order when more than one is unmet, highest first: FOOD, REST,
 * BATH, FLUSH, PLAY. The plan's own reasoning: the first three are what
 * neglect actually punishes, and PLAY is last because a creature that is
 * hungry, exhausted and filthy asking to play reads as broken. Kept as
 * this plan's starting point rather than switching to "whichever need is
 * most severe" -- the fixed order is simpler to reason about from outside
 * Core (a script or a player can learn it once) and the plan itself frames
 * severity-based ordering as merely "a defensible alternative", not a
 * requirement. */
kf_pet_want kf_pet_wants(const kf_pet_state *state, kf_pet_want previous);

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
 * 0021 (life stages/evolution), to version 3 with ADR 0023 (personality
 * traits), to version 4 with the evolution-tree reconciliation
 * (docs/superpowers/plans/2026-08-09-evolution-tree-reconciliation.md):
 * `care_actions_taken` was added and the valid range of `teen_form` grew to
 * include KF_PET_TEEN_FORM_DUST, so an older save's `teen_form` would be
 * misread rather than merely missing a field, and to version 5 with mess
 * (docs/superpowers/plans/2026-08-09-mess.md): `poop_count`,
 * `seconds_until_next_poop` and `dirtiness_mp` were added, to version 6
 * with sickness (docs/superpowers/plans/2026-08-09-sickness-and-death.md):
 * `neglect_seconds` and `sick` were added -- a version-5 save has no
 * accumulated neglect to fall back to that would not silently un-sicken a
 * creature that was ill at save time, so it is refused rather than guessed
 * at -- and to version 7, in the same plan, with death: `dead` was added.
 * Bumped to version 8 with care variations (docs/superpowers/plans/
 * 2026-08-09-care-variations.md): `last_reaction` and `last_care_action`
 * were added, so a creature reloaded mid-sulk is still sulking rather than
 * silently forgetting how the last care action went. Bumped to version 9
 * with sleep (docs/superpowers/plans/2026-08-13-screens-clock-sleep.md's
 * Task 6, ADR 0048): `asleep` was added -- a version-8 save has no notion
 * of whether the creature was asleep, and defaulting it to false on load
 * would be no more honest than guessing, so it is refused rather than
 * silently trusted, the same accepted cost every version bump before this
 * one already took. And bumped again to version 10 with the 2026-08-11
 * bedtime-behaviour extension (ADR 0052): `tucked_in` was added -- a
 * version-9 save has no notion of whether a tuck-in bonus is still owed, so
 * a version-9 save is refused, not defaulted to false (which would happen
 * to be silently correct for a creature that was never tucked in, and
 * silently wrong -- one lost bonus, no worse than that, but still a guess
 * -- for one that was). Chris was asked directly about a migration path and
 * said no: "no save migration needed now during development. I'll let you
 * know if I need it at some point." So there is none here either, matching
 * the identical policy every earlier bump in this file already took.
 * A save from an earlier version is refused by kf_pet_load_and_advance()'s
 * unpack() step and falls back to a fresh pet, exactly the behaviour ADR
 * 0015 already established for any unrecognised version -- no migration
 * code, an explicit, accepted cost.
 *
 * Bumped again to version 11 with the 2026-08-11 overnight-floor extension
 * (ADR 0053, docs/architecture/adr-0053-overnight-floors-poop-suppression.md):
 * `hunger_floor_mp`, `happiness_floor_mp`, `energy_floor_mp` and
 * `dirtiness_cap_mp` were added -- a version-10 save has no notion of a
 * floor rolled but not yet paid out, so it is refused rather than defaulted
 * to 0 (which would happen to be correct for a creature that was awake at
 * save time, and silently wrong -- a missing floor for the rest of that
 * night -- for one saved mid-sleep). Chris was not re-asked about migration
 * for this bump; the identical "no migration during development" answer
 * from the version-10 bump is treated as still standing. */
#define KF_PET_SAVE_KEY "pet"
#define KF_PET_SAVE_BYTES 109u /* see kf_pet.cpp's pack()/unpack() for the exact layout */

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
