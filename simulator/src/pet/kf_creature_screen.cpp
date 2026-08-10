/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors. */

#include "kf_creature_screen.h"

#include "kf_pet_session.h"

#include "kf/app.h"
#include "kf/assets.h"
#include "kf/blit.h"
#include "kf/budget.h"
#include "kf/creature.h"
#include "kf/font.h"
#include "kf/hal/log.h"
#include "kf/pet.h"

#include <cstring>

namespace {

constexpr const char *TAG = "creature-screen";

/* The bottom 60 rows are the reserved stats band (Task 9 of the hardware
 * bring-up plan, docs/superpowers/plans/2026-08-11-hardware-bringup.md --
 * see kf_screen_nav.cpp's own comment on kf_pet_screen.cpp staying
 * unreachable rather than deleted for how this band came to be empty in the
 * first place) -- the creature/mess/shrine drawing this file does every
 * frame only ever touches y=[0,260): kField is where the creature walks,
 * gets erased, and gets redrawn every frame, and nothing below y=260 ever
 * gets sprite, placeholder or mess content from this file. That is a
 * different claim from "this file never touches those rows at all", and as
 * of Task 9 it is no longer even "only kf_creature_screen_enter() touches
 * that band": kScreen and kf_creature_screen_enter()'s own comment cover the
 * full-panel wipe and the ONE-TIME paint of the care-button guide
 * (draw_care_guide()) and the three stat bars' fixed labels
 * (draw_stat_labels()) into this band, but the stat bars' own FILL portion
 * (update_stat_bars(), below) is also called every frame from kf_creature_
 * screen_frame() -- deliberately, and safely, because it redraws a bar's
 * rect only on the rare frame its quantised width actually changed (see
 * update_stat_bar()'s own comment), not every frame the way the guide/label
 * text never does. So: the guide and the labels stay exactly the
 * "painted once, on entry, never again" fixtures this paragraph used to
 * describe the WHOLE band as; the bars are the one deliberate exception,
 * and they are change-gated specifically so they cost nothing against the
 * per-frame dirty-rect budget while steady. */
constexpr kf_rect kField = {0, 0, 240, 260};

/* The full display, used only by kf_creature_screen_enter()'s entry repaint
 * -- see that function's own comment for why entry needs the whole panel
 * and per-frame drawing above only ever needs kField. */
constexpr kf_rect kScreen = {0, 0, KF_DISPLAY_WIDTH, KF_DISPLAY_HEIGHT};

constexpr kf_color kBackground = KF_RGB(232, 240, 216);

/* Stands in for a creature sprite the asset pack does not have -- this claim
 * now has to be split by target, where it did not used to need to be.
 *
 * On DESKTOP, the checked-in default pack (examples/hello_sprite/
 * assets.kfpack, simulator/CMakeLists.txt's KF_ASSET_PACK) still carries
 * exactly one sprite, "test_sprite", so every kf_creature_sprite_name()
 * lookup this screen makes still returns nullptr from kf_assets_get() there,
 * on every stage/pose/direction, unless a developer overrides KF_ASSET_PACK
 * to point at examples/creature_demo/assets.kfpack (see that CMakeLists.txt
 * variable's own comment on the incantation for that).
 *
 * On the DEVICE, that is no longer true: ports/esp32/main/CMakeLists.txt
 * flashes examples/creature_demo/assets.kfpack instead of hello_sprite's --
 * 94 entries, real art, animated idles -- so on real hardware these lookups
 * now succeed for every stage this screen draws, and this placeholder is
 * not expected to appear there at all. A magenta box on a real panel is a
 * fault to chase, not the "on both desktop and the device" expected state
 * this comment used to describe before that pack switched.
 *
 * Drawing nothing in the still-missing case (desktop's default pack, or any
 * future pack still short an entry) would make the screen look BROKEN
 * rather than unfinished, and would make the dirty-rect budget test below
 * pass vacuously (no draw, no dirty rectangle, nothing to measure) -- see
 * run_creature_screen_check()'s own comment. So: visible and obviously
 * wrong beats invisible and indistinguishable from a rendering bug, the
 * same reasoning kf_creature_sprite_name() already applies to the DEAD pose
 * falling back to the sick sprite (kf/creature.h). Hot pink, deliberately
 * nothing any finished creature art would plausibly ship in -- it reads as
 * "placeholder" on sight, the same way a missing-texture checkerboard does
 * in other engines. Already gone on the device, as of the pack switch
 * above; on desktop it disappears only once a developer points
 * KF_ASSET_PACK at real creature art too -- nothing else about this file
 * changes either time. */
constexpr kf_color kPlaceholderColor = KF_RGB(255, 0, 128);

/* Mess (Task 5). pet->poop_count (kf/pet.h) is a COUNT, 0..KF_PET_MAX_POOPS,
 * with no positions -- Core says so deliberately, because where each one
 * sits on screen is presentation's call, not the simulation's. This is that
 * call: KF_PET_MAX_POOPS fixed slots in a single row, addressed by index
 * alone -- deterministic, so the same count always paints the same pixels,
 * no jitter frame to frame and no RNG (Controller amendment A6: placing
 * mess with the RNG would make a bug report un-reproducible).
 *
 * The row sits inside kField (A3): kPoopSlotWidth * KF_PET_MAX_POOPS ==
 * kField.x1, so all eight slots fit the 240px width with no overlap and
 * none spill outside it, and the row itself (kPoopY0..kPoopY1) finishes
 * well short of y=260, the reserved stats band this file never draws into
 * otherwise -- with a further gap below the row and the band edge, so mess
 * reads as sitting on the floor rather than colliding with the band. */
constexpr int16_t kPoopSize = 12;
constexpr int16_t kPoopY0 = 232;
constexpr int16_t kPoopY1 = kPoopY0 + kPoopSize;
constexpr int16_t kPoopSlotWidth =
    static_cast<int16_t>(kField.x1 / static_cast<int16_t>(KF_PET_MAX_POOPS));

/* Makes the block comment above's "kPoopSlotWidth * KF_PET_MAX_POOPS ==
 * kField.x1" claim true by construction rather than merely asserted in
 * prose: kPoopSlotWidth is computed by integer division just above, which
 * silently floors on a remainder, so a KF_PET_MAX_POOPS that does not
 * evenly divide kField.x1 would otherwise leave the row short of the
 * field's full width -- still safe (never overlapping or spilling), just
 * quietly not what the comment says. This fails the build instead. */
static_assert(kPoopSlotWidth * static_cast<int16_t>(KF_PET_MAX_POOPS) ==
                  kField.x1,
              "kPoopSlotWidth * KF_PET_MAX_POOPS must equal kField.x1 -- "
              "see the block comment above kPoopSize for why");

/* The whole mess row, used only when the poop count actually changes -- see
 * kf_creature_screen_frame()'s mess-drawing comment for why repainting
 * through this one rect, rather than each poop's own rect standing alone,
 * is what keeps that the ONE frame it happens on cheap. */
constexpr kf_rect kMessBand = {0, kPoopY0, kField.x1, kPoopY1};

/* A plain brown, unmistakably neither the field background nor
 * kPlaceholderColor's hot pink -- nothing subtle is being attempted here,
 * the same "obviously placeholder" reasoning kPlaceholderColor's own
 * comment gives, until real mess art exists. */
constexpr kf_color kPoopColor = KF_RGB(92, 64, 51);

/* Where poop number `index` (0-based, < pet->poop_count) sits. Pure
 * function of the index alone -- see the block comment above kPoopSize for
 * why that matters (Controller amendment A6: no RNG). */
kf_rect poop_rect(uint8_t index) {
    const int16_t x0 = static_cast<int16_t>(
        index * kPoopSlotWidth + (kPoopSlotWidth - kPoopSize) / 2);
    return kf_rect{x0, kPoopY0, static_cast<int16_t>(x0 + kPoopSize), kPoopY1};
}

/* The egg bob: "bob in one place like it's wobbling/squishing every so
 * often" (the project owner's own words), the buildable half of that ask.
 * Moving up and down a few pixels needs no new art and no frame
 * sequencing -- it is pure positioning, an offset applied to where the
 * egg's sprite is drawn, nothing more. The other half -- the egg actually
 * SQUISHING, i.e. deforming -- needed different artwork per frame, which
 * this bob deliberately never provided (positioning a sprite cannot deform
 * it) and which the animated-indexed-sprites plan's Task 6 built the CODE
 * side for without waiting on the art: it wired resolve_sprite()'s draw
 * path below through kf_creature_anim_wrap()/kf_blit_frame(), so any
 * egg_idle_<dir> entry that shipped several squish frames would play them,
 * in step with this same bob, with no further code change -- see
 * kf_creature_tick_anim()'s call in the egg branch just below, and
 * kf/creature.h's kf_creature_sprite_name() comment for the entry-name
 * side of that split.
 *
 * That art now exists (tools/character_manifest.toml's [stages.egg],
 * `frames = 9`; see .superpowers/sdd/first-animations-report.md for the
 * generation record) -- a subtle squash-and-stretch settle, generated to
 * deform in place with no net travel of its own, specifically so it
 * layers under this positional bob rather than fighting it: the ART
 * squishes the egg's shape frame to frame, the CODE offset below still
 * moves the whole drawn result up and down, and the two compose for free
 * because kf_blit_frame() draws whichever frame the animation clock
 * currently points at wherever this function says to draw it. Retiring
 * the bob in favour of the art alone was the other option the plan named;
 * this is the "coexist" branch of that choice, an art/product call, not a
 * code one. Not faked with a scale transform either -- see the
 * animated-indexed-sprites plan's Task 6 report for why stretching a
 * static sprite would read as broken art, not squish.
 *
 * Integer triangle wave, not a lookup table or a sine call: hakoniwaos/
 * stays free of floating point AND of trig (see tools/check_no_heap.py and
 * this file's own comment on why it ships unchanged to the device,
 * kf_creature_screen.h's header comment), and a triangle wave is exact
 * integer arithmetic that needs neither. kEggBobPeriodMs is how long one
 * full up-and-back-down-and-back cycle takes (see egg_bob_offset_y()'s own
 * comment for the exact shape); kEggBobAmplitudePx is how far off centre
 * it swings at the extremes -- both small and slow on purpose, an idle
 * wobble, not a bounce, per the owner's own "every so often" framing.
 *
 * Deliberately NOT applied inside kf_creature_update() (hakoniwaos/src/
 * creature.cpp): that function only knows how to move an (x,y) toward a
 * target, and the egg's bob is not movement at all, it is a per-frame draw
 * offset applied on top of a position that (per the wander gate above)
 * never actually changes while the pet is an egg. Keeping it here, next to
 * where it's applied, means Core never needs to know the egg wobbles. */
constexpr uint32_t kEggBobPeriodMs = 3000u;
constexpr int16_t kEggBobAmplitudePx = 2;

/* kEggBobPeriodMs divided into quarters, for egg_bob_offset_y() below.
 * static_assert rather than a comment's promise -- see kPoopSlotWidth's own
 * pattern -- because the wave's zero-at-rest guarantee (this function's own
 * comment) depends on the period dividing evenly by 4 with nothing left
 * over. */
constexpr uint32_t kEggBobQuarterMs = kEggBobPeriodMs / 4u;
static_assert(kEggBobQuarterMs * 4u == kEggBobPeriodMs,
              "kEggBobPeriodMs must be a multiple of 4 -- see the block "
              "comment above egg_bob_offset_y() for why");

/* The offset (in whole pixels, positive means drawn lower on screen) for
 * an egg that has been idle for `elapsed_ms`. A plain integer triangle
 * wave, zero-phase-aligned: unlike the more obvious "0 up to amplitude and
 * back" shape, THIS one is exactly 0 at elapsed_ms == 0 and at every whole
 * multiple of kEggBobPeriodMs after that -- rising to +kEggBobAmplitudePx
 * a quarter-period in, back through 0 at the half-period mark, down to
 * -kEggBobAmplitudePx at three-quarters, and back to 0 to close the cycle.
 * That zero-at-rest property matters here, not just aesthetically: a fresh
 * egg (g_egg_bob_elapsed_ms starts at 0u) draws at offset 0 on its very
 * first frame, exactly where kf_creature_bounds() already says it is --
 * see kf_creature_screen_debug_bounds()'s own comment on why that debug
 * accessor deliberately never includes this offset, which only holds
 * together if frame zero's offset is itself zero. It is also what keeps
 * run_creature_screen_sprite_check() (headless_main.cpp) correct without
 * that check needing to know the bob exists at all: every frame it drives
 * passes dt_ms == 0, so g_egg_bob_elapsed_ms never leaves 0 either, and
 * this function returns exactly 0 for the whole check -- a "0 up to
 * amplitude" shape would instead have started that check's sampling
 * window already 2px off from where the sprite was actually drawn.
 *
 * The per-frame CHANGE in this value is what the dirty-rect budget cares
 * about (see kf_creature_screen_frame()'s own comment where this is
 * applied): each quarter-segment is linear, so consecutive frames a normal
 * 33ms tick apart move by at most 1px, nowhere near enough to stop the
 * erase rect and the draw rect overlapping and merging into one
 * (kf/framebuffer.h's kf_fb_mark_dirty() comment) -- the same way the
 * wander's own movement already relies on. */
int16_t egg_bob_offset_y(uint32_t elapsed_ms) {
    const uint32_t phase = elapsed_ms % kEggBobPeriodMs;
    const uint32_t a = static_cast<uint32_t>(kEggBobAmplitudePx);
    int32_t offset;
    if (phase < kEggBobQuarterMs) {
        /* 0 -> +amplitude */
        offset = static_cast<int32_t>((phase * a) / kEggBobQuarterMs);
    } else if (phase < 2u * kEggBobQuarterMs) {
        /* +amplitude -> 0 */
        offset = static_cast<int32_t>(
            ((2u * kEggBobQuarterMs - phase) * a) / kEggBobQuarterMs);
    } else if (phase < 3u * kEggBobQuarterMs) {
        /* 0 -> -amplitude */
        offset = -static_cast<int32_t>(
            ((phase - 2u * kEggBobQuarterMs) * a) / kEggBobQuarterMs);
    } else {
        /* -amplitude -> 0 */
        offset = -static_cast<int32_t>(
            ((4u * kEggBobQuarterMs - phase) * a) / kEggBobQuarterMs);
    }
    return static_cast<int16_t>(offset);
}

/* How long the current egg has been idle, for egg_bob_offset_y() above.
 * Accumulated every frame the pet is an egg (see kf_creature_screen_
 * frame()); harmless to keep ticking across a screen re-entry or even
 * across an egg hatching and (in some future life) another egg starting --
 * this is a cosmetic phase with no gameplay meaning and nothing reads it
 * except the wave above, so there is nothing to get wrong by not resetting
 * it. */
uint32_t g_egg_bob_elapsed_ms = 0u;

/* The death scene (spec: docs/superpowers/specs/2026-08-09-core-care-loop-
 * design.md, "Death without a player holds on the last creature's scene"):
 * a small roadside shrine, centred in the field, the moment kf_pet_state::
 * dead becomes true. See kf_creature_screen_frame()'s own comment for why
 * this is special-cased ahead of the normal pose/sprite pipeline rather
 * than routed through it. */
constexpr const char *kShrineSpriteName = "shrine_idle_s";

/* The shrine art (tools/character_manifest.toml) is one sprite, one
 * direction -- a shrine is scenery, not a creature with facings, so unlike
 * every other sprite this screen draws there is no stage/pose/dir lookup
 * here at all, just this one fixed name (a pack ENTRY name, no frame
 * number -- see kf/creature.h's kf_creature_sprite_name() comment for why).
 * Sized like the creature's own 48x48 sprite footprint (hakoniwaos/src/
 * creature.cpp's kSpriteSize is private to that file, so this is a second,
 * independent constant, not a shared one -- the two happening to match
 * today is not a claim that they must stay in lockstep) for the
 * placeholder-rectangle fallback below, used until the art pipeline
 * actually ships shrine_idle_s -- see kPlaceholderColor's own comment for
 * why "visible and obviously wrong" beats "invisible" whenever the pack
 * does not have an asset yet. */
constexpr int16_t kShrinePlaceholderSize = 48;

/* Whether the shrine scene has been painted onto the panel yet for the
 * pet's current death. Same "-1 means nothing painted yet" shape as
 * g_drawn_poops, just boolean instead of a count: there is only ever one
 * shrine to draw (not zero-to-eight like poops), so "have I drawn it" is
 * the whole question. Reset to false by kf_creature_screen_enter() (a
 * screen re-entry wipes the panel, so whatever was there needs repainting
 * -- Controller amendment A2's reasoning applied to the shrine instead of
 * mess), which is also what makes a player who leaves the death scene (MENU
 * to Info) and comes back see the shrine again rather than a blank field. */
bool g_drawn_dead = false;

/* Centres a `width`x`height` box inside kField, floor-dividing any odd
 * remainder -- same "deterministic over which side absorbs the leftover
 * pixel, not fussy about which" reasoning as poop_rect()'s own slot
 * centring above. Used only for the shrine today; kept general (a size,
 * not a hardcoded 48) because the real shrine_idle_s sprite's actual
 * dimensions, once the art pipeline ships it, are very unlikely to be
 * exactly 48x48. */
kf_rect centered_in_field(int16_t width, int16_t height) {
    const int16_t x0 = static_cast<int16_t>(
        kField.x0 + ((kField.x1 - kField.x0) - width) / 2);
    const int16_t y0 = static_cast<int16_t>(
        kField.y0 + ((kField.y1 - kField.y0) - height) / 2);
    return kf_rect{x0, y0, static_cast<int16_t>(x0 + width),
                    static_cast<int16_t>(y0 + height)};
}

/* Draws the shrine, centred in the field: kf_assets_get() directly, NOT
 * kf_creature_sprite_name() -- see kf_creature_screen_frame()'s own
 * comment on why the death scene deliberately does not go through the
 * creature's normal sprite-name/pose machinery at all. Falls back to
 * kPlaceholderColor, exactly like every other not-yet-in-the-pack sprite
 * this screen draws, when the art pipeline has not shipped shrine_idle_s
 * yet -- which it may not have: the concurrent art-generation task names
 * it exactly that, but this code has to handle it being absent gracefully
 * either way, the same contract every other kf_assets_get() call in this
 * file already honours. */
void draw_shrine_scene(void) {
    const kf_sprite *shrine = kf_assets_get(kShrineSpriteName);
    if (shrine != nullptr) {
        const kf_rect r = centered_in_field(
            static_cast<int16_t>(shrine->width),
            static_cast<int16_t>(shrine->height));
        kf_blit(shrine, r.x0, r.y0);
    } else {
        const kf_rect r =
            centered_in_field(kShrinePlaceholderSize, kShrinePlaceholderSize);
        kf_fill_rect(r, kPlaceholderColor);
    }
}

kf_creature g_creature;
kf_rect g_previous = {0, 0, 0, 0};
bool g_up = false;

/* Whether the pet was an egg as of the LAST kf_creature_screen_frame()
 * call that actually looked -- i.e. whether the per-frame block below
 * that gates kf_creature_update() behind pet->stage == KF_PET_STAGE_EGG
 * ran its bob branch or its wander branch last time. Starts true: kf_
 * creature_screen_init() always runs right after a fresh kf_pet_session_
 * init(), and a fresh pet is always an egg (kf/pet.h's kf_pet_init()), so
 * the very first frame is never a transition.
 *
 * Exists solely to detect the transition INTO the egg stage -- see the
 * re-centre this drives, just below the egg gate in kf_creature_screen_
 * frame() -- not the transition out of it, which needs nothing extra:
 * kf_creature_update() simply resuming from wherever the egg was frozen
 * is already correct (kf_creature_init()'s own comment on the ordinary
 * "just hatched" case covers that, and this file's per-frame block above
 * it already explains why). The ordinary path -- boot as an egg, hatch,
 * grow up, never revisit EGG again -- never sets this back to true after
 * the first frame, so it never fires there. The one path that DOES is
 * kf_pet_session_debug_jump_to_stage(KF_PET_STAGE_EGG, ...)
 * (sdl_debug_window.cpp's "Egg" stage-jump button): that can land on a
 * pet that was, a moment ago, a CHILD or TEEN that had wandered all over
 * the field, and g_creature.x/y do not get reset just because pet->stage
 * did -- see the re-centre comment below for why that matters. */
bool g_was_egg = true;

/* How many poops are actually painted on screen right now -- NOT
 * pet->poop_count, which the mess-drawing code below compares this
 * against every frame precisely so it can tell "the count changed since
 * last frame, repaint" from "nothing to do". -1 is not a value
 * pet->poop_count (uint8_t) can ever hold, so it means "nothing painted
 * yet, or the screen was just wiped" -- the state right after
 * kf_creature_screen_init() and after every kf_creature_screen_enter()
 * (Controller amendment A2: the entry repaint wipes any mess along with
 * everything else, and nothing in the per-frame path would ever notice
 * unless this is reset too). Either way it forces the very next frame to
 * repaint regardless of what pet->poop_count already is. */
int g_drawn_poops = -1;

/* Remembers the outcome of the last sprite resolution (Controller amendment
 * A4: west-first lookup) so a creature with no west art pays one strcmp per
 * frame, not two failed kf_assets_get() lookups. `requested_name` is the
 * name actually asked for FIRST -- the "_w_" name when facing west, the
 * plain per-direction name otherwise -- which is also the only thing that
 * can legitimately change frame to frame (the pet's stage/pose/direction),
 * so comparing against it is exactly "did the thing that could change the
 * answer actually change". kf_assets_get()'s own contract (kf/assets.h)
 * is what makes caching the resolved pointer itself safe: valid for the
 * remainder of the program once it is non-null. */
struct SpriteCache {
    char requested_name[32] = {};
    const kf_sprite *sprite = nullptr;
    bool mirrored = false;
    bool valid = false;
};
SpriteCache g_sprite_cache;

/* Resolves the sprite (and whether to draw it mirrored) for this pet's
 * current pose and the creature's current facing, applying the west-first
 * fallback: ask the pack for real "_w_" art first, and only draw the "_e_"
 * sprite mirrored when the pack does not have any -- see kf/creature.h's
 * kf_creature_direction comment and kf/blit.h's kf_blit_mirrored() for the
 * two halves of why. Non-west directions have no fallback at all: build the
 * name, look it up, done. Falls all the way through to nullptr (handled by
 * the caller via the placeholder colour, above) when nothing resolves.
 *
 * Every OTHER test that runs this function still only ever sees the
 * checked-in default asset pack (examples/hello_sprite/assets.kfpack, one
 * sprite, "test_sprite"), so for them every kf_assets_get() call here
 * returns nullptr and only the `sprite == nullptr` branch (and the
 * placeholder-colour fallback above) ever runs -- deliberately: those
 * tests' golden checksums assume today's asset-less pack, and repointing
 * KF_ASSET_PACK or hand-rolling a pack there would break them for no
 * reason. The non-null branches -- a real sprite found on the first try,
 * the cache hit on a repeat lookup, and the "_w_"-not-found -> mirrored
 * "_e_" fallback -- are covered separately, without touching that default:
 * headless_main.cpp's run_creature_screen_sprite_check() points the
 * runtime pack override (kf_host_assets_set_pack_path(), host_assets.h) at
 * examples/creature_demo/assets.kfpack for the length of its own check
 * only, restoring it before returning, and drives this function through
 * kf_creature_screen_frame() with kf_creature_screen_debug_set_direction()
 * forcing each facing in turn. */
void resolve_sprite(const kf_pet_state *pet, kf_creature_pose pose,
                     kf_creature_direction dir, const kf_sprite **out_sprite,
                     bool *out_mirrored) {
    char name[32];
    kf_creature_sprite_name(pet, pose, dir, name, sizeof(name));

    if (g_sprite_cache.valid &&
        std::strcmp(name, g_sprite_cache.requested_name) == 0) {
        *out_sprite = g_sprite_cache.sprite;
        *out_mirrored = g_sprite_cache.mirrored;
        return;
    }

    /* The requested name just changed (or this is the very first call) --
     * a cache miss is exactly the moment the resolved sprite could have
     * changed, which is precisely when the animation cursor needs to reset
     * (kf_creature_anim_wrap()'s own comment, kf/creature.h): a 9-frame
     * walk cycle can leave g_creature.anim.frame at 7, and a 3-frame
     * objecting pose resolved next has no frame 7. kf_creature_anim_wrap()
     * (called just before every draw, below) would clamp that back to a
     * safe frame anyway, but only after visibly jumping mid-cycle for one
     * frame -- resetting here means a pose change always starts its new
     * cycle at frame 0, not wherever the previous pose's cursor happened to
     * be. */
    g_creature.anim.frame = 0u;
    g_creature.anim.accum_ms = 0u;

    const kf_sprite *sprite = kf_assets_get(name);
    bool mirrored = false;
    if (sprite == nullptr && dir == KF_CREATURE_DIR_W) {
        char east_name[32];
        kf_creature_sprite_name(pet, pose, KF_CREATURE_DIR_E, east_name,
                                sizeof(east_name));
        sprite = kf_assets_get(east_name);
        mirrored = (sprite != nullptr);
    }

    std::strncpy(g_sprite_cache.requested_name, name,
                 sizeof(g_sprite_cache.requested_name) - 1u);
    g_sprite_cache.requested_name[sizeof(g_sprite_cache.requested_name) - 1u] =
        '\0';
    g_sprite_cache.sprite = sprite;
    g_sprite_cache.mirrored = mirrored;
    g_sprite_cache.valid = true;

    *out_sprite = sprite;
    *out_mirrored = mirrored;
}

/* Task 6: hardware button input for the five care actions.
 *
 * This is NOT the product's care UI -- there is no menu, no icon, no
 * indication on screen of which button does what. It exists because Task 4
 * made the old LVGL Home (kf_pet_screen.cpp, still unreachable from a
 * running build -- see kf_screen_nav.h's own comment) unreachable, and that
 * screen held the only Feed/Play buttons a running simulator had. Without
 * this, care_actions_taken can only ever move from the Lua binding
 * (kf_lua_port.cpp), and the project owner asked to be able to watch the
 * creature react to care with his own hands, not just a demo script's.
 * What the real care UI looks like -- menus, icons, whatever a later layout
 * pass decides -- is still entirely undecided and not being settled here;
 * this is a way to exercise the care loop, not a proposal for what ships.
 *
 * KF_BTN_MENU and KF_BTN_B stay kf_screen_nav.cpp's alone (screen
 * switching); that leaves exactly five buttons for exactly five actions,
 * the mapping the brief spells out:
 *   A     -> feed   UP   -> play   DOWN -> rest
 *   LEFT  -> bath   RIGHT -> flush (no variation -- see kf_pet_flush())
 */

/* One variation counter PER ACTION, not one shared counter across all four
 * varying actions -- see this block's own header for why: three presses of
 * the SAME button must walk that action through all three variations,
 * which a shared counter would not guarantee (interleaving a Play press
 * between two Feed presses would advance Feed's variation on Play's turn).
 * Each starts at 0 and cycles 0 -> 1 -> 2 -> 0 -> ... independently.
 * KF_PET_CARE_VARIATION_COUNT (kf/pet.h) is Core's own count of how many
 * ways there are to do each action; this file does not hardcode 3. */
uint8_t g_feed_variation = 0u;
uint8_t g_play_variation = 0u;
uint8_t g_rest_variation = 0u;
uint8_t g_bath_variation = 0u;

/* For the info-level log below -- readable names for kf_pet_reaction, the
 * same three bands kf_creature_pose_for() (kf/creature.h) turns into a pose
 * on screen. This is the ONLY place this file spells the enum out as text;
 * everywhere else it stays a number, exactly like kf_pet_screen.cpp's own
 * teen_form/adult_branch captions do for opaque indices it has no name
 * for -- reaction, unlike those, already has three fixed, Core-defined
 * bands worth naming here for a human reading the log. */
const char *reaction_name(uint8_t reaction) {
    switch (reaction) {
    case KF_PET_REACTION_LIKED:
        return "liked";
    case KF_PET_REACTION_DISLIKED:
        return "disliked";
    case KF_PET_REACTION_NEUTRAL:
    default:
        return "neutral";
    }
}

/* Reads `pressed` (a kf_button edge bitmask -- see kf/types.h) and fires at
 * most one care action per button per call, through the kf_pet_session_*
 * wrappers only (kf_pet_session.h) -- never kf_pet_feed() et al. directly,
 * the same "presentation never mutates Core directly" rule every other
 * caller of this pet's state already follows. `pet` is the pointer
 * kf_pet_session_state() returned this frame: after a session call below
 * mutates the state it points at, `pet->last_reaction`/`pet->
 * care_actions_taken` already reflect that call, which is what the log
 * line and kf_creature_screen_frame()'s own care_actions_taken-changed
 * check both rely on.
 *
 * Split out of kf_creature_screen_frame() so kf_creature_screen_debug_
 * press() (below, DEBUG/TEST ONLY) can drive exactly this same logic with
 * a synthetic bitmask instead of a real kf_app_buttons_pressed() read --
 * one implementation, two callers, not a second less-tested path. */
void handle_care_buttons(const kf_pet_state *pet, uint32_t pressed) {
    if (pressed & KF_BTN_A) {
        const uint8_t variation = g_feed_variation;
        g_feed_variation = static_cast<uint8_t>(
            (variation + 1u) % KF_PET_CARE_VARIATION_COUNT);
        kf_pet_session_feed(variation);
        KF_LOGI(TAG, "feed variation=%u reaction=%s", variation,
                reaction_name(pet->last_reaction));
    }
    if (pressed & KF_BTN_UP) {
        const uint8_t variation = g_play_variation;
        g_play_variation = static_cast<uint8_t>(
            (variation + 1u) % KF_PET_CARE_VARIATION_COUNT);
        kf_pet_session_play(variation);
        KF_LOGI(TAG, "play variation=%u reaction=%s", variation,
                reaction_name(pet->last_reaction));
    }
    if (pressed & KF_BTN_DOWN) {
        const uint8_t variation = g_rest_variation;
        g_rest_variation = static_cast<uint8_t>(
            (variation + 1u) % KF_PET_CARE_VARIATION_COUNT);
        kf_pet_session_rest(variation);
        KF_LOGI(TAG, "rest variation=%u reaction=%s", variation,
                reaction_name(pet->last_reaction));
    }
    if (pressed & KF_BTN_LEFT) {
        const uint8_t variation = g_bath_variation;
        g_bath_variation = static_cast<uint8_t>(
            (variation + 1u) % KF_PET_CARE_VARIATION_COUNT);
        kf_pet_session_bath(variation);
        KF_LOGI(TAG, "bath variation=%u reaction=%s", variation,
                reaction_name(pet->last_reaction));
    }
    if (pressed & KF_BTN_RIGHT) {
        /* No variation, no reaction -- kf_pet_flush() leaves last_reaction/
         * last_care_action exactly as the previous real care action left
         * them (see its own comment in hakoniwaos/src/pet.cpp), so logging
         * either here would misattribute someone else's reaction to a
         * chore that has none of its own. */
        kf_pet_session_flush();
        KF_LOGI(TAG, "flush");
    }
}

/* ------------------------------------------------------------------------
 * Stats band (Task 9, docs/superpowers/plans/2026-08-11-hardware-bringup.md)
 *
 * The owner's own words, after the game ran on real hardware: "I also can't
 * see the pet's stats anywhere now like how hungry/tired etc." He is right:
 * the old LVGL pet screen (kf_pet_screen.cpp) had bars for hunger, happiness
 * and energy, and this screen took over Home without replacing them -- see
 * kField's own comment for how the band ended up empty in the first place.
 * Three bars, one per need kf_pet_state carries (kf/pet.h): hunger_mp,
 * happiness_mp, energy_mp, each 0..KF_PET_MILLIPERCENT_MAX (100000)
 * millipercent -- an integer, not a float (hakoniwaos/ stays free of
 * floating point, tools/check_no_heap.py), and NOT a plain percentage: a
 * bar's fill fraction is need_mp / KF_PET_MILLIPERCENT_MAX, not need_mp / 100.
 *
 * THE TRAP (this task's own brief, and Task 5 of the creature plan sprang
 * it once already for the mess row): a need decays over MINUTES, far slower
 * than the ~33ms frame tick, so the raw millipercent value changes almost
 * every single frame even though the bar's own on-screen appearance barely
 * ever does. Comparing the raw value and redrawing on any change would cost
 * three more dirty rectangles on nearly every frame, forever -- and past
 * KF_MAX_DIRTY_RECTS (8, kf/framebuffer.h) the whole framebuffer collapses
 * to one screen-sized box and re-transfers ~31ms against a 33.3ms budget:
 * invisible on a Mac, the entire budget on the device. The fix is the same
 * one g_drawn_poops already uses for the mess row: QUANTISE the need to the
 * bar's own pixel width first (stat_bar_filled_px(), below) and compare
 * THAT integer, not the millipercent it came from. A kStatsBarW-wide bar
 * then redraws at most kStatsBarW times across a need's whole 0..100% span,
 * not once per frame.
 *
 * THE OTHER TRAP (also already sprung twice on this branch, once for the
 * mess and once for the shrine): kf_creature_screen_enter() repaints the
 * whole 240x320 panel, which wipes whatever a bar last drew. Whatever
 * tracks "what is currently painted" -- g_drawn_stat_px, below -- has to be
 * reset there, or a bar that happened to already be showing the right
 * quantised width would never repaint after a screen re-entry and would
 * silently show stale content (or, right after a fresh kf_creature_screen_
 * init(), no content at all) forever. See kf_creature_screen_enter()'s own
 * comment for exactly where this happens.
 *
 * COEXISTING WITH THE GUIDE. The care-button guide (draw_care_guide(),
 * below) already lives in this same 240x60 band, and Task 9's own brief is
 * explicit that it must not simply be deleted: on a real device it is MORE
 * useful than on desktop, not less -- the seven buttons are unlabelled
 * tactile switches, so the guide is the only thing on screen that says what
 * any of them do. Both fit: three 8px-tall bar rows at the top of the band
 * (kStatsRowsY0=262 downward) leave the guide's own row room lower down
 * (kGuideTextY, redefined below, now centred in the REMAINING space under
 * the bars rather than the whole band) with clear margins on every side --
 * see the static_asserts below for what actually enforces that, rather than
 * this paragraph's arithmetic alone. */

/* Order matches kf_pet_state's own hunger_mp/happiness_mp/energy_mp field
 * order exactly, and is reused as the array index everywhere below --
 * kStatHunger == 0, etc. This is NOT kf_pet_care_action (Feed/Play/Rest/
 * Bath, kf/pet.h) -- a different, unrelated enumeration that happens to
 * also start at 0; do not confuse a stat's index with a care action's. */
enum { kStatHunger = 0, kStatHappiness = 1, kStatEnergy = 2, kStatCount = 3 };

constexpr const char *kStatLabels[kStatCount] = {"HUNGER", "HAPPY", "ENERGY"};

/* One fill colour per bar, chosen so the three read apart from each other
 * at a glance without reading the label -- orange/yellow/blue rather than
 * three shades of the same hue. The TRACK colour (the "not filled" portion)
 * is shared across all three bars on purpose: an empty bar communicates
 * "not full", and which need it is comes from the label and the row
 * position, not from a second colour axis. Plain flat colours, no
 * gradient -- the same "nothing subtle being attempted" reasoning
 * kPlaceholderColor's own comment gives, just not a deliberately-wrong
 * colour this time: this is real, if simple, UI. */
constexpr kf_color kStatFillColors[kStatCount] = {
    KF_RGB(214, 118, 40), /* hunger: orange */
    KF_RGB(224, 196, 32), /* happiness: yellow */
    KF_RGB(60, 140, 210), /* energy: blue */
};
constexpr kf_color kStatTrackColor = KF_RGB(190, 190, 190);

/* Layout. Three rows, top of the band, each KF_FONT_CELL_H (8px, kf/font.h)
 * tall with a 1px gap between rows (kStatsRowPitch=9) -- label text and bar
 * share the row rather than stacking, which is what keeps three bars inside
 * the top half of a 60-row band with real margin left for the guide below. */
constexpr int16_t kStatsRowsY0 = 262; /* 2px below kField's own y1 (260) */
constexpr int16_t kStatsRowPitch = static_cast<int16_t>(KF_FONT_CELL_H + 1);
constexpr int16_t kStatsLabelX0 = 2;

/* A compile-time strlen for the static_assert just below: kStatLabels holds
 * `const char *` (pointers), so sizeof() on one of its elements measures the
 * POINTER's own size (8 bytes), not the string it points at -- a mistake
 * this file caught at build time on its very first attempt (sizeof(ptr)-1
 * came out to 7 for every label, pointer-size-minus-one, regardless of which
 * string), which is exactly why this exists instead. Plain recursion is a
 * constexpr function's only tool before C++14's relaxed rules, but this
 * project builds C++17, so a loop would work too -- recursion here purely
 * because it is the shorter, more obviously terminating way to write it. */
constexpr size_t const_strlen(const char *s) {
    return (*s == '\0') ? 0u : 1u + const_strlen(s + 1);
}

/* The widest label this band draws is 6 characters ("HUNGER"/"ENERGY");
 * kf_text_width(str) (kf/font.h) is exactly strlen(str)*KF_FONT_CELL_W, so
 * this constant IS what kf_text_width() would return for either of them --
 * computed once here, at compile time, rather than called at every draw,
 * because BOTH draw_stat_labels() (the label text) and stat_bar_rect() (the
 * bar that must start clear of it) need the identical answer and must never
 * be allowed to disagree about it. The static_assert is what stops a longer
 * label silently drifting out of sync with this arithmetic if one is ever
 * added or renamed. */
constexpr int16_t kStatsLabelZoneChars = 6;
constexpr int16_t kStatsLabelZoneW =
    static_cast<int16_t>(kStatsLabelZoneChars * KF_FONT_CELL_W);
static_assert(const_strlen(kStatLabels[kStatHunger]) <=
                      static_cast<size_t>(kStatsLabelZoneChars) &&
                  const_strlen(kStatLabels[kStatHappiness]) <=
                      static_cast<size_t>(kStatsLabelZoneChars) &&
                  const_strlen(kStatLabels[kStatEnergy]) <=
                      static_cast<size_t>(kStatsLabelZoneChars),
              "kStatsLabelZoneChars must be >= every label's own length -- "
              "see the block comment above kStatsLabelZoneW for why");

constexpr int16_t kStatsBarX0 =
    static_cast<int16_t>(kStatsLabelX0 + kStatsLabelZoneW + 4);
constexpr int16_t kStatsBarW = 190;
constexpr int16_t kStatsBarH = KF_FONT_CELL_H;
static_assert(kStatsBarX0 + kStatsBarW <= kField.x1,
              "a stat bar would spill past the field's own right edge "
              "(240px) -- shrink kStatsBarW or kStatsLabelZoneChars");

/* Bottom of the third (last) bar row -- the top edge of whatever space is
 * left in the band for the guide, below. */
constexpr int16_t kStatsBandBottomOfBars = static_cast<int16_t>(
    kStatsRowsY0 + (kStatCount - 1) * kStatsRowPitch + kStatsBarH);
static_assert(kStatsBandBottomOfBars < KF_DISPLAY_HEIGHT,
              "the three stat bar rows do not fit above the band's own "
              "bottom edge (320) -- see kStatsRowPitch/kStatsRowsY0");

/* Bounding rect of stat bar `index`'s full 0..100% track, in framebuffer
 * space. `index` outside [0,kStatCount) returns an empty rect -- see
 * kf_creature_screen_debug_stat_bar_bounds()'s own header comment
 * (kf_creature_screen.h) for why that is a deliberate, testable contract
 * rather than undefined behaviour. */
kf_rect stat_bar_rect(int index) {
    if (index < 0 || index >= kStatCount) { return kf_rect{0, 0, 0, 0}; }
    const int16_t y0 =
        static_cast<int16_t>(kStatsRowsY0 + index * kStatsRowPitch);
    return kf_rect{kStatsBarX0, y0,
                    static_cast<int16_t>(kStatsBarX0 + kStatsBarW),
                    static_cast<int16_t>(y0 + kStatsBarH)};
}

/* How many of a bar's kStatsBarW pixels should be painted "filled" for a
 * need reading `mp` -- the quantisation this whole mechanism exists for,
 * see the block comment above kStatHunger for why. Plain integer division,
 * floors: `mp` is already kf_pet_millipercent (kf/pet.h), an exact integer,
 * so this introduces no rounding hakoniwaos/ would have to apologise for
 * (no float anywhere near it, tools/check_no_heap.py). Widening to
 * uint32_t before multiplying: 100000 * 190 is ~1.9e7, comfortably inside
 * uint32_t, but 100000 alone already exceeds int16_t's range, so the
 * multiplication has to happen at uint32_t width regardless of what the
 * result narrows back down to. */
int16_t stat_bar_filled_px(kf_pet_millipercent mp) {
    const uint32_t px = (static_cast<uint32_t>(mp) *
                          static_cast<uint32_t>(kStatsBarW)) /
                         KF_PET_MILLIPERCENT_MAX;
    return static_cast<int16_t>(px);
}

/* What update_stat_bar() last actually painted for each bar, in the same
 * quantised pixel units stat_bar_filled_px() returns -- exactly g_drawn_
 * poops's own "-1 means nothing painted yet, or the screen was just
 * entered" shape, one slot per bar instead of one shared count. -1 can
 * never collide with a real value: stat_bar_filled_px() only ever returns
 * [0, kStatsBarW], and kStatsBarW (190) is well short of int16_t's range,
 * so -1 stays a safe, unambiguous sentinel forever. Reset to -1 by
 * kf_creature_screen_enter() -- see that function's own comment -- which is
 * what forces every bar to repaint on the very next call, matching
 * whatever the panel wipe just erased them to. */
int16_t g_drawn_stat_px[kStatCount] = {-1, -1, -1};

/* Draws (or, on a steady frame, does nothing to) stat bar `index`, for a
 * need reading `mp`. A no-op unless the QUANTISED width actually differs
 * from what is already painted -- see the block comment above kStatHunger
 * for why comparing this integer, not the raw millipercent, is what keeps
 * three independently, continuously decaying needs off the per-frame
 * dirty-rect budget: called every frame (update_stat_bars(), below) but
 * only ever draws on the rare frame a bar's on-screen appearance would
 * actually differ.
 *
 * Up to two kf_fill_rect() calls when the bar is neither fully empty nor
 * fully full -- the filled portion in its own colour, the remainder in the
 * shared track colour -- but the two rects always share an edge (the
 * filled/empty boundary), so kf_fb_mark_dirty() (kf/framebuffer.h's own
 * comment on touching/overlapping rects merging) folds them into ONE
 * tracked dirty rectangle, not two. That is what keeps a single bar's
 * redraw costing exactly the "1 rect per changed bar" the design note this
 * task's plan gives budgets for, not 2. */
void update_stat_bar(int index, kf_pet_millipercent mp) {
    const int16_t filled = stat_bar_filled_px(mp);
    if (filled == g_drawn_stat_px[index]) { return; /* steady: nothing to do */ }
    const kf_rect full = stat_bar_rect(index);
    if (filled > 0) {
        kf_fill_rect(kf_rect{full.x0, full.y0,
                              static_cast<int16_t>(full.x0 + filled), full.y1},
                     kStatFillColors[index]);
    }
    if (filled < kStatsBarW) {
        kf_fill_rect(kf_rect{static_cast<int16_t>(full.x0 + filled), full.y0,
                              full.x1, full.y1},
                     kStatTrackColor);
    }
    g_drawn_stat_px[index] = filled;
}

/* Draws the three bars' fixed LABEL text only -- "HUNGER"/"HAPPY"/"ENERGY",
 * never redrawn again once painted. Called only from kf_creature_screen_
 * enter(), exactly like draw_care_guide() just below it and for the
 * identical reason: label text never changes once painted, so drawing it
 * from the per-frame path would cost three more rectangles every frame for
 * nothing. Safe at no extra rect-count cost there for the same reason
 * draw_care_guide() already is: it runs right after kScreen's own fill,
 * which already marks the entire panel dirty as one rectangle, so every
 * kf_text_draw() call below merges into that rather than adding rectangles
 * of its own. The BAR fill itself is a separate concern, drawn by
 * update_stat_bars() below, not this function. */
void draw_stat_labels(void) {
    for (int i = 0; i < kStatCount; ++i) {
        const int16_t y =
            static_cast<int16_t>(kStatsRowsY0 + i * kStatsRowPitch);
        kf_text_draw(kStatsLabelX0, y, kStatLabels[i], KF_BLACK, kBackground);
    }
}

/* Refreshes all three bars for the pet's CURRENT needs -- a no-op for any
 * bar whose quantised width has not moved since the last call (update_
 * stat_bar()'s own comment). Called both from kf_creature_screen_enter()
 * (once, right after the -1 reset just below draws it there -- see that
 * function's own comment for why the FIRST paint after an entry happens
 * here rather than being duplicated as its own special case) and from
 * kf_creature_screen_frame() every frame after that. Reads hunger_mp/
 * happiness_mp/energy_mp straight off `pet` (kf/pet.h) -- they already ARE
 * millipercent, 0..KF_PET_MILLIPERCENT_MAX, exactly stat_bar_filled_px()'s
 * own input range, not a percentage and not any other scale. */
void update_stat_bars(const kf_pet_state *pet) {
    update_stat_bar(kStatHunger, pet->hunger_mp);
    update_stat_bar(kStatHappiness, pet->happiness_mp);
    update_stat_bar(kStatEnergy, pet->energy_mp);
}
/* ------------------------------------------------------------------------ */

/* The on-screen button guide: the project owner could not tell which key
 * did what (there is no menu, no icon, nothing on screen -- see
 * handle_care_buttons()'s own header comment on this being an input
 * affordance, not a designed UI), and asked for a legible guide. This is
 * text labels, not pictographic icons: a true icon would need generated
 * art nobody has made yet, and kf/font.h's bitmap text (ADR 0010) is
 * already on hand and cheap. One label per care action, same order as
 * handle_care_buttons() above and the keyboard remap this task also makes
 * (sdl_input.cpp): feed, play, rest, bath, flush.
 *
 * "1:FEED" etc, not "A:FEED" -- these name the KEY a player presses on
 * THIS keyboard (1-5, per the owner's own request), not the physical
 * button (KF_BTN_A/UP/DOWN/LEFT/RIGHT). That is correct on a keyboard and
 * would be wrong on the real device, which has no number keys at all --
 * this file cannot fix that, and does not try to; see this task's own
 * report for why the device-facing wording is a design decision for the
 * project owner, not something to guess at here.
 *
 * Lives in the reserved stats band, y=[260,320) (kField's own comment),
 * BELOW the three stat bars Task 9 added (kStatsBandBottomOfBars and
 * everything above it) -- not a temporary occupant of the whole band any
 * more now that the real stats HUD has landed alongside it, per that task's
 * own brief: "do not simply delete the guide", because on the real device
 * it is MORE useful than on desktop, not a fixture the HUD displaces. */
constexpr const char *kGuideLabels[5] = {
    "1:FEED", "2:PLAY", "3:REST", "4:BATH", "5:FLUSH",
};

/* One slot per label, evenly dividing the field's width -- same
 * "deterministic, no leftover-pixel surprises" shape as kPoopSlotWidth,
 * and checked the same way: a static_assert rather than a comment's
 * promise. KF_PET_CARE_ACTION_COUNT (4, kf/pet.h) covers feed/play/rest/
 * bath; flush has no kf_pet_care_action of its own (see that enum's own
 * comment -- flushing is a chore, not a care action with an opinion
 * attached), so the guide's own fixed count of 5 slots is spelled out
 * directly rather than derived from a Core constant that does not include
 * it. */
constexpr int16_t kGuideSlotWidth =
    static_cast<int16_t>(kField.x1 / 5);
static_assert(kGuideSlotWidth * 5 == kField.x1,
              "kGuideSlotWidth * 5 must equal kField.x1 -- see the block "
              "comment above kGuideLabels for why");

/* Centred in whatever room is left UNDER the stat bars, not in the whole
 * band any more (contrast the old formula, which measured from y=260) --
 * kStatsBandBottomOfBars down through KF_DISPLAY_HEIGHT is the guide's own
 * remaining sub-region now, and this centres inside exactly that, the same
 * "vertically centred, not pinned to an edge" cosmetic reasoning as
 * before, just measured from the new floor. */
constexpr int16_t kGuideTextY = static_cast<int16_t>(
    kStatsBandBottomOfBars +
    (KF_DISPLAY_HEIGHT - kStatsBandBottomOfBars - KF_FONT_CELL_H) / 2);
static_assert(kGuideTextY >= kStatsBandBottomOfBars,
              "the guide's text row must not overlap the stat bars above it");
static_assert(kGuideTextY + KF_FONT_CELL_H <= KF_DISPLAY_HEIGHT,
              "the guide's text row must not run past the panel's own "
              "bottom edge");

/* Draws all five labels once. Called only from kf_creature_screen_enter()
 * -- see kf_creature_screen_frame()'s own comment (the dirty-rect trap)
 * for why this must never run from the per-frame path: five independent
 * label rectangles redrawn every frame would blow the whole
 * KF_MAX_DIRTY_RECTS budget on the guide alone, for a fixture that never
 * changes after it is first painted. Safe to call from kf_creature_
 * screen_enter() at no extra rect-count cost: it runs immediately after
 * that function's own kScreen fill, which already marks the ENTIRE panel
 * (including this band) dirty as one rectangle, so every kf_text_draw()
 * call below lands fully inside an already-dirty region and merges into
 * it (kf/framebuffer.h's kf_fb_mark_dirty() comment) rather than adding
 * rectangles of its own. */
void draw_care_guide(void) {
    for (int i = 0; i < 5; ++i) {
        const char *label = kGuideLabels[i];
        const int16_t text_w = kf_text_width(label);
        const int16_t slot_x0 = static_cast<int16_t>(i * kGuideSlotWidth);
        const int16_t x = static_cast<int16_t>(
            slot_x0 + (kGuideSlotWidth - text_w) / 2);
        kf_text_draw(x, kGuideTextY, label, KF_BLACK, kBackground);
    }
}

} // namespace

