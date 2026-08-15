/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * Global device settings -- persisted INDEPENDENTLY of any one pet's save
 * (kf/pet.h's KF_PET_SAVE_KEY), so they survive kf_pet_session_debug_
 * reset() and a fresh egg. This is not a hypothetical distinction: it is
 * the whole reason this file exists. The first field is volume, and the
 * owner was explicit about its lifetime: "persist globally through
 * multiple pets until the user changes it again themselves in settings."
 * A pet's own save is wiped by a reset; this store is never touched by
 * one.
 *
 * Deliberately tiny and general-shaped: ONE small versioned blob under its
 * own store key (KF_SETTINGS_SAVE_KEY), the same fixed-byte-layout
 * convention kf/pet.h's own save format uses (see hakoniwaos/src/
 * settings.cpp's pack()/unpack()) -- sized to hold whatever the NEXT
 * global, cross-pet preference turns out to be too. A one-key-per-option
 * store would be the wrong shape for a device that is about to have a
 * second one of these.
 *
 * Pure Core logic: no HAL calls inside kf_settings_default(), matching
 * kf/pet.h's own "only the save/load wrapper functions touch the HAL"
 * split -- kf_settings_save()/kf_settings_load() are the thin wrappers,
 * not where any real logic lives (there is none; this is a single byte).
 *
 * Valid C, same convention as kf/pet.h and kf/arena.h: nothing here
 * belongs to the HAL boundary every header under kf/hal/ is mechanically
 * checked against, but there is no reason for this header to be any less
 * portable than it needs to be.
 */

#ifndef KF_SETTINGS_H
#define KF_SETTINGS_H

#include "kf/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Volume, 0..4 -- the SAME numeric meaning as kf_volume_level (kf/hal/
 * audio.h's KF_VOLUME_OFF..KF_VOLUME_4), but stored here as a raw uint8_t
 * rather than that enum type itself: this header has nothing to do with
 * the HAL boundary (see this file's own header comment), and kf_pet_state's
 * own last_reaction/last_care_action fields already make the identical
 * "opaque small integer, not the HAL/Core enum type" choice for the same
 * reason -- keeping this a plain byte means kf/settings.h does not have to
 * include kf/hal/audio.h just to name one field's type. The mapping is
 * exact (hakoniwaos/src/settings.cpp's unpack() validates it against that
 * enum's own range), not merely similar. */
/* Brightness, 1..4. Stored the same way and for the same reasons as volume
 * above: a plain byte, not a HAL type.
 *
 * FOUR LEVELS, NOT FIVE, and the asymmetry with volume is deliberate rather
 * than an oversight. Volume's fifth position is OFF, which is a thing a
 * person genuinely wants: a silent pet is still a pet. A screen at zero
 * brightness is not a dim screen, it is a device that looks broken and has
 * no way to tell you it is not -- you cannot read the menu you would need in
 * order to turn it back up. So the range starts at 1, and "off" is what the
 * sleep path does on its own schedule, where the device knows how to undo
 * it. */
typedef struct {
    uint8_t volume;
    uint8_t brightness;
} kf_settings;

/* KF_VOLUME_4 (kf/hal/audio.h) -- loudest, non-off -- closest to this
 * feature's own PRE-EXISTING behaviour (every sound always played at full,
 * unscaled amplitude) for a device that has never had this setting stored
 * yet. FEEL, Chris's to tune once he has heard all five levels on the
 * board, same status as every other illustrative default in this
 * codebase. */
#define KF_SETTINGS_DEFAULT_VOLUME 4u

/* Full brightness, matching the pre-existing behaviour for a device that has
 * never stored this setting: the backlight was a plain on/off GPIO until
 * 2026-08-14 and "on" meant full. FEEL, Chris's to tune on the board -- the
 * same status as the volume default above. */
#define KF_SETTINGS_BRIGHTNESS_MIN 1u
#define KF_SETTINGS_BRIGHTNESS_MAX 4u
#define KF_SETTINGS_DEFAULT_BRIGHTNESS 4u

/* The 0..255 duty kf_display_set_backlight() wants, for a 1..4 level.
 *
 * PERCEPTUAL, not linear, for exactly the reason the volume curve is (ADR
 * 0057): eyes respond roughly logarithmically to light, so evenly-spaced
 * duty values read as "bright, bright, bright, slightly less bright". These
 * four are spaced to look evenly stepped instead. The bottom one is 10%
 * rather than something smaller because an OLED-style near-black is not
 * useful on a transmissive LCD -- below about this the backlight stops
 * lighting the panel usefully and the screen just looks broken, which is the
 * same failure the missing OFF position above avoids. */
uint8_t kf_settings_brightness_duty(uint8_t level);

/* Every field at its documented default. */
kf_settings kf_settings_default(void);

#define KF_SETTINGS_SAVE_KEY "settings"
#define KF_SETTINGS_SAVE_BYTES 3u /* version + volume + brightness -- see
                                    * hakoniwaos/src/settings.cpp's pack()/
                                    * unpack() for the exact layout */

/* Writes `settings` under KF_SETTINGS_SAVE_KEY. Call the instant the
 * volume actually changes (the Settings screen's own A-on-SAVE, mirroring
 * kf.set_clock()'s own commit point exactly) -- not batched, not deferred,
 * the same "the device is unplugged, never shut down cleanly" reasoning
 * ADR 0056 applies to care-action saves. */
kf_result kf_settings_save(const kf_settings *settings);

/* Reads the save under KF_SETTINGS_SAVE_KEY into `settings`, falling back
 * to kf_settings_default() if there is no save yet (KF_ERR_UNAVAILABLE from
 * kf_store_read()) or it was written by an incompatible version -- exactly
 * kf_pet_load_and_advance()'s own two fallback paths (kf/pet.h), for the
 * identical reason: a fresh device or an old/corrupt save should land on a
 * defined, safe default, not an error. Returns whatever kf_store_read()
 * itself returned on any OTHER failure (propagated, not swallowed); KF_OK
 * on every path that actually populated `settings`, including both
 * fallback ones. */
kf_result kf_settings_load(kf_settings *settings);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KF_SETTINGS_H */
