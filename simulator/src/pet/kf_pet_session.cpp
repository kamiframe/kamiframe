/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 */

#include "kf_pet_session.h"

#include "kf/clock.h"
#include "kf/hal/log.h"
#include "kf/hal/storage.h"
#include "kf/hal/time.h"

#include <cstdint>

/* Two flags, not one -- see kf_pet_session.h's "DEBUG ONLY" section
 * header comment for the full reasoning; this is the short version.
 *
 * KF_PET_SESSION_ENABLE_DEBUG_CONTROLS gates the four cheap functions
 * (_debug_advance/_debug_reset/_debug_age_seconds/_debug_jump_to_stage) --
 * thin wrappers over kf_pet_advance()/kf_pet_init() with no static memory
 * of their own. ports/esp32/main/CMakeLists.txt turns this ON: it is the
 * ESP32 build's only way to reach the pet session's debug fast-forward at
 * all, via ports/esp32/main/kf_dbg_bridge.cpp's KFDBG ADVANCE/RESET/MULT
 * (ADR 0030) and KFDBG JUMP (ADR 0034) -- _debug_jump_to_stage() rides
 * this flag rather than a new one because it costs exactly what its three
 * siblings already cost, and is now called both from sdl_debug_window.cpp
 * (desktop) and kf_dbg_bridge.cpp's handle_jump() (ESP32).
 *
 * KF_PET_SESSION_ENABLE_DEBUG_TOOLS gates _debug_seek() and the
 * scrubbable-timeline snapshot ring backing it -- the genuinely expensive
 * part, kDebugSnapshotCapacity times sizeof(DebugSnapshot) north of 200KB,
 * which has no business being an unconditional static allocation on a
 * device with 512KB of internal SRAM that also needs room for LVGL, Lua
 * and FreeRTOS itself. This is the same "enforce the device's limits,
 * don't just document them" rule ADR 0006 already applies everywhere else
 * in this codebase. ports/esp32/main/CMakeLists.txt turns this OFF --
 * unchanged from before this file had two flags instead of one. The same
 * flag also gates _state_mutable_for_test() below (Task 5): not expensive
 * like the snapshot ring, but the same "no business being reachable from a
 * real device" status, so it rides this flag rather than a third one.
 *
 * Both default ON (1); every backend other than ESP32 gets the full
 * desktop behaviour with nothing to opt into. When either is 0, every
 * function it gates is still DECLARED in kf_pet_session.h (callers don't
 * need to care) but not DEFINED here -- calling one from a backend that
 * has it off is a link error, which is the correct outcome: none of them
 * are meant to be reachable from a backend that disabled them. */
#ifndef KF_PET_SESSION_ENABLE_DEBUG_TOOLS
#define KF_PET_SESSION_ENABLE_DEBUG_TOOLS 1
#endif
#ifndef KF_PET_SESSION_ENABLE_DEBUG_CONTROLS
#define KF_PET_SESSION_ENABLE_DEBUG_CONTROLS 1
#endif

namespace {

constexpr const char *TAG = "pet-session";

#if KF_PET_SESSION_ENABLE_DEBUG_TOOLS || KF_PET_SESSION_ENABLE_DEBUG_CONTROLS

/* How many seconds of a stage's own duration have already been lived
 * through as of `stage` starting -- i.e. where `stage` begins on the
 * pet's own lifetime clock. Adult has no duration of its own (it is
 * terminal, see kf/pet.h), so this is also the total axis length the
 * debug window's timeline draws tick marks across. Pure function of
 * config + stage, no session state involved, which is why this lives as
 * a free function rather than a Session member. Gated on EITHER debug
 * flag, not just TOOLS: kf_pet_session_debug_age_seconds() (CONTROLS)
 * needs total_age_seconds() below just as much as debug_snapshot_push()
 * (TOOLS) does. Nothing outside those two needs a pet's age in isolation
 * from its state. */
uint64_t elapsed_before_stage(const kf_pet_config &config, kf_pet_stage stage) {
    uint64_t t = 0u;
    if (stage > KF_PET_STAGE_EGG) {
        t += config.egg_duration_seconds;
    }
    if (stage > KF_PET_STAGE_BABY) {
        t += config.baby_duration_seconds;
    }
    if (stage > KF_PET_STAGE_CHILD) {
        t += config.child_duration_seconds;
    }
    if (stage > KF_PET_STAGE_TEEN) {
        t += config.teen_duration_seconds;
    }
    return t;
}

uint64_t total_age_seconds(const kf_pet_state &state, const kf_pet_config &config) {
    return elapsed_before_stage(config, state.stage) + state.stage_elapsed_seconds;
}

#endif // KF_PET_SESSION_ENABLE_DEBUG_TOOLS || KF_PET_SESSION_ENABLE_DEBUG_CONTROLS

#if KF_PET_SESSION_ENABLE_DEBUG_TOOLS

/* One full state snapshot, timestamped on the pet's own lifetime clock
 * (see total_age_seconds() above), not a wall-clock or session-uptime
 * reading. See kf_pet_session_debug_seek()'s header comment in
 * kf_pet_session.h for why snapshots, not an action-replay log: the
 * error this trades away (an approximated care_integral at the exact
 * moment of a stage transition) is bounded by snapshot SPACING, which
 * this file controls tightly (every state change), rather than by
 * however far apart a player's care-action clicks happen to land.
 *
 * TOOLS-only, unlike total_age_seconds() above: the ring this backs is
 * exactly the "expensive timeline" half of the DEBUG ONLY split -- see
 * kf_pet_session.h's header comment. */
struct DebugSnapshot {
    kf_pet_state state;
    uint64_t age_seconds;
};

/* 2048 * sizeof(DebugSnapshot) -- desktop-simulator-only process memory,
 * nothing close to an arena budget (this file has none; see
 * kf_pet_session.h's own header comment on why this lives in
 * simulator/, not hakoniwaos/), and gone entirely from the build when
 * KF_PET_SESSION_ENABLE_DEBUG_TOOLS is 0 (see this file's top-of-file
 * comment) -- independent of KF_PET_SESSION_ENABLE_DEBUG_CONTROLS, which
 * does not affect this ring at all. Generous enough that a normal debug
 * session never evicts its own history; a sustained high-multiplier run
 * (see sdl_debug_window.cpp's 256x) can still fill it, at which point the
 * ring starts overwriting its OLDEST entries -- see debug_snapshot_push()
 * below. */
constexpr size_t kDebugSnapshotCapacity = 2048u;

#endif // KF_PET_SESSION_ENABLE_DEBUG_TOOLS

struct Session {
    kf_pet_state state{};
    kf_pet_config config{};
    uint64_t last_call_us = 0;
    /* Elapsed milliseconds accumulated but not yet applied via
     * kf_pet_advance() -- can exceed 1000 while waiting to cross
     * KF_PET_SESSION_FLUSH_SECONDS. See kf_pet_session_frame()'s header
     * comment in kf_pet_session.h for why this batches rather than
     * flushing every whole second. */
    uint64_t pending_ms = 0;
    bool ready = false;

