/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * Task 2 of the Nibble-and-the-game-session plan. Owns the ONE in-flight
 * minigame session, its per-game storage (one "g_"+id key per game --
 * see kf_game_session_begin()'s own comment on why a corrupt record must
 * cost one game's high score, never every game's, and never the pet's own
 * save), and the reward application at the end of a round. Mirrors kf_pet_
 * session.h's own split: kf/game.h is pure record and tier maths with no
 * notion of "the" live session, exactly the way kf/pet.h is pure with no
 * notion of "the" live pet; this file is the something that owns the one
 * instance a running build actually uses and gives it a name the Lua
 * binding (Task 3, sdk/lua/kf_lua_port.cpp's game.* table) can reach.
 *
 * A game session and a pet care action are DIFFERENT EVENTS -- see kf_pet_
 * session.h's kf_pet_session_reward_need()/_spend_need() for the function
 * this file routes every reward and every energy cost through instead of
 * kf_pet_session_feed()/_play()/_rest(), and why that distinction is the
 * entire reason Task 2 exists.
 *
 * Lives in simulator/, not hakoniwaos/, for the identical reason kf_pet_
 * session.h does: this is a simulator-only orchestration layer over Core's
 * pure kf/game.h maths, not Core itself.
 */

#ifndef KF_GAME_SESSION_H
#define KF_GAME_SESSION_H

#include "kf/game.h"
#include "kf_pet_session.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The numbers a game needs to adapt itself, snapshotted at kf_game_
 * session_begin() -- BEFORE that call spends the energy cost below, not
 * after, so a game reads how tired the pet already was going in, not how
 * tired spending the ticket just made it. `handicap_percent` is 0 at or
 * above 50% energy, rising linearly (integer maths only) to 100 at 0%
 * energy -- Chris's "visible handicap, never a lockout" decision (the
 * design's section 1.4): a game reads this and plays worse against a
 * tired pet, but kf_game_session_begin() itself never refuses for low
 * energy, only for a dead or sleeping one. A game is free to ignore this
 * value entirely; Nibble (examples/creature_demo/nibble.lua) does not. */
typedef struct {
    uint32_t energy_mp;
    uint8_t stage;
    uint8_t base_trait;
    uint8_t handicap_percent;
} kf_game_session_context;

/* Begins a session for game `id`, spending `energy_cost_mp` up front --
 * WIN OR LOSE, unconditionally, the instant this call succeeds. This is
 * the load-bearing piece of the whole slice: it is what stops a player
 * farming one game forever for free rewards, and it is what makes the
 * pet's own state matter to a game rather than only the reverse. Spent via
 * kf_pet_session_spend_need(KF_PET_NEED_ENERGY, ...) -- see that
 * function's own header comment (kf_pet_session.h) for why this is not
 * kf_pet_session_rest() run backwards.
 *
 * Returns false, touching nothing, in exactly two cases: the pet is dead
 * or asleep (never for low energy -- see kf_game_session_context's own
 * comment above), or `id` is empty or longer than 13 characters (the
 * store key is "g_" + id, and KF_STORE_MAX_KEY_LEN is 15 -- kf/budget.h).
 * A caller that gets false back has spent nothing and started no session.
 *
 * UNDERSCORE, NOT A DOT: the design and the plan this implements both
 * describe the key as "g." + id (e.g. "g.nibble"). kf_store_write()'s own
 * documented contract (kf/hal/storage.h) restricts every key to
 * `[A-Za-z0-9_]` -- a literal '.' is rejected with KF_ERR_INVALID, the
 * same NVS-derived restriction kf_pet_session.h's own KF_PET_DEATH_CAUSE_
 * KEY already has to live inside. Found by running this task's own
 * headless check, which failed every kf_store_write() with KF_ERR_INVALID
 * until this changed -- "g_" keeps the same two-character prefix and the
 * same 13-character id budget, so nothing else about the design's
 * reasoning moves.
 *
 * `need`/`need_fraction_percent` are the game's own manifest reward table
 * (examples/creature_demo/nibble.lua's `reward.need`/`need_fraction_
 * percent`) -- carried in here, alongside `id` and `energy_cost_mp`,
 * because all four are the SAME kind of thing: manifest content the
 * caller already has in hand before a session exists, not something
 * derived from how the session plays out. `need` is KF_PET_NEED_NONE for
 * a game with no secondary need (an empty `reward.need`); `need_fraction_
 * percent` is ignored when `need` is NONE. See kf_game_session_end()
 * below for how the two are actually spent. */
bool kf_game_session_begin(const char *id, uint32_t energy_cost_mp,
                            kf_pet_need need, uint32_t need_fraction_percent,
                            kf_game_session_context *out);

/* Accumulates `points` into the running session score -- a no-op that
 * logs, never a panic, if no session is currently in flight (a cartridge
 * bug, or a stray call after kf_game_session_end() already ran, must not
 * bring the firmware down over a missed score). Saturates at UINT32_MAX
 * rather than wrapping, the same defensive ceiling kf_game_record_apply()
 * (kf/game.h) already applies to `total_score` -- unreachable through any
 * real eight-round session, but a session that never reaches that many
 * points is exactly the case a saturating add costs nothing to guard for
 * free. */
void kf_game_session_score(uint32_t points);

/* Records a named in-round event. Only "perfect" has any effect right now
 * -- it increments the in-flight perfect_count that flows into the
 * persisted kf_game_record's own field of the same name at kf_game_
 * session_end(). Every other name, including ones a future game invents
 * for its own purposes, is silently ignored, NOT an error: a cartridge
 * calling game.event() with a name this build does not recognise must
 * never crash the firmware over it. A no-op if no session is in flight,
 * matching kf_game_session_score() above. */
void kf_game_session_event(const char *event);

/* Ends the in-flight session: computes the tier against `good_threshold`/
 * `great_threshold` (kf_game_tier_for(), kf/game.h), applies rewards,
 * persists the updated kf_game_record under this game's own store key,
 * and clears the in-flight session. Returns KF_GAME_TIER_MISS, touching
 * nothing further, if no session was in flight -- a no-op that logs, never
 * a panic, the same contract kf_game_session_score()/_event() already
 * have.
 *
 * MISS pays nothing and subtracts nothing beyond the energy already spent
 * at kf_game_session_begin() -- Chris's "losing costs only the energy
 * already spent" decision (the design's section 1.5). GOOD and GREAT both
 * pay happiness via kf_pet_session_reward_need(), scaled by kf_pet_
 * reaction_to() for THIS game's own id (a stable per-id variation index,
 * not the numeric variation a hand-performed care action cycles through --
 * see the .cpp), against config->care_boost_{liked,neutral,disliked}_mp
 * (kf_pet_session_config()) as "a full grant" -- the exact amount a real
 * kf_pet_play() would have granted for that reaction, so a game's reward
 * and a hand-performed care action never quietly disagree about what a
 * full grant of happiness is worth. If this game declared a secondary
 * need at kf_game_session_begin() (need != KF_PET_NEED_NONE), that need
 * ALSO rises, by need_fraction_percent of ITS OWN full grant (the same
 * reaction-scaled lookup, against the care action that need corresponds
 * to) -- this is what makes need_fraction_percent expressible at all:
 * kf_pet_feed() grants whatever a full meal is worth and has no
 * fractional form (the design's section 2.1).
 *
 * GOOD and GREAT are deliberately paid the SAME magnitude here -- there is
 * no separate "great bonus" field anywhere in a game's manifest (compare
 * kf_pet_config's own bath_happiness_*_mp, which has no disliked figure
 * for the identical reason: a field nothing populates is an invitation to
 * populate it with something ill-considered later). The tier still means
 * something to the PLAYER (a picker screen showing GOOD vs GREAT, a
 * different fanfare) even though the pet-side reward does not scale with
 * it a second time on top of the reaction. */
kf_game_tier kf_game_session_end(uint32_t good_threshold,
                                  uint32_t great_threshold);

/* Reads game `id`'s persisted record, for a picker screen. Returns false
 * -- `out` left untouched -- if the game has never been played, or its
 * stored record could not be read back at all (wrong size, wrong version,
 * or a backend error): both cases mean "nothing meaningful to show",
 * exactly the same "missing and corrupt degrade identically" contract
 * kf_pet_session_last_death_cause()'s own `known` flag already has for
 * the pet's death record. Never panics on a corrupt record -- a bad game
 * record costs one game's high score, never the pet's own save (see this
 * header's own top-of-file comment). */
bool kf_game_session_record(const char *id, kf_game_record *out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KF_GAME_SESSION_H */
