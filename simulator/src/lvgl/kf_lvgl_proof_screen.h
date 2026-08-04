/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * PLACEHOLDER CONTENT, same spirit as kf/demo.h: this is "LVGL draws
 * something and it is provably deterministic," not a menu design. There is
 * nothing to put in a real menu yet -- see ADR 0013. Do not build on top of
 * this; delete it once real menu screens exist.
 */

#ifndef KF_LVGL_PROOF_SCREEN_H
#define KF_LVGL_PROOF_SCREEN_H

/* One label and one bar on LVGL's active screen, added to the input group
 * so the keypad bridge has something to prove it can reach. Call once,
 * after kf_lvgl_port_init(). */
void kf_lvgl_proof_screen_init(void);

#endif /* KF_LVGL_PROOF_SCREEN_H */
