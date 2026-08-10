/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * One accessor, and the reason it exists is entirely in app_main.cpp's own
 * comment where it is measured -- read that first. Split into its own
 * header, rather than folded into kf_dbg_bridge.h, because the two files
 * have opposite roles here: kf_dbg_bridge.h is the bridge exposing state TO
 * app_main.cpp (kf_dbg_input_mask(), kf_dbg_time_multiplier()); this is
 * app_main.cpp exposing state TO the bridge, so kf_dbg_bridge.cpp's
 * handle_state() can serialise it into KFDBG STATE's JSON (ADR 0036). */

#ifndef KF_APP_POST_FRAME_H
#define KF_APP_POST_FRAME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Microseconds the most recently completed loop iteration spent on the work
 * a PORT does after kf_app_frame() returns -- kf_pet_session_frame(),
 * kf_screen_nav_frame(), the conditional kf_lvgl_port_pump(), kf_lua_port_
 * frame() -- measured in app_main.cpp's own loop, around exactly that
 * segment. 0 before the first iteration completes.
 *
 * Deliberately NOT part of kf_frame_stats (kf/app.h): that struct is Core's
 * accounting of what happens INSIDE kf_app_frame(), and cpu_us there keeps
 * meaning exactly what it means today. This segment only exists because a
 * port chose to draw outside kf_app_frame() -- Core has no way to know that
 * happened, let alone time it -- so the measurement belongs here, one layer
 * up. Task 7 of the hardware bring-up plan reads this alongside cpu_us to
 * see the two separately rather than folded into one number that would
 * answer neither question precisely. */
uint32_t kf_app_post_frame_us(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KF_APP_POST_FRAME_H */
