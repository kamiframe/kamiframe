/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 * Task 5 of the Lua game-layer plan: "when on_frame has been disabled by an
 * error, the engine draws the message into the reserved band. The panel
 * says what the console says."
 * Scoped to the LUA-OWNS-HOME case only (kf_lua_home_screen.cpp), not
 * kf_creature_screen.cpp: the C++ screen's own correctness never depends on
 * Lua (Global Constraint -- "the C++ screen must remain fully working
 * under KF_HOME_SCREEN=cpp"), so a narration-only script erroring under
 * that build leaves nothing on the PICTURE broken to announce, only the
 * log. When Lua itself is what draws Home, a disabled on_frame means the
 * scene has frozen on its last good frame with no other indication --
 * exactly the case this banner exists for.
 */

#ifndef KF_ERROR_BANNER_H
#define KF_ERROR_BANNER_H

#include "kf/scene.h"

#include <cstdint>

/* The row the banner occupies -- the 12px gap between the stat bars'
 * bottom edge (y=288, see kf_creature_screen.cpp's stats-band layout
 * comment: three rows of KF_FONT_CELL_H+1 starting at y=262) and the
 * care-button guide row (y=300). A shared constant, not duplicated by
 * value, so nothing can quietly disagree about where this sits. */
constexpr int16_t KF_ERROR_BANNER_Y = 290;

/* Creates the banner as a hidden text object and returns its id -- call
 * once, from the owning screen's own entry point, after every other object
 * it creates in that same call (so it paints in front of them once shown).
 * Hidden by default: kf/scene.h's own "a new object starts visible"
 * comment on kf_scene_add_box()/_text()/_sprite() is exactly why this
 * explicitly hides it right away -- a caller that never sees a script
 * error must never pay a single dirty-rect for one. */
kf_scene_id kf_error_banner_create(void);

/* Shows/hides the banner and, while shown, keeps its text in sync with
 * kf_lua_port_last_error() -- call once per frame, after every other
 * declaration that frame. Costs nothing on a frame nothing about the error
 * state changed: kf_scene_set_visible()/_set_text() only mark dirty on an
 * actual change, the same redundant-comparison-elimination every other
 * setter in this codebase already gets from kf_scene_commit(). */
void kf_error_banner_update(kf_scene_id id);

#endif /* KF_ERROR_BANNER_H */
