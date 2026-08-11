/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * Task 5 of the Lua game-layer plan: the five hardware care buttons
 * (Task 6 of the pet-screen plan), extracted out of kf_creature_screen.cpp
 * so they keep working under EITHER KF_HOME_SCREEN implementation. Before
 * this task there was only ever one screen reading kf_app_buttons_
 * pressed() for care actions, so coupling this to kf_creature_screen.cpp's
 * own per-frame function cost nothing; now that a second implementation
 * (kf_lua_home_screen.cpp) can own Home instead, a hardware button press
 * has to reach kf_pet_session_* regardless of which one is compiled in, or
 * KF_HOME_SCREEN=lua would silently make Feed/Play/Rest/Bath/Flush dead
 * buttons -- a regression this task must not introduce even though moving
 * INPUT into the game is explicitly Task 7's job, not this one.
 */

#ifndef KF_HOME_SCREEN_INPUT_H
#define KF_HOME_SCREEN_INPUT_H

#include "kf/pet.h"

#include <cstdint>

/* Reads `pressed` (a kf_button bitmask, the debounced press-edge state
 * kf_app_buttons_pressed() already computes) and dispatches A/UP/DOWN/LEFT
 * to feed/play/rest/bath respectively, RIGHT to flush -- each with its own
 * independently-cycling variation counter (0->1->2->0, KF_PET_CARE_
 * VARIATION_COUNT wide), so five presses of the SAME button in a row do not
 * all land on the pet's favourite or least-favourite variation. */
void kf_home_screen_handle_care_buttons(const kf_pet_state *pet,
                                        uint32_t pressed);

#endif /* KF_HOME_SCREEN_INPUT_H */
