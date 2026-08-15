/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * A minimal software 3D rasteriser, to answer one measured question: could
 * this device run a 3D virtual pet?
 *
 * WHY THIS LIVES HERE AND NOT IN hakoniwaos/. Core is heap-free AND
 * float-free, enforced by tools/check_no_heap.py and tools/check_no_float.py
 * (both scan hakoniwaos/src and hakoniwaos/include only). A perspective
 * divide and a Lambert dot product are floating point by nature -- doing them
 * in fixed point would be a second design exercise on top of this one, and
 * would make the measurement answer a question nobody asked. So this file
 * sits in simulator/src/pet/ and is compiled into BOTH builds by relative
 * path, exactly the way kf_frame_loop.cpp already is: simulator/CMakeLists.txt
 * lists it in kamiframe_pet_port, and ports/esp32/main/CMakeLists.txt lists
 * the same single source file in its SRCS. One canonical copy, not a fork.
 * The ESP32-S3 has a single-precision FPU, so `float` here is real hardware
 * arithmetic on the target; `double` would be software-emulated and is
 * avoided throughout (-Wdouble-promotion is on and warnings are errors).
 *
 * NO Z-BUFFER, AND THAT IS A DELIBERATE CHOICE, NOT AN OMISSION. A
 * full-resolution depth buffer is 240*320*2 = 153,600 bytes, the same size as
 * the framebuffer itself, and kf/budget.h's internal-SRAM pool is 320KB total
 * with the framebuffer (153,600) and scratch (49,152) already carved out of
 * it. There is nowhere to put one. This uses PAINTER'S ALGORITHM instead:
 * back-face culling first, then triangles drawn back to front by their
 * average view-space depth. That is exactly correct for a convex object and
 * very close to correct for the near-convex blob in kf_soft3d_mesh.h; it
 * would need revisiting for a creature with, say, a limb crossing in front of
 * its own body.
 *
 * NO HEAP. Every buffer here is a fixed-size static array sized from the
 * mesh's own compile-time vertex and triangle counts. Total working set is
 * under 8KB (see kf_soft3d.cpp's own accounting comment) -- small enough to
 * sit in internal SRAM without disturbing any arena in kf/budget.h.
 */

#ifndef KF_SOFT3D_H
#define KF_SOFT3D_H

#include "kf/types.h"

#include <cstdint>

/* What one render actually cost, in units that do not depend on the host.
 *
 * `pixels_written` is the number that feeds kf/budget.h's device draw-time
 * model the same way kf/blit.h's counters do for every other screen in this
 * project. `tris_submitted` minus `tris_drawn` is how much the back-face and
 * off-screen culls actually removed -- worth reporting, because a cull that
 * silently stopped working would show up as a triangle throughput figure that
 * looks fine and a picture that does not. */
typedef struct {
    uint32_t tris_submitted;
    uint32_t tris_drawn;
    uint32_t pixels_written;
    uint32_t spans;
} kf_soft3d_stats;

/* Geometry: rotate the mesh, project it into `viewport`, cull back faces and
 * anything entirely off-viewport, shade each surviving triangle against one
 * directional light, and depth-sort what is left. Writes nothing to the
 * framebuffer. Split out from the raster below so a caller can time the two
 * halves apart -- per-triangle setup cost and per-pixel fill cost scale with
 * completely different things (triangle count versus window area), and a
 * verdict about 3D on this hardware depends on knowing which one dominates.
 *
 * Angles are in degrees. Yaw spins about the model's +y axis, pitch about
 * its +x. */
void kf_soft3d_transform(kf_rect viewport, float yaw_deg, float pitch_deg);

/* Raster: fill the triangles the last kf_soft3d_transform() left behind, back
 * to front, clipped to `viewport`. Marks `viewport` dirty as a single
 * rectangle -- the whole window is being repainted, so one rectangle is both
 * correct and the cheapest thing to send. */
void kf_soft3d_rasterize(kf_rect viewport, kf_soft3d_stats *out);

/* Both halves, for callers that do not need them timed apart. */
void kf_soft3d_render(kf_rect viewport, float yaw_deg, float pitch_deg,
                      kf_soft3d_stats *out);

/* Moves the one directional light, in VIEW space. Normalised internally; a
 * zero-length vector leaves the light where it was.
 *
 * A fixed light on a spinning object is genuinely ambiguous to look at -- a
 * rotating solid and a flat shape with cycling colours produce a similar
 * impression. Moving the light while the object is still resolves it at
 * once, because a highlight travelling ACROSS a surface is something only a
 * surface does. Whether this reads as 3D to a person is the one question the
 * timing numbers cannot answer, so the knob exists. */
void kf_soft3d_set_light(float x, float y, float z);

/* The mesh's triangle count, so a caller can report throughput without
 * including the generated mesh header itself. */
uint32_t kf_soft3d_mesh_triangle_count(void);

#endif /* KF_SOFT3D_H */