void kf_creature_screen_init(void) {
    kf_creature_init(&g_creature, kField);
    kf_creature_screen_enter();
    g_up = true;
}

void kf_creature_screen_frame(uint32_t dt_ms) {
    if (!g_up) { return; }
    const kf_pet_state *pet = kf_pet_session_state();

    /* Task 9: refresh the stats band before anything else this frame does,
     * including before the death branch just below -- a dead pet's last
     * known needs are still worth showing (the shrine scene freezes
     * everything else about the display the same way, see draw_shrine_
     * scene()'s own comment), and this call is a no-op on any frame none of
     * the three needs actually moved (update_stat_bar()'s own comment), so
     * placing it unconditionally here costs nothing on the steady frames
     * that make up the overwhelming majority of a running screen. */
    update_stat_bars(pet);

    /* The death scene (spec: "Death without a player holds on the last
     * creature's scene") is NOT a creature pose, and is handled entirely
     * outside the normal per-frame pipeline below -- no care buttons (Core
     * already no-ops every kf_pet_feed()/_play()/_rest()/_bath()/kf_pet_
     * flush() call once state->dead, see hakoniwaos/src/pet.cpp, so this
     * is a courtesy skip rather than a correctness requirement), no
     * wander, no mess redraw, and deliberately no call to kf_creature_
     * pose_for()/kf_creature_sprite_name() at all -- a shrine is scenery,
     * not something the creature struck a pose as. This is also why
     * KF_CREATURE_POSE_DEAD's fallback to the sick sprite
     * (kf_creature_sprite_name(), kf/creature.h) is now unreachable from
     * this screen: kf_creature_pose_for() is simply never called with
     * pet->dead true here any more. It stays exactly as it is regardless
     * -- kf_creature_pose_for() is still correct Core logic (still tested
     * directly by run_creature_pose_check(), and still what a future
     * non-screen caller, e.g. a debug bridge or a different front end,
     * would get if it asked) -- this screen has just stopped being the
     * caller that ever asks it about a dead pet.
     *
     * pet->dead is terminal FROM CORE'S OWN PERSPECTIVE -- kf/pet.h's
     * comment on the field says nothing IN THAT FILE ever clears it, and
     * that "a new creature is a new kf_pet_init()" is the honest way to
     * describe what reviving would otherwise look like: Core's position is
     * that there is no un-dying, only starting over. That is true of
     * kf/pet.cpp, but it is NOT true of this binary: kf_pet_session_debug_
     * reset() (the Reset button) and kf_pet_session_debug_jump_to_stage()
     * (the stage-jump buttons, sdl_debug_window.cpp) both call kf_pet_
     * init() directly on THIS SAME kf_pet_state instance, on THIS SAME
     * running screen, which sets state->dead back to false without this
     * screen ever losing focus or re-entering. From here, that is
     * indistinguishable from un-dying: pet->dead, the exact flag this
     * function polls every frame, really does go from true back to false
     * while the screen stays up. So there IS a case to handle beyond
     * "just died" and "already dead" -- see the block just below this one,
     * which is exactly that case. */
    if (pet->dead) {
        if (!g_drawn_dead) {
            kf_fill_rect(g_previous, kBackground); /* erase wherever the
                                                      * creature last stood */
            draw_shrine_scene();
            g_drawn_dead = true;
            /* Deliberately NOT touching g_drawn_poops or the mess row
             * (y=[232,244), see kPoopY0/kPoopY1 above) here: whatever
             * mess was on screen the moment the pet died stays exactly
             * as it was, painted right alongside the shrine. This is a
             * decision, not an oversight the spec happens not to
             * contradict -- "Death without a player holds on the last
             * creature's scene" (this comment's own header) reads most
             * honestly as the WHOLE scene, mess included, not a version
             * of it quietly tidied up the moment the creature dies. A
             * shrine standing in an otherwise-swept field would tell a
             * gentler story than the one that actually happened, and for
             * a creature whose neglect-driven death this mess is often
             * physical evidence of, that is the wrong story to tell.
             * Nothing else needs to change to get this: the mess-drawing
             * block further down never runs at all once this function
             * returns early below, so whatever was last painted simply
             * persists -- the same "do nothing, let it stand" mechanism
             * that already makes the shrine itself static once drawn. */
        }
        return;
    }

    /* Revive: g_drawn_dead is still true from a shrine painted on a
     * previous frame, but pet->dead just read false above -- one of the
     * two callers described in the block comment above flipped it back.
     * Left alone, g_drawn_dead would stay true forever (nothing else ever
     * clears it), so the shrine would sit painted at the field's centre
     * -- x=[96,144), y=[106,154), see centered_in_field()/
     * kShrinePlaceholderSize above -- on top of whatever this function
     * draws below for the rest of this pet's life, until the next
     * kf_creature_screen_enter() (a screen switch away and back)
     * happened to wipe it first.
     *
     * The fix is the field repaint kf_creature_screen_enter() already
     * does on every screen entry, run here instead because a revive is
     * not a screen entry -- Home stays the active screen the whole time,
     * so nothing else is going to call that function for us. One
     * kf_fill_rect(kField, ...) is exactly one dirty rectangle (kField is
     * one rect by construction), and everything the rest of this frame
     * draws -- the creature, and mess if any is waiting -- lands inside
     * that same rectangle and merges into it rather than adding a second
     * one (kf/framebuffer.h's kf_fb_mark_dirty() comment), so this still
     * costs exactly the "1" a normal frame already spends on the creature
     * -- see this file's own per-frame dirty-rect accounting further
     * down. That holds for THIS transition frame; it is not a per-frame
     * cost, because g_drawn_dead is immediately set false below and this
     * block does not run again until the pet dies and revives once more. */
    if (g_drawn_dead) {
        kf_fill_rect(kField, kBackground);
        g_drawn_dead = false;
    }

    /* Task 6: read this frame's debounced button edges the same way
     * kf_screen_nav.cpp reads MENU/B -- kf_app_buttons_pressed() directly,
     * not LVGL's keypad indev (see kf_screen_nav.h's own header comment for
     * why the two are kept orthogonal; the same reasoning applies to care
     * buttons). Only runs while THIS screen's own per-frame function is the
     * one being called at all, which kf_screen_nav_frame() already
     * guarantees is only while Home is the active screen (kf_screen_nav.cpp's
     * g_screens[g_active].update(dt_ms)) -- an inactive screen's update
     * never runs, so it never reaches this line, and needs no extra "am I
     * active" check of its own. Must run BEFORE the care_actions_taken
     * check just below, in the same call, so a button-triggered action is
     * noticed the instant it lands rather than one frame late. */
    handle_care_buttons(pet, kf_app_buttons_pressed());

    /* Notice a care action that happened since last frame and start the
     * reaction showing on the body. seen_care_actions/reaction_hold_ms live
     * on the presentation-only kf_creature, not on the pet itself -- see
     * kf/creature.h's own comment on why kf_pet_state::last_reaction being
     * sticky needs a caller-owned countdown on top of it. */
    if (pet->care_actions_taken != g_creature.seen_care_actions) {
        g_creature.seen_care_actions = pet->care_actions_taken;
        g_creature.reaction_hold_ms = 1200u;
    }

    /* The egg does not wander -- it sits still, centred in the field,
     * until it hatches (an egg that strolls across the field reads as
     * wrong, per the project owner's own reaction to watching it). kf_
     * creature_update() (hakoniwaos/src/creature.cpp) has no idea what a
     * life stage is -- it only knows an (x,y), a target, and a field -- and
     * is deliberately not being taught: bolting a `const kf_pet_state *`
     * onto a function whose only job is "move toward a target" would make
     * Core's wander depend on Core's pet simulation for a distinction that
     * only matters to PRESENTATION (whether to animate at all), when this
     * screen already has both `pet` and `g_creature` in scope and is a
     * strictly cleaner seam to decide it in. So the gate lives here: while
     * pet->stage == KF_PET_STAGE_EGG, kf_creature_update() is simply never
     * called, which leaves g_creature.x/y exactly where kf_creature_init()
     * put them -- the field's centre, see that function's own comment --
     * for as long as the pet stays an egg. The instant it hatches this
     * resumes calling kf_creature_update() every frame exactly as before;
     * nothing about the wander itself changes, and there is no special
     * "just hatched" transition to handle because the creature was already
     * sitting on a real, previously-chosen wander target the whole time
     * (kf_creature_init() picks one at construction, long before the pet
     * could ever have hatched).
     *
     * That "exactly where kf_creature_init() put them" guarantee is true
     * at boot and false after kf_pet_session_debug_jump_to_stage(
     * KF_PET_STAGE_EGG, ...) (sdl_debug_window.cpp's "Egg" button) lands
     * on a pet that had been wandering: the jump resets pet->stage, but
     * nothing resets g_creature.x/y, which simply stay wherever the
     * wander last left them -- anywhere in the field, not necessarily the
     * centre. Left alone, that produces two real bugs, not one merely
     * cosmetic one: an off-centre bobbing egg (wrong, but harmless), and
     * -- for a creature that had wandered close enough to the field's
     * bottom edge -- a bobbed draw rect whose y1 can reach 262, past
     * kField's own y1 of 260, breaking the "this file only ever draws
     * into y=[0,260) per frame" invariant this file's own header comment
     * states as a hard rule, not a guideline.
     *
     * Fixed by re-centring, not by clamping the bobbed rect to kField:
     * clamping would stop the overflow but leave the OTHER bug -- an egg
     * sitting wherever the previous creature happened to wander to, not
     * "in one place" the way the project owner asked for -- exactly as
     * wrong as it was before, just no longer crashing the invariant.
     * Re-centring fixes both defects with the one call that already
     * defines what "the egg's place" means (kf_creature_init(), used at
     * boot for the exact same purpose), rather than inventing a second,
     * weaker rule that only prevents the drawing bug and quietly leaves
     * the design bug in place. g_was_egg (above) is what lets this run
     * exactly once per transition, not every frame the pet stays an egg:
     * re-centring on every bob frame would fight kf_creature_screen_
     * debug_egg_bob_offset_y()'s own promise that offset 0 always means
     * "exactly where kf_creature_bounds() says", by moving that baseline
     * out from under a check mid-run. */
    if (pet->stage == KF_PET_STAGE_EGG) {
        if (!g_was_egg) {
            kf_creature_init(&g_creature, kField);
        }
        g_egg_bob_elapsed_ms += dt_ms;
        /* kf_creature_update() below is the wander's only caller of
         * kf_creature_tick_anim() (hakoniwaos/src/creature.cpp), and the
         * egg takes the OTHER branch, never calling it -- so without this,
         * the egg would be the one sprite in the game that never animates,
         * which is precisely backwards from what was asked for. See
         * kf_creature_tick_anim()'s own comment (kf/creature.h) for why
         * this is a second caller of the same function rather than a
         * second copy of its arithmetic. */
        kf_creature_tick_anim(&g_creature, dt_ms);
    } else {
        kf_creature_update(&g_creature, kField, dt_ms);
    }
    g_was_egg = (pet->stage == KF_PET_STAGE_EGG);

    /* Erase where it was, draw where it is. Two dirty rectangles at most:
     * one when the creature did not move this frame, since the erase below
     * and the draw further down then touch the exact same rectangle and
     * merge into one (see kf/framebuffer.h's own comment on
     * kf_fb_mark_dirty()) -- both marked by kf_fill_rect()/kf_blit_frame()/
     * kf_blit_frame_mirrored() themselves. See run_creature_screen_check()
     * for what pins this budget down, and this task's own report for why
     * advancing which FRAME gets drawn below never changes this count --
     * the erase and the draw happen every frame regardless, moving or not,
     * animating or not. */
    kf_fill_rect(g_previous, kBackground);

    /* Mess (Task 5). Static by construction: this only ever paints on a
     * frame where pet->poop_count actually differs from g_drawn_poops (it
     * grew, shrank, or the screen was just entered -- A2), or where the
     * erase just above happened to punch a hole through a poop that was
     * standing at the creature's old position. A frame where neither is
     * true touches no mess pixels and marks no mess rectangle at all --
     * the whole point being that eight poops redrawn every frame, on top
     * of the creature's own (up to two, per Controller amendment A1),
     * would blow past KF_MAX_DIRTY_RECTS. See run_creature_screen_check()
     * for what pins the steady-state budget down.
     *
     * Runs before the creature is drawn below, so the creature paints over
     * any mess it happens to be standing on -- it is walking ON the floor,
     * not under it. */
    if (pet->poop_count != g_drawn_poops) {
        /* The count changed, or this is the first frame after entry.
         * Repaint the row once: redraw every currently-active slot, having
         * first cleared whatever was there before -- UNLESS g_drawn_poops
         * is -1, in which case there is nothing to clear. -1 only ever
         * means "kf_creature_screen_init() or kf_creature_screen_enter()
         * just ran" (g_drawn_poops's own comment), and both of those
         * already fill the WHOLE panel (or field) to kBackground right
         * before setting it -- kMessBand sits entirely inside that fill,
         * so it is already background pixel for pixel. Clearing it again
         * here would cost a second, disjoint dirty rectangle (on top of
         * the one the entry repaint already spent) and 5,760 bytes for a
         * fill that changes no pixel -- see run_creature_screen_check()'s
         * worst_rects budget, which this exact redundancy used to eat.
         * Any other transition (poop_count genuinely grew or shrank while
         * g_drawn_poops was already a real count) DOES need the clear:
         * a shrink would otherwise leave stale poops from slots that are
         * no longer active. This still costs exactly ONE dirty rectangle
         * no matter how many poops are active -- every poop_rect() is
         * fully inside kMessBand, which the fill just below already
         * marked dirty, and a rectangle that overlaps an already-tracked
         * one merges into it instead of adding a new one (kf/framebuffer.h's
         * kf_fb_mark_dirty() comment). This is the one frame this task's
         * budget does not have to hold at 2 -- it is not part of the
         * steady-state loop the check measures, only the rare frame
         * something about the mess actually changed. */
        if (g_drawn_poops != -1) {
            kf_fill_rect(kMessBand, kBackground);
        }
        for (uint8_t i = 0; i < pet->poop_count; ++i) {
            kf_fill_rect(poop_rect(i), kPoopColor);
        }
        g_drawn_poops = pet->poop_count;
    } else {
        /* Nothing about the mess changed -- but the creature's OLD
         * position (g_previous, just erased above) may have been sitting
         * on top of a poop, and that erase painted background straight
         * over it. Put back only the poops the erase actually touched.
         * Free, not merely cheap: each redrawn poop_rect() overlaps
         * g_previous, which the erase already marked dirty, so it merges
         * into that same rectangle rather than adding a new one --
         * Controller amendment A1: "redraw them; do not build machinery
         * to avoid it." */
        for (uint8_t i = 0; i < pet->poop_count; ++i) {
            const kf_rect r = poop_rect(i);
            if (!kf_rect_is_empty(kf_rect_intersect(g_previous, r))) {
                kf_fill_rect(r, kPoopColor);
            }
        }
    }

    const kf_creature_pose pose =
        kf_creature_pose_for(pet, g_creature.reaction_hold_ms);
    const kf_sprite *sprite = nullptr;
    bool mirrored = false;
    resolve_sprite(pet, pose, g_creature.dir, &sprite, &mirrored);

    kf_rect now = kf_creature_bounds(&g_creature);
    if (pet->stage == KF_PET_STAGE_EGG) {
        /* The bob: shift the DRAWN rect up/down by a few pixels around the
         * position kf_creature_bounds() reports -- see egg_bob_offset_y()'s
         * own comment above for the wave itself, and this file's header
         * comment on kf_creature_screen_debug_bounds() for why that debug
         * accessor deliberately still returns the UNshifted position: it
         * exists to expose the wander's own state (kf_creature::x/y),
         * which the bob never touches, not "wherever the sprite happened
         * to land on screen this frame". Applied to both g_previous (via
         * the assignment at the end of this function) and `now` alike, so
         * next frame's erase always targets exactly where this frame's
         * draw actually put pixels -- an erase that missed the bob offset
         * would leave a stray sliver of the old frame behind every time
         * the offset changed. */
        const int16_t offset = egg_bob_offset_y(g_egg_bob_elapsed_ms);
        now.y0 = static_cast<int16_t>(now.y0 + offset);
        now.y1 = static_cast<int16_t>(now.y1 + offset);
    }
    if (sprite != nullptr) {
        /* Bring the cursor back in range for THIS sprite before reading it
         * -- resolve_sprite()'s cache-miss branch already reset it to 0 the
         * frame the resolved sprite changed, but this still runs every
         * frame (not just on a change) as the belt to that braces: it is
         * what actually stops a stale, past-the-end frame from ever
         * reaching kf_blit_frame() below, rather than merely making it rare.
         * frame_count == 1 was true for every real sprite in the repo when
         * this task landed (proven then only by the fixture pack's
         * test_sprite_anim, so this call was a no-op in practice). It is no
         * longer universally true: the roster's first animated poses
         * (egg's "idle" and every other animated entity's "neutral" -- see
         * tools/character_manifest.toml's state_frames entries and
         * .superpowers/sdd/first-animations-report.md) now carry
         * frame_count == 9, and this is the call that keeps their cursor
         * legal on every pose/direction change. Still a no-op for every
         * sprite that remains single-frame -- kf_creature_anim_wrap()
         * clamps into [0, frame_count), and [0, 1) only ever contains 0. */
        kf_creature_anim_wrap(&g_creature, sprite->frame_count);
        if (mirrored) {
            kf_blit_frame_mirrored(sprite, now.x0, now.y0, g_creature.anim.frame);
        } else {
            kf_blit_frame(sprite, now.x0, now.y0, g_creature.anim.frame);
        }
    } else {
        /* No art in the pack for this pet yet -- see kPlaceholderColor's
         * own comment above for why this draws something obviously wrong
         * rather than nothing at all. */
        kf_fill_rect(now, kPlaceholderColor);
    }
    g_previous = now;
}