    /* Task 8's attention-signal memory -- see kf_pet_session_wants()'s own
     * header comment in kf_pet_session.h. Session-only, never saved. */
    kf_pet_want last_want = KF_PET_WANT_NONE;

    /* ADR 0056 -- the periodic-checkpoint dirty flag and its deadline. See
     * KF_PET_SESSION_PERIODIC_SAVE_SECONDS's own comment (kf_pet_session.h)
     * for the endurance reasoning; see save() below for exactly when each
     * gets touched. Neither is ever saved to disk -- both describe the gap
     * between memory and disk, which is meaningless once the two agree. */
    bool dirty = false;
    uint64_t next_periodic_save_us = 0;

    /* TEST ONLY (see kf_pet_session_debug_save_attempt_count() in
     * kf_pet_session.h) -- incremented unconditionally in production too
     * (there is no TOOLS #if around the increment site itself, only
     * around the getter), because it costs one integer increment per save
     * and gating it would mean two different call counts depending on
     * which backend compiled this file -- not worth the divergence for a
     * counter nothing but a desktop test ever reads. */
    uint32_t debug_save_attempts = 0;

    /* The last death's cause -- see kf_pet_session.h's own comment on
     * kf_pet_death_cause for why this is read from its own store key
     * rather than living on kf_pet_state. Populated once at init (from
     * whatever is on disk, if anything) and kept current in memory by
     * note_death_if_new() below; never touched by kf_pet_session_frame()'s
     * ordinary decay path unless a death actually happens this call. */
    kf_pet_death_cause death_cause{};

#if KF_PET_SESSION_ENABLE_DEBUG_TOOLS
    /* Debug-only: the timeline snapshot ring, gated on TOOLS specifically
     * (not CONTROLS -- see kf_pet_session.h's "DEBUG ONLY" section).
     * Written by debug_snapshot_push() after every state change; read
     * only by kf_pet_session_debug_seek(). A plain ring buffer, not a
     * growable log -- oldest entries are silently overwritten once full,
     * exactly like any other fixed-capacity structure this project uses
     * (no heap in Core; this file is not Core, but there is no reason to
     * reach for unbounded growth here either). Absent entirely from
     * Session on a backend that has TOOLS off -- see this file's
     * top-of-file comment. */
    DebugSnapshot debug_snapshots[kDebugSnapshotCapacity];
    size_t debug_snapshot_count = 0;
    size_t debug_snapshot_next = 0;
#endif
};
Session g;

#if KF_PET_SESSION_ENABLE_DEBUG_TOOLS

/* Called after every state-changing operation below (a live-tick flush,
 * a debug_advance(), a care action, or a reset) so
 * kf_pet_session_debug_seek() always has a snapshot at or before any age
 * it is asked to reach. Deliberately NOT called from seek() itself --
 * scrubbing is a preview, not a new history event; snapshotting every
 * seek would let a single drag gesture (dozens of seek() calls a
 * second) flood the ring and evict real history for a look that gets
 * thrown away the moment the user drags somewhere else. */
void debug_snapshot_push() {
    DebugSnapshot &slot = g.debug_snapshots[g.debug_snapshot_next];
    slot.state = g.state;
    slot.age_seconds = total_age_seconds(g.state, g.config);
    g.debug_snapshot_next = (g.debug_snapshot_next + 1u) % kDebugSnapshotCapacity;
    if (g.debug_snapshot_count < kDebugSnapshotCapacity) {
        g.debug_snapshot_count++;
    }
}

void debug_snapshot_reset() {
    g.debug_snapshot_count = 0;
    g.debug_snapshot_next = 0;
    debug_snapshot_push();
}

#else // !KF_PET_SESSION_ENABLE_DEBUG_TOOLS

/* No-op stand-ins so every call site below (init/frame/feed/play/rest, all
 * real gameplay code the ESP32 build genuinely runs, PLUS the CONTROLS-
 * gated _debug_advance/_debug_reset further down) stays exactly as
 * written regardless of which backend compiles this file, rather than
 * growing an #if at every call site. Used whenever TOOLS is off,
 * independent of CONTROLS -- a backend with CONTROLS on and TOOLS off
 * (ESP32) still has no ring to push a snapshot into. */
void debug_snapshot_push() {}
void debug_snapshot_reset() {}

#endif // KF_PET_SESSION_ENABLE_DEBUG_TOOLS

/* Death-cause recording. Everything below reads only PUBLIC kf_pet_state/
 * kf_pet_config fields -- is_neglected() itself (hakoniwaos/src/pet.cpp) is
 * file-local to Core and this file has no way to call it, so its five-way
 * predicate is re-derived here by hand, the exact same "cheaper to keep in
 * sync by inspection than a shared export" call sdl_debug_window.cpp's own
 * describe_neglect_driver() already makes for the identical predicate, on
 * the identical reasoning: four lines, not worth widening Core's surface
 * for. If is_neglected() ever changes, both copies have to change with it,
 * by hand -- there is no compiler to catch drift between them. */

/* Version byte for KF_PET_DEATH_CAUSE_KEY's 2-byte record: [version,
 * bitmask]. Bumping this the moment the bitmask's meaning ever changes is
 * what lets an old record be told apart from a new one that happens to be
 * the same size -- load_death_cause() below refuses anything with a
 * mismatched version rather than guessing at what an old bit pattern
 * meant, the same "refuse rather than misread" call kf_pet_load_and_
 * advance() already makes for the pet's own save (kf/pet.h's
 * KF_PET_SAVE_BYTES comment). */
constexpr uint8_t kDeathCauseRecordVersion = 1u;

constexpr uint8_t kDeathBitHunger = 0x01u;
constexpr uint8_t kDeathBitHappiness = 0x02u;
constexpr uint8_t kDeathBitEnergy = 0x04u;
constexpr uint8_t kDeathBitPoopCount = 0x08u;
constexpr uint8_t kDeathBitDirtiness = 0x10u;

uint8_t compute_death_driver_mask(const kf_pet_state &state,
                                   const kf_pet_config &config) {
    uint8_t mask = 0u;
    if (state.hunger_mp <= config.neglect_need_mp) {
        mask |= kDeathBitHunger;
    }
    if (state.happiness_mp <= config.neglect_need_mp) {
        mask |= kDeathBitHappiness;
    }
    if (state.energy_mp <= config.neglect_need_mp) {
        mask |= kDeathBitEnergy;
    }
    if (state.poop_count > config.neglect_poop_count) {
        mask |= kDeathBitPoopCount;
    }
    if (state.dirtiness_mp >= config.neglect_dirtiness_mp) {
        mask |= kDeathBitDirtiness;
    }
    return mask;
}

void death_cause_from_mask(uint8_t mask, kf_pet_death_cause *out) {
    out->known = true;
    out->hunger = (mask & kDeathBitHunger) != 0u;
    out->happiness = (mask & kDeathBitHappiness) != 0u;
    out->energy = (mask & kDeathBitEnergy) != 0u;
    out->poop_count = (mask & kDeathBitPoopCount) != 0u;
    out->dirtiness = (mask & kDeathBitDirtiness) != 0u;
}

/* Reads whatever is under KF_PET_DEATH_CAUSE_KEY, if anything, into
 * g.death_cause. Called once from kf_pet_session_init() -- this file
 * already requires kf_store_init() to be up by then (see this file's own
 * header comment in kf_pet_session.h), the same requirement the pet's own
 * save already has.
 *
 * EVERY failure mode degrades to `known = false` and returns, never
 * asserts, never blocks boot: a fresh save directory (KF_ERR_UNAVAILABLE,
 * the ordinary "never died here yet" case), a record written by a future
 * version this build does not understand, a record whose size does not
 * match what this build expects, or any other backend error. This is the
 * exact contract kf_pet_load_and_advance() already gives the pet's own
 * save for an unreadable value -- fall back, do not fail -- deliberately
 * extended here rather than re-invented, because a death-cause record is
 * even less essential to get right than the pet's own save: losing it
 * costs a blank readout line, not a lost creature. */
void load_death_cause() {
    uint8_t buf[2] = {0u, 0u};
    size_t out_bytes = 0u;
    const kf_result result =
        kf_store_read(KF_PET_DEATH_CAUSE_KEY, buf, sizeof(buf), &out_bytes);
    if (result != KF_OK || out_bytes != sizeof(buf) ||
        buf[0] != kDeathCauseRecordVersion) {
        g.death_cause = kf_pet_death_cause{};
        return;
    }
    death_cause_from_mask(buf[1], &g.death_cause);
}

/* Computes the current driver mask from `g.state`/`g.config` and writes it
 * under KF_PET_DEATH_CAUSE_KEY, updating `g.death_cause` in memory at the
 * same time so a reader never has to round-trip through storage to see a
 * death that just happened. A failed write is logged, not fatal -- see
 * kf_pet_session_save()'s own identical treatment of a failed pet-save
 * write for the reasoning (a full store should be visible in the log, not
 * crash the device), and the in-memory copy is already correct for the
 * rest of THIS run regardless of whether the write lands. */
void record_death_cause() {
    const uint8_t mask = compute_death_driver_mask(g.state, g.config);
    death_cause_from_mask(mask, &g.death_cause);
    const uint8_t buf[2] = {kDeathCauseRecordVersion, mask};
    const kf_result result =
        kf_store_write(KF_PET_DEATH_CAUSE_KEY, buf, sizeof(buf));
    if (result != KF_OK) {
        KF_LOGE(TAG,
                "kf_store_write(%s) failed (%d) -- this run still knows "
                "what killed it, only a FUTURE boot would lose the record",
                KF_PET_DEATH_CAUSE_KEY, static_cast<int>(result));
    }
}

/* Call after any kf_pet_advance() (or kf_pet_load_and_advance()) that could
 * have just killed the pet, passing whether `g.state.dead` was already
 * true going INTO that call. Records only on the alive -> dead EDGE, not
 * on every call against an already-dead pet: kf_pet_advance() is a
 * verified no-op once `dead` is true (hakoniwaos/src/pet.cpp's leading
 * `if (state->dead) return;`), so the state this would compute a mask
 * from never changes again after the real moment of death, and re-writing
 * the identical record on every subsequent flush would be pure waste. */
void note_death_if_new(bool was_dead) {
    if (was_dead || !g.state.dead) {
        return;
    }
    record_death_cause();
}

} // namespace

