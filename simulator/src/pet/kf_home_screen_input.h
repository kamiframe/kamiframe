/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * Task 5 of the Lua game-layer plan: the hardware care buttons (Task 6 of
 * the pet-screen plan), extracted out of kf_creature_screen.cpp so they
 * keep working under EITHER KF_HOME_SCREEN implementation. Before that
 * task there was only ever one screen reading kf_app_buttons_pressed()
 * for care actions, so coupling this to kf_creature_screen.cpp's own
 * per-frame function cost nothing; now that a second implementation
 * (kf_lua_home_screen.cpp) can own Home instead, a hardware button press
 * has to reach kf_pet_session_* regardless of which one is compiled in, or
 * KF_HOME_SCREEN=lua would silently make Feed/Rest/Bath/Flush dead
 * buttons -- a regression that task had to not introduce even though
 * moving INPUT into the game is explicitly a later task's job, not that
 * one's.
 *
 * PLAY (UP) NO LONGER LIVES IN kf_home_screen_handle_care_buttons() BELOW
 * -- Task 5 of the Nibble-and-the-game-session plan moved it out: Home's
 * 2:PLAY now opens a picker (creature.lua's own Home-only block reads
 * kf.button("up") directly and shows it) rather than playing immediately,
 * so UP pressed on Home does nothing at the C level any more. What used
 * to be the UP branch here is now kf_home_screen_quick_play() below,
 * called from the picker's "Quick play" choice instead of from a raw
 * button edge -- same variation-cycling counter, same kf_pet_session_
 * play() call, same log line; only WHAT triggers it changed.
 */

#ifndef KF_HOME_SCREEN_INPUT_H
#define KF_HOME_SCREEN_INPUT_H

#include "kf/pet.h"

#include <cstdint>

/* Reads `pressed` (a kf_button bitmask, the debounced press-edge state
 * kf_app_buttons_pressed() already computes) and dispatches A/DOWN/LEFT to
 * feed/rest/bath respectively, RIGHT to flush -- each with its own
 * independently-cycling variation counter (0->1->2->0, KF_PET_CARE_
 * VARIATION_COUNT wide), so five presses of the SAME button in a row do not
 * all land on the pet's favourite or least-favourite variation. UP is
 * deliberately not handled here any more -- see this header's own
 * top-of-file comment. */
void kf_home_screen_handle_care_buttons(const kf_pet_state *pet,
                                        uint32_t pressed);

/* The Home care action UP used to trigger directly, now triggered by the
 * play picker's "Quick play" choice instead (kf.quick_play(), sdk/lua/
 * kf_lua_port.cpp) -- see this header's own top-of-file comment for why
 * it moved here rather than disappearing. Same cycling counter
 * (g_play_variation, shared with nothing else -- PLAY's own, exactly as
 * before), same kf_pet_session_play() call, same "no-op against a
 * sleeping pet" gate (kf_pet_session_play() itself already refuses that,
 * matching every other care action). A no-op call costs nothing worth
 * guarding against here: the picker itself only offers Quick play while
 * there is a session to play against. */
void kf_home_screen_quick_play(void);

/* Resets all four per-action variation counters to 0 -- TEST ONLY. They are
 * process-global (one shared counter set outlives any single pet session),
 * which is exactly wrong for headless_main.cpp's run_lua_vs_cpp_screen_
 * check(): it drives the SAME button press through two separate pet
 * sessions in one process, and the second session must see variation 0
 * again, not wherever the first session's presses left the counters. Not
 * useful outside a test that resets its whole pet session between runs in
 * one process the way that check does. */
void kf_home_screen_input_reset_variations_for_test(void);

#endif /* KF_HOME_SCREEN_INPUT_H */
