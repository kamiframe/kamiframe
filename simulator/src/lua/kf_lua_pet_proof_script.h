/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * PLACEHOLDER CONTENT, same spirit as kf_lua_proof_script.h and
 * kf_lvgl_proof_screen.h: this proves the pet.* Lua binding surface reads
 * and mutates the real, live kf_pet_session state correctly, not that any
 * particular pet game logic is correct. See ADR 0016 (and ADR 0021 for the
 * stage/evolution additions) and headless_main.cpp's run_lua_pet_check(),
 * which runs these scripts back to back against the SAME continuing pet
 * session.
 *
 * Five scripts, not one, because they prove different things and mixing
 * them into a single kf.report() call would need to encode multiple values
 * into one integer:
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
 *
 *   kKfLuaPetStageProofScriptSource   Reports pet.stage() (mapped to the
 *       same 0..4 index kf_pet_stage uses in C++, via a local Lua table --
 *       proves the STRING pet.stage() hands back round-trips correctly,
 *       not just that some string comes out), pet.teen_form(),
 *       pet.adult_branch(), pet.base_trait() and pet.dominant_care_trait()
 *       (ADR 0023), packed into one integer (stage*100000 +
 *       teen_form*10000 + adult_branch*1000 + base_trait*10 +
 *       dominant_care_trait -- widened from the original stage/teen_form/
 *       adult_branch-only packing to fit the two new fields, each still
 *       comfortably within its own decimal digit: stage is [0,4],
 *       teen_form [0,2], adult_branch [0,1], base_trait [0,5]
 *       (KF_PET_BASE_TRAIT_COUNT), dominant_care_trait [0,2]).
 *       run_lua_pet_check() computes the identical packed value directly
 *       from kf_pet_session_state()/kf_pet_dominant_care_trait() in C++
 *       and compares -- the same "script report vs. live C++ state" proof
 *       the other two scripts use, extended to the five accessors.
 *
 *   kKfLuaPetMessProofScriptSource    Reports pet.poops() and
 *       pet.dirtiness() packed into one integer (poops * 1000000 +
 *       dirtiness -- dirtiness is millipercent, so at most 100000, a
 *       decimal digit clear of the multiplier). Touches nothing. Proves
 *       the two mess reads see live state, by the same comparison the
 *       decay script uses for hunger.
 *
 *   kKfLuaPetCleanProofScriptSource   Calls pet.clean() and reports
 *       pet.poops(). Run immediately after the mess script has left real
 *       mess on the floor, so a zero afterwards can only mean the call
 *       reached kf_pet_clean() -- the same "mutate, then check the live
 *       C++ state independently" proof the care script uses, applied to
 *       the fourth care action.
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

inline constexpr const char *kKfLuaPetStageProofScriptSource = R"lua(
local kStageIndex = { egg = 0, baby = 1, child = 2, teen = 3, adult = 4 }

function on_frame(dt_ms)
    local idx = kStageIndex[pet.stage()]
    kf.report(idx * 100000 + pet.teen_form() * 10000 + pet.adult_branch() * 1000 +
               pet.base_trait() * 10 + pet.dominant_care_trait())
end

kf.log("pet stage proof script loaded")
)lua";

inline constexpr const char *kKfLuaPetStageProofScriptChunkName =
    "=pet_stage_proof_script";

inline constexpr const char *kKfLuaPetMessProofScriptSource = R"lua(
function on_frame(dt_ms)
    kf.report(pet.poops() * 1000000 + pet.dirtiness())
end

kf.log("pet mess proof script loaded")
)lua";

inline constexpr const char *kKfLuaPetMessProofScriptChunkName =
    "=pet_mess_proof_script";

inline constexpr const char *kKfLuaPetCleanProofScriptSource = R"lua(
function on_frame(dt_ms)
    pet.clean()
    kf.report(pet.poops())
end

kf.log("pet clean proof script loaded")
)lua";

inline constexpr const char *kKfLuaPetCleanProofScriptChunkName =
    "=pet_clean_proof_script";

#endif /* KF_LUA_PET_PROOF_SCRIPT_H */