void kf_pet_session_init(void) {
    KF_ASSERT(!g.ready, "kf_pet_session_init called twice without an "
                        "intervening kf_pet_session_shutdown()");

    g.config = kf_pet_default_config();
    const kf_result result = kf_pet_load_and_advance(&g.state, &g.config);
    KF_ASSERT(result == KF_OK,
              "kf_pet_load_and_advance failed at boot (%d) -- a corrupt or "
              "missing save should fall back to a fresh pet, not fail this "
              "hard; if this fires, something in kf_pet_load_and_advance's "
              "own error handling regressed",
              static_cast<int>(result));

    g.last_call_us = 0;
    g.pending_ms = 0;
    g.last_want = KF_PET_WANT_NONE;

    /* ADR 0056: the state kf_pet_load_and_advance() just produced (above)
     * is very likely NOT what is on disk -- it already fast-forwarded the
     * offline gap in memory, but never wrote that back. Starting dirty is
     * the honest description of that, and means the first periodic
     * checkpoint (below) captures the post-fast-forward baseline rather
     * than only a much later care action or sleep transition doing it --
     * bounding how much a NEXT boot's own fast-forward has to replay. Not
     * urgent for correctness (see that macro's own comment: the on-disk
     * `last_advanced` a boot finds is always a valid, exact baseline to
     * replay forward from, however old it is), just tidy. */
    g.dirty = true;
    g.next_periodic_save_us =
        kf_time_mono_us() +
        static_cast<uint64_t>(KF_PET_SESSION_PERIODIC_SAVE_SECONDS) *
            1000000ull;
    g.debug_save_attempts = 0;

    /* Read whatever death-cause record already exists BEFORE deciding
     * whether this boot needs to write a fresh one -- see the check just
     * below. */
    load_death_cause();

    /* Not note_death_if_new(): that helper wants a "was dead going INTO
     * this call" flag, which does not exist here -- kf_pet_load_and_
     * advance() both loads a save AND fast-forwards it in one call, so
     * there is no separate "before" moment to read `dead` from without
     * re-implementing the load. Instead: if the pet came back dead and
     * there is no record for it yet (`!g.death_cause.known`), record one
     * now. This is the one call site where "no record yet" does not mean
     * "never died" -- it can also mean "died on an EARLIER boot, before
     * this feature existed, or before this key was ever written", and
     * either way the state this pet loaded with is frozen (a dead pet's
     * fields never change again, see note_death_if_new()'s own comment),
     * so computing the mask from it now is exactly as correct as
     * computing it at the real moment of death would have been. Skipped
     * entirely once a record already exists, so a pet that was already
     * dead on a PREVIOUS boot of this same build does not rewrite an
     * identical record on every subsequent boot. */
    if (g.state.dead && !g.death_cause.known) {
        record_death_cause();
    }

    g.ready = true;
    debug_snapshot_reset();
}

