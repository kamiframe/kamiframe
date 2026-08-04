/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * The pet simulation framework: needs, decay, and offline fast-forward.
 * See docs/architecture/adr-0015-pet-simulation-framework.md.
 *
 * "This is your secret weapon and the thing no generic console has" --
 * 02-min-spec-sheet.md, item 4. A dev writes a pet by configuring and
 * skinning this before ever writing custom logic, which is why decay
 * rates are a kf_pet_config a caller supplies, not a constant baked in
 * here.
 *
 * Pure Core logic: no HAL calls inside kf_pet_advance(), kf_pet_feed(),
 * kf_pet_play() or kf_pet_rest(), by design -- see
 * 08-phase1-slice1-decisions.md's HAL boundary table: "Pet simulation,
 * needs, decay, evolution | Core | Pure logic. Should be unit-testable
 * with no HAL at all." Only kf_pet_load_and_advance() and kf_pet_save()
 * touch the HAL (storage and time), and they are thin wrappers around the
 * pure functions above, not where the actual maths lives.
 *
 * Not yet built, on purpose -- see the ADR's "what deliberately is not
 * built": life stages, evolution, personality traits, care-mistake
 * tracking, the random event scheduler. This slice is needs, decay and
 * offline fast-forward alone -- the piece the hardware-purchase trigger
 * ("save + offline fast-forward working," see README.md's roadmap)
 * actually depends on.
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

/* Decay rates, in millipercent per hour. Config, not a constant, because
 * "a dev writes a pet by configuring and skinning this" is the whole
 * point -- see the header comment above. kf_pet_default_config() below
 * returns illustrative values for the demo and the determinism check,
 * not a recommendation for any real pet's tuning. */
typedef struct {
    uint32_t hunger_decay_mp_per_hour;
    uint32_t happiness_decay_mp_per_hour;
    uint32_t energy_decay_mp_per_hour;
} kf_pet_config;

/* A reasonable illustrative default: hunger drains fastest (empty from
 * full in a bit over 4 days), energy slowest (a bit over 8), happiness in
 * between. Not tuned against anything real -- there is no real pet yet to
 * tune it against, see the ADR. */
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
} kf_pet_state;

/* A fresh pet: every need full, last_advanced invalid. */
void kf_pet_init(kf_pet_state *state);

/* Applies decay for exactly `elapsed_seconds`, in ONE closed-form step --
 * not simulated second by second, which is what makes "the device was
 * off for 3 days" a multiply, not three days of loop iterations. Clamps
 * every need to [0, KF_PET_MILLIPERCENT_MAX]. Deliberately does NOT read
 * a clock or touch `last_advanced`: the caller (kf_pet_load_and_advance()
 * below, or a future frame-loop caller passing a per-frame delta) decides
 * what "elapsed" means and updates the timestamp itself, which is what
 * keeps this function trivially unit-testable with an arbitrary elapsed
 * value and no HAL in the picture at all. */
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
 * MSVC) are obliged to keep identically. */
#define KF_PET_SAVE_KEY "pet"
#define KF_PET_SAVE_BYTES 22u /* 1 version + 3*4 need fields + 1 valid + 8 epoch */

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
 * error.
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