void kf_creature_screen_enter(void) {
    /* Repaints the WHOLE 240x320 panel, not just kField -- because LVGL's
     * display driver flushes into this same framebuffer at the full panel
     * size (kf_lvgl_display.cpp), any LVGL screen (Info, today) can leave
     * pixels sitting in y=[260,320) that this screen would otherwise never
     * touch again: kf_screen_nav_wants_lvgl() is false once Home is active,
     * so the LVGL pump that would normally repaint Info's own leftovers
     * never runs while Home is showing. One dirty rect either way --
     * kScreen fully contains kField, and overlapping/touching rects merge
     * (kf/framebuffer.h's own comment on kf_fb_mark_dirty()) -- so this
     * costs nothing extra against the RECT-COUNT budget run_creature_
     * screen_check() (simulator/src/headless/headless_main.cpp) polices:
     * one dirty rect, same as if only kField had been touched. It is NOT
     * free in BYTES, though -- 240x320x2 = 153,600, well past that same
     * check's 13,824-byte per-frame limit. Nothing breaks: this function
     * is called once per screen switch, never from inside a frame either
     * check measures. Said plainly so the byte cost is not mistaken for
     * zero just because the rect cost is.
     *
     * The reserved stats band (kField's own comment, above) gets its own
     * two fixtures painted on top of this blank fill: the care-button guide
     * (draw_care_guide()) and, as of Task 9, the three stat bars' fixed
     * labels (draw_stat_labels()) -- both calls have to come AFTER this
     * fill, not before, or the fill would immediately wipe whatever they
     * just drew. The bars' own FILL portion is deliberately NOT painted
     * here by a third call -- see update_stat_bars() being invoked below,
     * after g_drawn_stat_px is reset, for why that one step is shared with
     * the per-frame path rather than duplicated as its own special case
     * here (the same choice mess-drawing already makes: g_drawn_poops's own
     * comment, just below). */
    kf_fill_rect(kScreen, kBackground);
    draw_care_guide();
    draw_stat_labels();
    g_previous = kf_creature_bounds(&g_creature);

    /* The death scene must be invalidated on entry too, the same reasoning
     * as g_drawn_poops just below applied to the shrine instead of mess:
     * the fill just above wiped any shrine that was painted, but does not
     * touch pet->dead itself, so left alone g_drawn_dead would still read
     * true and the next kf_creature_screen_frame() would think it had
     * nothing left to do -- a blank field instead of the shrine, for a
     * pet that is in fact still dead. This is what makes leaving the death
     * scene (MENU, to Info) and coming back repaint the shrine rather than
     * leaving it missing. */
    g_drawn_dead = false;

    /* Mess must be invalidated here too (Controller amendment A2): the
     * fill just above just wiped any poops that were painted on screen,
     * along with everything else, but it does not touch pet->poop_count
     * itself -- Core's number is unaffected, only what is currently
     * PAINTED is. Left alone, g_drawn_poops would still read whatever it
     * held before this entry, so the next kf_creature_screen_frame() would
     * see "count unchanged" and draw nothing, and the mess would silently
     * stay invisible until poop_count happens to change for some other
     * reason. -1 is not a real count, so it forces that next frame to
     * repaint regardless of what pet->poop_count already is -- see
     * g_drawn_poops's own comment. Not drawing the mess itself here: that
     * stays kf_creature_screen_frame()'s job, run once, the very next time
     * it is called. */
    g_drawn_poops = -1;

    /* Stat bars (Task 9): same -1-sentinel reasoning as g_drawn_poops just
     * above, one slot per bar (g_drawn_stat_px's own comment) -- the fill
     * just wiped whatever was painted, but does not touch the pet's needs
     * themselves, so left alone every bar would read "unchanged" on the
     * next comparison and stay invisible. UNLIKE the mess, this function
     * DOES paint the bars itself, right here, rather than leaving it for
     * the very next kf_creature_screen_frame() call: a bar showing nothing
     * at all until the first per-frame tick would be a visible, if
     * momentary, blank band on every screen entry, and update_stat_bars()
     * is idempotent and cheap to call an extra time -- unlike mess, which
     * has a real "-1 means skip the redundant clear" branch that this
     * function's own fill above already makes correct without needing the
     * call at all. This paint merges into the very same dirty rectangle
     * kScreen's fill above already opened (kf/framebuffer.h's kf_fb_mark_
     * dirty() comment on touching rects merging), so it costs nothing extra
     * against the per-entry rect budget either. */
    update_stat_bars(kf_pet_session_state());
}

void kf_creature_screen_debug_set_direction(kf_creature_direction dir) {
    g_creature.dir = dir;
}

kf_rect kf_creature_screen_debug_bounds(void) {
    return kf_creature_bounds(&g_creature);
}

void kf_creature_screen_debug_press(uint32_t buttons) {
    handle_care_buttons(kf_pet_session_state(), buttons);
}

uint32_t kf_creature_screen_debug_reaction_hold_ms(void) {
    return g_creature.reaction_hold_ms;
}

int16_t kf_creature_screen_debug_egg_bob_offset_y(void) {
    return egg_bob_offset_y(g_egg_bob_elapsed_ms);
}

uint16_t kf_creature_screen_debug_anim_frame(void) {
    return g_creature.anim.frame;
}

kf_rect kf_creature_screen_debug_stat_bar_bounds(int index) {
    return stat_bar_rect(index);
}

int16_t kf_creature_screen_debug_stat_bar_filled_px(int index) {
    if (index < 0 || index >= kStatCount) { return -1; }
    return g_drawn_stat_px[index];
}