void kf_pet_session_frame(uint32_t synthetic_frame_delta_ms) {
    KF_ASSERT(g.ready,
              "kf_pet_session_frame called before kf_pet_session_init");

    uint32_t dt_ms = synthetic_frame_delta_ms;
    if (dt_ms == 0u) {
        const uint64_t now_us = kf_time_mono_us();
        dt_ms = g.last_call_us == 0u
                    ? 0u
                    : static_cast<uint32_t>(
                          (now_us >= g.last_call_us
                               ? now_us - g.last_call_us
                               : 0u) /
                          1000u);
        g.last_call_us = now_us;
    }

    g.pending_ms += dt_ms;

    constexpr uint64_t kFlushThresholdMs =
        static_cast<uint64_t>(KF_PET_SESSION_FLUSH_SECONDS) * 1000ull;
    if (g.pending_ms >= kFlushThresholdMs) {
        const uint32_t whole_seconds =
            static_cast<uint32_t>(g.pending_ms / 1000ull);

        /* Both read BEFORE kf_pet_advance() mutates the state, so each
         * compares against what was true going INTO this flush. */
        const bool was_asleep = g.state.asleep;
        const bool was_dead = g.state.dead;

        kf_pet_advance(&g.state, &g.config, whole_seconds);
        g.pending_ms = g.pending_ms % 1000ull;
        debug_snapshot_push();
        note_death_if_new(was_dead);

        /* ADR 0056's dirty flag. `!was_dead`, not `!g.state.dead` --
         * kf_pet_advance() itself is a verified no-op for an
         * already-dead pet (hakoniwaos/src/pet.cpp's leading
         * `if (state->dead) return;`), so a flush starting from dead
         * changes nothing this call and has nothing new to persist. This
         * is the actual "idle pet" case the periodic checkpoint below
         * gets to skip. An egg, by contrast, IS marked dirty here (its
         * needs do not decay, but stage_elapsed_seconds still advances
         * every flush) -- harmless, since its periodic checkpoint is
         * still bounded by the same dirty-gated interval, not every
         * flush. */
        if (!was_dead) {
            g.dirty = true;
        }

        if (!was_asleep && g.state.asleep) {
            /* Checkpoint 2 of 3 (kf_pet_session_save()'s own comment):
             * the awake -> asleep transition, live -- Chris's "low power
             * mode". Unconditional on the dirty flag: falling asleep is
             * itself the state change worth saving, whatever the flag
             * already said. */
            kf_pet_session_save();
        } else if (g.dirty && kf_time_mono_us() >= g.next_periodic_save_us) {
            /* Checkpoint 3 of 3: the periodic safety net, gated on both
             * "due" and "dirty" -- see KF_PET_SESSION_PERIODIC_SAVE_
             * SECONDS's own comment (kf_pet_session.h) for the endurance
             * arithmetic this interval is sized against. */
            kf_pet_session_save();
        }
    }
}

