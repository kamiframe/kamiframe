/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * See kf_error_banner.h.
 */

#include "kf_error_banner.h"

#include "../../sdk/lua/kf_lua_port.h"

#include "kf/types.h"

#include <cstdio>

namespace {
constexpr kf_color kBannerFg = KF_WHITE;
constexpr kf_color kBannerBg = KF_RGB(180, 24, 24);
} // namespace

kf_scene_id kf_error_banner_create(void) {
    const kf_scene_id id = kf_scene_add_text("");
    kf_scene_set_pos(id, 0, KF_ERROR_BANNER_Y);
    kf_scene_set_colors(id, kBannerFg, kBannerBg);
    kf_scene_set_visible(id, false);
    return id;
}

void kf_error_banner_update(kf_scene_id id) {
    if (!kf_lua_port_disabled_after_error()) {
        kf_scene_set_visible(id, false);
        return;
    }
    /* KF_SCENE_TEXT_MAX is 40 -- one full 240px row -- so the message is
     * truncated to fit alongside the fixed "ERROR: " prefix; kf_scene_set_
     * text() would truncate it anyway (and log once, kf/scene.h's own
     * comment) but doing it here keeps the fixed prefix from being the
     * part that gets cut. */
    char line[48];
    std::snprintf(line, sizeof(line), "ERROR: %s", kf_lua_port_last_error());
    kf_scene_set_visible(id, true);
    kf_scene_set_text(id, line);
}
