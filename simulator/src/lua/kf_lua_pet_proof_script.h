/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * PLACEHOLDER CONTENT, same spirit as kf_lua_proof_script.h and
 * kf_lvgl_proof_screen.h: this proves the pet.* Lua binding surface reads
 * and mutates the real, live kf_pet_session state correctly, not that any
 * particular pet game logic is correct. See ADR 0016 and
 * headless_main.cpp's run_lua_pet_check(), which runs these two scripts
 * back to back against the SAME continuing pet session.
 *
 * Two scripts, not one, because they prove two different things and
 * mixing them into a single kf.report() call would need to encode
 * multiple values into one integer:
 *
 *   kKfLuaPetDecayProofScriptSource   Reports pet.hunger() every frame,
 *       touching nothing else. Proves reads are wired to a genuinely live,
 *       ticking state -- run_lua_pet_check() checks the last reported
 *       value against kf_pet_session_state()->hunger_mp read directly
 *       from C++ (proves the FFI marshaling is exact) AND that it is
 *       strictly less than max after enough live frames (proves it is
 *       really decaying, not frozen).
 *
 *   kKfLuaPetCareProofScriptSource    Calls pet.feed(), pet.play() and
 *       pet.rest() every frame, then reports pet.hunger(). Since the care
 *       boost vastly exceeds one frame's decay (see kf/pet.cpp's
 *       kCareBoostMp), all three needs should be pinned at
 *       KF_PET_MILLIPERCENT_MAX by the end -- checked both via the
 *       script's own report (proves pet.feed() is wired) and by reading
 *       happiness_mp/energy_mp directly from kf_pet_session_state() in
 *       C++ (proves pet.play() and pet.rest() are ALSO correctly wired,
 *       not just aliased to the same call as pet.feed()).
 */

#ifndef KF_LUA_PET_PROOF_SCRIPT_H
#define KF_LUA_PET_PROOF_SCRIPT_H

inline constexpr const char *kKfLuaPetDecayProofScriptSource = R"lua(
function on_frame(dt_ms)
    kf.report(pet.hunger())
end

kf.log("pet decay proof script loaded")
)lua";

inline constexpr const char *kKfLuaPetDecayProofScriptChunkName =
    "=pet_decay_proof_script";

inline constexpr const char *kKfLuaPetCareProofScriptSource = R"lua(
function on_frame(dt_ms)
    pet.feed()
    pet.play()
    pet.rest()
    kf.report(pet.hunger())
end

kf.log("pet care proof script loaded")
)lua";

inline constexpr const char *kKfLuaPetCareProofScriptChunkName =
    "=pet_care_proof_script";

#endif /* KF_LUA_PET_PROOF_SCRIPT_H */