const kf_pet_state *kf_pet_session_state(void) {
    KF_ASSERT(g.ready,
              "kf_pet_session_state called before kf_pet_session_init");
    return &g.state;
}

const kf_pet_config *kf_pet_session_config(void) {
    KF_ASSERT(g.ready,
              "kf_pet_session_config called before kf_pet_session_init");
    return &g.config;
}

namespace {

/* Same clamp-at-the-ceiling shape as pet.cpp's own file-local clamp_add()
 * (hakoniwaos/src/pet.cpp) -- duplicated rather than exported for the
 * identical "four lines, not worth widening a header's surface for"
 * reasoning kf/game.h's own put_u16/get_u16 helpers already accept. This
 * file is not Core (see its own top-of-file comment), so it cannot reach
 * pet.cpp's file-local helper regardless. */
kf_pet_millipercent clamp_need_up(kf_pet_millipercent value,
                                   kf_pet_millipercent add) {
    const uint64_t sum = static_cast<uint64_t>(value) + add;
    return sum > KF_PET_MILLIPERCENT_MAX
               ? KF_PET_MILLIPERCENT_MAX
               : static_cast<kf_pet_millipercent>(sum);
}

kf_pet_millipercent clamp_need_down(kf_pet_millipercent value,
                                     kf_pet_millipercent subtract) {
    return subtract >= value ? 0u : value - subtract;
}

/* Shared by kf_pet_session_reward_need()/_spend_need() below: which raw
 * field on g.state a kf_pet_need selects. Returns nullptr for
 * KF_PET_NEED_NONE -- both callers already guard on that before reaching
 * here, but a defensive nullptr (rather than, say, falling back to
 * hunger_mp) means a future call site that forgets the guard gets a crash
 * to notice, not a wrong need silently adjusted. */
kf_pet_millipercent *need_field(kf_pet_need need) {
    switch (need) {
    case KF_PET_NEED_HUNGER:
        return &g.state.hunger_mp;
    case KF_PET_NEED_HAPPINESS:
        return &g.state.happiness_mp;
    case KF_PET_NEED_ENERGY:
        return &g.state.energy_mp;
    case KF_PET_NEED_NONE:
    default:
        return nullptr;
    }
}

} // namespace

void kf_pet_session_reward_need(kf_pet_need need, kf_pet_millipercent amount_mp) {
    KF_ASSERT(g.ready,
              "kf_pet_session_reward_need called before kf_pet_session_init");
    kf_pet_millipercent *field = need_field(need);
    const bool applies = !g.state.dead && field != nullptr && amount_mp != 0u;
    if (applies) {
        *field = clamp_need_up(*field, amount_mp);
    }
    debug_snapshot_push();
    if (applies) {
        kf_pet_session_save();
    }
}

void kf_pet_session_spend_need(kf_pet_need need, kf_pet_millipercent amount_mp) {
    KF_ASSERT(g.ready,
              "kf_pet_session_spend_need called before kf_pet_session_init");
    kf_pet_millipercent *field = need_field(need);
    const bool applies = !g.state.dead && field != nullptr && amount_mp != 0u;
    if (applies) {
        *field = clamp_need_down(*field, amount_mp);
    }
    debug_snapshot_push();
    if (applies) {
        kf_pet_session_save();
    }
}

/* Every care action below shares a shape: mutate, push a debug snapshot,
 * then kf_pet_session_save() -- checkpoint 1 of 3 (see that function's own
 * comment). These are rare, human-paced events (a button press), never a
 * per-frame path, so an unconditional immediate save would cost nothing
 * worth gating behind the dirty flag the periodic checkpoint uses -- see
 * docs/architecture/adr-0056-pet-save-checkpoints.md.
 *
 * "Unconditional" is not quite right, though: every one of kf/pet.h's care
 * functions is a documented no-op under some condition of its own (dead,
 * for feed/play/rest/bath/flush; dead-or-not-asleep for wake; not-drowsy
 * for tuck_in -- see each Core function's own header comment in kf/pet.h).
 * A pet screen that still lets a shrine be tapped (or a test loop that
 * calls one of these against a dead/inapplicable pet on purpose) would
 * otherwise burn a real NVS write for a press that changed nothing at
 * all. Each function below reads the SAME condition Core's own early
 * return checks -- not a guess at one -- before deciding whether the
 * mutation could possibly have done anything, and only saves if so. */

void kf_pet_session_feed(uint8_t variation) {
    KF_ASSERT(g.ready,
              "kf_pet_session_feed called before kf_pet_session_init");
    const bool applies = !g.state.dead;
    kf_pet_feed(&g.state, &g.config, variation);
    debug_snapshot_push();
    if (applies) {
        kf_pet_session_save();
    }
}

void kf_pet_session_play(uint8_t variation) {
    KF_ASSERT(g.ready,
              "kf_pet_session_play called before kf_pet_session_init");
    const bool applies = !g.state.dead;
    kf_pet_play(&g.state, &g.config, variation);
    debug_snapshot_push();
    if (applies) {
        kf_pet_session_save();
    }
}

void kf_pet_session_rest(uint8_t variation) {
    KF_ASSERT(g.ready,
              "kf_pet_session_rest called before kf_pet_session_init");
    const bool applies = !g.state.dead;
    kf_pet_rest(&g.state, &g.config, variation);
    debug_snapshot_push();
    if (applies) {
        kf_pet_session_save();
    }
}

void kf_pet_session_bath(uint8_t variation) {
    KF_ASSERT(g.ready,
              "kf_pet_session_bath called before kf_pet_session_init");
    const bool applies = !g.state.dead;
    kf_pet_bath(&g.state, &g.config, variation);
    debug_snapshot_push();
    if (applies) {
        kf_pet_session_save();
    }
}

void kf_pet_session_flush(void) {
    KF_ASSERT(g.ready,
              "kf_pet_session_flush called before kf_pet_session_init");
    const bool applies = !g.state.dead;
    kf_pet_flush(&g.state);
    debug_snapshot_push();
    if (applies) {
        kf_pet_session_save();
    }
}

void kf_pet_session_wake(void) {
    KF_ASSERT(g.ready,
              "kf_pet_session_wake called before kf_pet_session_init");
    const bool applies = !g.state.dead && g.state.asleep;
    kf_pet_wake(&g.state, &g.config);
    debug_snapshot_push();
    if (applies) {
        kf_pet_session_save();
    }
}

void kf_pet_session_tuck_in(void) {
    KF_ASSERT(g.ready,
              "kf_pet_session_tuck_in called before kf_pet_session_init");
    const bool applies = kf_pet_drowsy(&g.state);
    kf_pet_tuck_in(&g.state);
    debug_snapshot_push();
    if (applies) {
        kf_pet_session_save();
    }
}

kf_pet_want kf_pet_session_wants(void) {
    KF_ASSERT(g.ready,
              "kf_pet_session_wants called before kf_pet_session_init");
    g.last_want = kf_pet_wants(&g.state, g.last_want);
    return g.last_want;
}

const kf_pet_death_cause *kf_pet_session_last_death_cause(void) {
    KF_ASSERT(g.ready, "kf_pet_session_last_death_cause called before "
                        "kf_pet_session_init");
    return &g.death_cause;
}

void kf_pet_session_save(void) {
    KF_ASSERT(g.ready,
              "kf_pet_session_save called before kf_pet_session_init");
    g.debug_save_attempts++;
    const kf_result result = kf_pet_save(&g.state);
    if (result != KF_OK) {
        KF_LOGE(TAG,
                "kf_pet_save failed (%d) -- will try again at the next "
                "checkpoint",
                static_cast<int>(result));
        /* Dirty stays true, and the periodic deadline is deliberately
         * NOT pushed forward -- see ADR 0056: a failed write (e.g.
         * KF_ERR_EXHAUSTED, a full NVS partition) should be retried at
         * the very next checkpoint of ANY kind, not stall until a full
         * KF_PET_SESSION_PERIODIC_SAVE_SECONDS has passed again. */
        return;
    }
    g.dirty = false;
    g.next_periodic_save_us =
        kf_time_mono_us() +
        static_cast<uint64_t>(KF_PET_SESSION_PERIODIC_SAVE_SECONDS) *
            1000000ull;
}

void kf_pet_session_shutdown(void) {
    if (!g.ready) {
        return;
    }
    kf_pet_session_save();
    g.ready = false;
}

/* The cheap quartet -- gated on CONTROLS, not TOOLS, per this file's
 * top-of-file comment. Each calls debug_snapshot_push()/_reset(), defined
 * above under the TOOLS #if/#else pair: the real ring-writer when TOOLS
 * is on, a no-op when it is off (e.g. ESP32: CONTROLS on, TOOLS off) --
 * either way these four don't need to know which. */
#if KF_PET_SESSION_ENABLE_DEBUG_CONTROLS

void kf_pet_session_debug_advance(uint32_t seconds) {
    KF_ASSERT(g.ready, "kf_pet_session_debug_advance called before "
                        "kf_pet_session_init");

    /* The wall clock moves by the SAME amount, and the order matters: the
     * clock is set BEFORE the advance, because kf_pet_advance() evaluates
     * the night window against last_advanced and would otherwise be
     * reasoning about a different day than the one Lua is displaying.
     *
     * Until 2026-08-11 this function moved only Core's clock. That was a
     * deliberate choice when nothing read the hour -- and it quietly became
     * wrong the moment sleep landed, because Core's idea of the time and
     * kf_time_wall() (what Lua's kf.hour(), the wall clock on the home
     * screen, and the drowsy window all read) then disagreed by however
     * much had been skipped. Skip 1 Week and then wonder why the creature
     * will not go to bed. Chris, 2026-08-11: "the skip 1 hour, skip 1 day,
     * skip 1 week needs to also skip time on the real clock too".
     *
     * Fixed HERE rather than in sdl_debug_window.cpp on purpose: the three
     * desktop buttons are not the only caller. ports/esp32/main/
     * kf_dbg_bridge.cpp's KFDBG ADVANCE calls this too, so the device gets
     * the same coherent behaviour from the same change.
     *
     * SIDE EFFECT WORTH KNOWING ON DEVICE: kf_time_set_wall() writes
     * through to the DS3231 (ports/esp32/hal/esp_time.cpp), so a KFDBG
     * ADVANCE now genuinely moves the board's battery-backed clock, and it
     * stays moved across a power cut. That is the honest meaning of "skip
     * a week" and it is debug-gated, but it does mean the bench clock needs
     * resetting afterwards -- it is not a display-only fast-forward. */
    const kf_wall_time before = kf_time_wall();
    if (before.valid) {
        kf_time_set_wall(before.epoch_seconds + static_cast<int64_t>(seconds));
    }

    const bool was_dead = g.state.dead;
    kf_pet_advance(&g.state, &g.config, seconds);
    debug_snapshot_push();
    note_death_if_new(was_dead);
}

int64_t kf_pet_session_debug_clock_target(kf_pet_debug_clock_point point) {
    /* Five seconds INSIDE each state, not short of it -- see this
     * function's header comment in kf_pet_session.h for why aiming short
     * produces a button that appears to do nothing. The hours themselves
     * mirror kNightStartHour/kNightEndHour (hakoniwaos/src/pet.cpp); they
     * are duplicated here rather than exported because Core deliberately
     * keeps them private, and a debug affordance is not reason enough to
     * widen that surface. If the night window ever moves, clock_jump_check
     * fails loudly, which is the intended tripwire. Drowsy's own minute
     * (ADR 0052, the 2026-08-11 bedtime-behaviour extension) is the
     * identical duplication for kDrowsyWindowSeconds -- landing at :50
     * puts this five seconds inside the new ten-minute window rather than
     * the old whole hour. */
    int hour = 22;
    int minute = 0;
    switch (point) {
    case KF_PET_DEBUG_CLOCK_DROWSY:
        hour = 21;
        minute = 50;
        break;
    case KF_PET_DEBUG_CLOCK_MORNING:
        hour = 7;
        break;
    case KF_PET_DEBUG_CLOCK_BEDTIME:
    default:
        hour = 22;
        break;
    }

    const kf_wall_time now = kf_time_wall();
    kf_civil civil;
    kf_civil_from_epoch(now.epoch_seconds, &civil);
    civil.hour = static_cast<uint8_t>(hour);
    civil.minute = static_cast<uint8_t>(minute);
    civil.second = 5u;

    int64_t target = kf_epoch_from_civil(&civil);
    if (target <= now.epoch_seconds) {
        target += 24 * 60 * 60;
    }
    return target;
}

void kf_pet_session_debug_set_clock(int64_t epoch_seconds) {
    KF_ASSERT(g.ready, "kf_pet_session_debug_set_clock called before "
                        "kf_pet_session_init");

    /* Both clocks, together -- see this function's header comment in
     * kf_pet_session.h for why moving only one of them produces a creature
     * that looks broken but is behaving correctly against a clock nobody
     * updated. */
    kf_time_set_wall(epoch_seconds);
    g.state.last_advanced.valid = true;
    g.state.last_advanced.epoch_seconds = epoch_seconds;

    /* `asleep` is only recomputed inside an advance (apply_stage_segment(),
     * pet.cpp), so without this the creature would not notice the new hour
     * until the next KF_PET_SESSION_FLUSH_SECONDS boundary. One second is
     * the smallest advance that re-evaluates it, and is deliberately the
     * only ageing this function does -- see the header comment on why
     * travelling the real distance is the wrong behaviour here. */
    const bool was_dead = g.state.dead;
    kf_pet_advance(&g.state, &g.config, 1u);
    debug_snapshot_push();
    note_death_if_new(was_dead);
}

void kf_pet_session_debug_reset(void) {
    KF_ASSERT(g.ready,
              "kf_pet_session_debug_reset called before kf_pet_session_init");
    kf_pet_init(&g.state);
    g.pending_ms = 0;
    g.last_want = KF_PET_WANT_NONE;
    debug_snapshot_reset();
}

void kf_pet_session_debug_jump_to_stage(kf_pet_stage stage, uint8_t teen_form,
                                         uint8_t adult_branch) {
    KF_ASSERT(g.ready, "kf_pet_session_debug_jump_to_stage called before "
                        "kf_pet_session_init");

    /* A save that survived version-checking could in principle carry a
     * stage byte past ADULT (unpack() already refuses that -- see kf/
     * pet.cpp -- but this function does not go through unpack(), so it
     * gets its own defensive clamp rather than trusting the caller). */
    const kf_pet_stage target =
        stage > KF_PET_STAGE_ADULT ? KF_PET_STAGE_ADULT : stage;

    /* kf_pet_init() already gives requirement 1 for free -- every need at
     * KF_PET_MILLIPERCENT_MAX, not sick, not dead, care_integral_mp_seconds
     * and every other accumulator zeroed -- so this is the exact same
     * starting point kf_pet_session_debug_reset() uses, just with `stage`
     * set directly afterward instead of always leaving it at EGG. See this
     * function's header comment in kf_pet_session.h for why that is
     * deliberate: walking there via kf_pet_advance() would let Core pick
     * teen_form/adult_branch from a care history that was never real. */
    kf_pet_init(&g.state);
    g.state.stage = target;

    /* Meaningless before their own branch point (kf/pet.h's own words), so
     * only written once `target` has passed it -- kf_pet_init()'s 0 is
     * already the right answer otherwise. Out-of-range input (not just
     * unset) falls back to 0 rather than being written raw -- see the
     * header comment on why 0 is always a safe fallback for both.
     *
     * The valid range here is [0, KF_PET_TEEN_FORM_DUST], inclusive of the
     * dust form -- NOT "< KF_PET_TEEN_FORM_COUNT", which would silently
     * clamp KF_PET_TEEN_FORM_DUST itself (== KF_PET_TEEN_FORM_COUNT, by
     * kf/pet.h's own deliberate construction, see that macro's comment) to
     * 0. Dust is not an error value the way anything past it is -- it is a
     * real teen_form a genuinely-neglected creature can and does reach
     * (advance_to_next_stage(), hakoniwaos/src/pet.cpp), so a lever whose
     * entire purpose is making every form inspectable has no business
     * treating it as out of range. */
    if (target >= KF_PET_STAGE_TEEN) {
        g.state.teen_form =
            teen_form <= KF_PET_TEEN_FORM_DUST ? teen_form : 0u;
    }
    if (target == KF_PET_STAGE_ADULT) {
        const uint8_t adults_in_family =
            kf_pet_adults_in_family(g.state.teen_form);
        g.state.adult_branch =
            adult_branch < adults_in_family ? adult_branch : 0u;
    }

    g.pending_ms = 0;
    g.last_want = KF_PET_WANT_NONE;
    /* Same call kf_pet_session_debug_reset() makes above, for the identical
     * reason: a jump is a fabricated state with no continuity from whatever
     * the pet was doing a moment before, not a new point on the same
     * timeline -- see kf_pet_session_debug_seek()'s own header comment on
     * what happens when a ring mixes snapshots from two unrelated
     * timelines in the same age range. Starting a fresh ring here avoids
     * that entirely rather than risking it. */
    debug_snapshot_reset();

    /* Checkpoint immediately, for the same reason ADR 0056 made the care
     * actions checkpoint: the pet you just changed should still be there
     * after a power cut.
     *
     * This used to rely on the dirty-gated periodic save alone, which is a
     * TEN MINUTE interval -- so jumping to a stage to set up a test and then
     * pulling the plug inside that window silently gave the old pet back.
     * Found on hardware: a KFDBG NEXTSTAGE to Teen, a power cycle a few
     * minutes later for the coin-cell test, and the creature came back a
     * Child at its pre-jump age. Nothing was broken; the jump had simply
     * never been written, which is worse than broken because it looks like
     * the jump silently failed.
     *
     * Applies to both surfaces at once now that they share one table
     * (ADR 0060): the desktop window's stage buttons and KFDBG JUMP /
     * NEXTSTAGE all land here. */
    kf_pet_session_save();
}

uint64_t kf_pet_session_debug_age_seconds(void) {
    KF_ASSERT(g.ready, "kf_pet_session_debug_age_seconds called before "
                        "kf_pet_session_init");
    return total_age_seconds(g.state, g.config);
}

#endif // KF_PET_SESSION_ENABLE_DEBUG_CONTROLS

/* The expensive one -- gated on TOOLS specifically, independent of
 * CONTROLS above. See kf_pet_session.h's "DEBUG ONLY" header comment. */
#if KF_PET_SESSION_ENABLE_DEBUG_TOOLS

void kf_pet_session_debug_seek(uint64_t target_age_seconds) {
    KF_ASSERT(g.ready,
              "kf_pet_session_debug_seek called before kf_pet_session_init");
    if (g.debug_snapshot_count == 0u) {
        /* Can't happen in practice -- kf_pet_session_init() always seeds
         * one snapshot -- but a seek with nothing to seek from is a no-op,
         * not a crash. */
        return;
    }

    /* Find the snapshot with the largest age at or before the target; if
     * the target is earlier than every surviving snapshot (evicted, or
     * before this pet's own genesis), fall back to the EARLIEST surviving
     * one instead -- see kf_pet_session.h's header comment on this
     * clamping behaviour. One linear scan of at most kDebugSnapshotCapacity
     * plain-old-data entries, called at most once per frame even while
     * actively dragging the timeline -- not worth a sorted structure. */
    const DebugSnapshot *best_at_or_before = nullptr;
    const DebugSnapshot *earliest = nullptr;
    for (size_t i = 0; i < g.debug_snapshot_count; ++i) {
        const DebugSnapshot &snap = g.debug_snapshots[i];
        if (snap.age_seconds <= target_age_seconds &&
            (best_at_or_before == nullptr ||
             snap.age_seconds > best_at_or_before->age_seconds)) {
            best_at_or_before = &snap;
        }
        if (earliest == nullptr || snap.age_seconds < earliest->age_seconds) {
            earliest = &snap;
        }
    }
    const DebugSnapshot *base = best_at_or_before != nullptr ? best_at_or_before : earliest;

    kf_pet_state replay = base->state;
    if (target_age_seconds > base->age_seconds) {
        const uint64_t delta = target_age_seconds - base->age_seconds;
        const uint32_t delta_seconds =
            delta > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(delta);
        kf_pet_advance(&replay, &g.config, delta_seconds);
    }
    g.state = replay;

    /* A backward or forward jump makes whatever fractional live-tick
     * remainder was pending before this seek meaningless -- it was
     * measured against the pre-seek position. Drop it rather than let it
     * apply a small (< KF_PET_SESSION_FLUSH_SECONDS) phantom nudge on top
     * of wherever the seek just landed, the instant normal play resumes. */
    g.pending_ms = 0;
}

kf_pet_state *kf_pet_session_state_mutable_for_test(void) {
    KF_ASSERT(g.ready, "kf_pet_session_state_mutable_for_test called before "
                        "kf_pet_session_init");
    return &g.state;
}

void kf_pet_session_debug_force_periodic_save_due(void) {
    KF_ASSERT(g.ready, "kf_pet_session_debug_force_periodic_save_due called "
                        "before kf_pet_session_init");
    /* kf_time_mono_us(), not 0 -- "due now" has to mean "not later than the
     * very next comparison against it" regardless of what the mono clock
     * happens to read at the time (0 the whole run, in a headless check
     * that never calls kf_app_init()/kf_time_init() -- see kf/hal/time.h
     * and host_time.cpp's kf_time_mono_us()). 0 would still work today,
     * but reading the real clock is the version that stays correct if a
     * future caller of this ever runs with kf_app_init() up too. */
    g.next_periodic_save_us = kf_time_mono_us();
}

uint32_t kf_pet_session_debug_save_attempt_count(void) {
    KF_ASSERT(g.ready, "kf_pet_session_debug_save_attempt_count called "
                        "before kf_pet_session_init");
    return g.debug_save_attempts;
}

#endif // KF_PET_SESSION_ENABLE_DEBUG_TOOLS
