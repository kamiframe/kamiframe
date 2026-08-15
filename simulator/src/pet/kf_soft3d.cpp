/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * The software 3D rasteriser. See kf_soft3d.h for why it lives here rather
 * than in hakoniwaos/, and for the two constraints that shaped it (no heap,
 * no depth buffer).
 *
 * WORKING SET, counted rather than assumed, for KF_SOFT3D_MESH_VERTS = 146
 * and KF_SOFT3D_MESH_TRIS = 288:
 *
 *     view-space x/y/z      146 * 3 * 4 =  1,752 bytes
 *     screen x/y            146 * 2 * 4 =  1,168 bytes
 *     per-triangle colour   288 * 2     =    576 bytes
 *     per-triangle depth    288 * 4     =  1,152 bytes
 *     draw order + counting sort                ~1,700 bytes
 *                                        ----------------
 *                                          ~6.4 KB static
 *
 * That is 2% of kf/budget.h's internal SRAM pool and none of any arena. A
 * full-resolution Z-buffer, for contrast, would have been 153,600 bytes, and
 * the pool has roughly 86KB unclaimed after the framebuffer and scratch. The
 * choice was not close.
 */

#include "kf_soft3d.h"

#include "kf/blit.h"
#include "kf/budget.h"
#include "kf/framebuffer.h"

#include "kf_soft3d_mesh.h"

#include <cmath>
#include <cstdint>

namespace {

/* Camera. The model is roughly unit radius (the generator divides through by
 * its largest component), sitting at +kCameraZ along the view axis, so the
 * near face is at kCameraZ - 1 and the far face at kCameraZ + 1. */
constexpr float kCameraZ = 3.4f;

/* How much of the shorter viewport axis the model fills. Below 1.0 leaves a
 * margin, which matters here because the near side of a perspective
 * projection is magnified and the ears stick out past the unit radius. */
constexpr float kFitMargin = 0.72f;

/* One directional light, in VIEW space, pointing from the upper left towards
 * the camera. Fixed in view space rather than model space on purpose: the
 * model is what spins, so a view-space light is what makes the shading sweep
 * across the faces as it turns. Pre-normalised. */
constexpr float kLightX = -0.4243f;
constexpr float kLightY = 0.5657f;
constexpr float kLightZ = -0.7071f;

/* The live light direction. Starts at the constants above and stays there
 * unless a caller moves it, so every existing measurement is unaffected.
 *
 * Exists because a fixed light on a spinning object is ambiguous to look at:
 * you cannot tell a rotating solid from a flat shape whose colours happen to
 * be cycling. Moving the light while the object holds still resolves it
 * instantly -- the highlight travels ACROSS a surface, which only a surface
 * can do. That is a judgement about whether this reads as 3D to a person,
 * which no timing number can answer. */
float g_light_x = kLightX;
float g_light_y = kLightY;
float g_light_z = kLightZ;

/* Ambient floor, so a face turned fully away from the light is dark but still
 * readable against the background rather than a black hole. */
constexpr float kAmbient = 0.22f;

/* The creature's base colour, before shading. */
constexpr int kBaseR = 150;
constexpr int kBaseG = 220;
constexpr int kBaseB = 170;

/* Depth-sort buckets. A counting sort, not a comparison sort: 288 triangles
 * through an insertion sort is up to ~41,000 comparisons in the worst case,
 * which on a 240MHz core is real time spent doing nothing visible. A counting
 * sort is one pass to bucket and one pass to emit, and the only thing lost is
 * the ordering WITHIN a bucket -- at 256 buckets across a 2-unit depth range
 * that is a bucket every ~8 thousandths of a model radius, far finer than any
 * ordering error the eye could find on a near-convex blob. */
constexpr int kDepthBuckets = 256;

/* All of it static, none of it heap, sizes fixed by the mesh at compile time.
 * Not on the stack either: ESP-IDF's default task stacks are a few KB and
 * 6KB of locals in one frame is exactly the kind of thing that works on
 * desktop and overflows on device. */
float g_vx[KF_SOFT3D_MESH_VERTS];
float g_vy[KF_SOFT3D_MESH_VERTS];
float g_vz[KF_SOFT3D_MESH_VERTS];
float g_sx[KF_SOFT3D_MESH_VERTS];
float g_sy[KF_SOFT3D_MESH_VERTS];

kf_color g_tri_color[KF_SOFT3D_MESH_TRIS];
uint8_t g_tri_bucket[KF_SOFT3D_MESH_TRIS];
uint16_t g_visible[KF_SOFT3D_MESH_TRIS]; /* indices of surviving triangles */
uint16_t g_order[KF_SOFT3D_MESH_TRIS];   /* the same, depth-sorted */
uint16_t g_bucket_count[kDepthBuckets + 1];

uint32_t g_visible_count = 0;
uint32_t g_submitted = 0;

kf_color shade_color(float lambert) {
    const float s = kAmbient + (1.0f - kAmbient) * lambert;
    const int r = static_cast<int>(static_cast<float>(kBaseR) * s);
    const int g = static_cast<int>(static_cast<float>(kBaseG) * s);
    const int b = static_cast<int>(static_cast<float>(kBaseB) * s);
    return KF_RGB(static_cast<uint8_t>(r > 255 ? 255 : r),
                  static_cast<uint8_t>(g > 255 ? 255 : g),
                  static_cast<uint8_t>(b > 255 ? 255 : b));
}

/* One flat-shaded triangle, scanline filled, clipped to `clip`.
 *
 * Edges are stepped in float rather than fixed point. The S3's FPU is real
 * hardware for single precision, this is three divisions and two multiplies
 * per scanline against a span that is typically tens of pixels wide, and the
 * INNER loop -- the part that actually dominates -- is an integer store loop
 * with no arithmetic in it at all. Fixed-point edge stepping would have
 * bought a rounding-bug surface in exchange for very little. */
void fill_triangle(float ax, float ay, float bx, float by, float cx, float cy,
                   kf_color color, const kf_rect &clip, kf_soft3d_stats *stats) {
    /* Sort the three corners by y, so the long edge always runs a->c. */
    if (ay > by) {
        float t = ax; ax = bx; bx = t;
        t = ay; ay = by; by = t;
    }
    if (by > cy) {
        float t = bx; bx = cx; cx = t;
        t = by; by = cy; cy = t;
    }
    if (ay > by) {
        float t = ax; ax = bx; bx = t;
        t = ay; ay = by; by = t;
    }

    const float height = cy - ay;
    if (height <= 0.0f) {
        return; /* zero-height triangle: nothing to fill */
    }

    const float d_ac = (cx - ax) / height;
    const float d_ab = (by > ay) ? (bx - ax) / (by - ay) : 0.0f;
    const float d_bc = (cy > by) ? (cx - bx) / (cy - by) : 0.0f;

    int y_start = static_cast<int>(ay + 1.0f);
    int y_end = static_cast<int>(cy + 1.0f);
    if (y_start < clip.y0) {
        y_start = clip.y0;
    }
    if (y_end > clip.y1) {
        y_end = clip.y1;
    }

    kf_color *fb = kf_fb_pixels();

    for (int y = y_start; y < y_end; ++y) {
        const float fy = static_cast<float>(y);

        /* The long edge spans the whole triangle; the other side switches
         * from the a->b edge to the b->c edge at the middle vertex. */
        const float x_long = ax + (fy - ay) * d_ac;
        const float x_short =
            (fy < by) ? (ax + (fy - ay) * d_ab) : (bx + (fy - by) * d_bc);

        float left = x_long < x_short ? x_long : x_short;
        float right = x_long < x_short ? x_short : x_long;

        int x_start = static_cast<int>(left + 1.0f);
        int x_end = static_cast<int>(right + 1.0f);
        if (x_start < clip.x0) {
            x_start = clip.x0;
        }
        if (x_end > clip.x1) {
            x_end = clip.x1;
        }
        if (x_end <= x_start) {
            continue;
        }

        kf_color *row = fb + static_cast<size_t>(y) * KF_DISPLAY_WIDTH;
        for (int x = x_start; x < x_end; ++x) {
            row[x] = color;
        }
        stats->pixels_written += static_cast<uint32_t>(x_end - x_start);
        stats->spans++;
    }
}

} // namespace

void kf_soft3d_set_light(float x, float y, float z) {
    /* Normalised here rather than trusted from the caller: the Lambert term
     * is a dot product against this vector, so a non-unit direction silently
     * scales every face's brightness and reads as "the light got dimmer"
     * rather than "the caller passed a bad vector". A zero-length vector
     * falls back to the default instead of dividing by zero -- an unlit
     * black object is a worse answer than an unmoved light. */
    const float len = std::sqrt(x * x + y * y + z * z);
    if (len < 1e-6f) {
        g_light_x = kLightX;
        g_light_y = kLightY;
        g_light_z = kLightZ;
        return;
    }
    g_light_x = x / len;
    g_light_y = y / len;
    g_light_z = z / len;
}

uint32_t kf_soft3d_mesh_triangle_count(void) {
    return static_cast<uint32_t>(KF_SOFT3D_MESH_TRIS);
}

void kf_soft3d_transform(kf_rect viewport, float yaw_deg, float pitch_deg) {
    g_visible_count = 0;
    g_submitted = 0;

    const int vw = viewport.x1 - viewport.x0;
    const int vh = viewport.y1 - viewport.y0;
    if (vw <= 0 || vh <= 0) {
        return;
    }

    const float cx = static_cast<float>(viewport.x0) + static_cast<float>(vw) * 0.5f;
    const float cy = static_cast<float>(viewport.y0) + static_cast<float>(vh) * 0.5f;

    /* Focal length in pixels, chosen so the model fills kFitMargin of the
     * shorter viewport axis at the camera distance above. This is what makes
     * one renderer serve both a 120x120 pet window and a 240x320 full screen
     * without a second set of constants. */
    const float half = (vw < vh ? static_cast<float>(vw) : static_cast<float>(vh)) * 0.5f;
    const float focal = half * kFitMargin * kCameraZ;

    const float yaw = yaw_deg * 0.017453292f;
    const float pitch = pitch_deg * 0.017453292f;
    const float sy = std::sin(yaw), cyaw = std::cos(yaw);
    const float sp = std::sin(pitch), cp = std::cos(pitch);

    for (int i = 0; i < KF_SOFT3D_MESH_VERTS; ++i) {
        const float mx = kf_soft3d_mesh_verts[i * 3 + 0];
        const float my = kf_soft3d_mesh_verts[i * 3 + 1];
        const float mz = kf_soft3d_mesh_verts[i * 3 + 2];

        /* Yaw about y, then pitch about x. */
        const float x1 = mx * cyaw + mz * sy;
        const float z1 = -mx * sy + mz * cyaw;
        const float y2 = my * cp - z1 * sp;
        const float z2 = my * sp + z1 * cp;

        /* Camera sits at the origin looking down +z, so the model is pushed
         * away from it. Nothing here can reach z <= 0 (kCameraZ is 3.4 and
         * the model radius is 1), which is why there is no near-plane clip:
         * adding one would be dead code that never runs and could never be
         * shown to work. */
        g_vx[i] = x1;
        g_vy[i] = y2;
        g_vz[i] = z2 + kCameraZ;

        const float inv_z = 1.0f / g_vz[i];
        g_sx[i] = cx + focal * x1 * inv_z;
        /* Screen y grows downward, model y grows up. */
        g_sy[i] = cy - focal * y2 * inv_z;
    }

    /* Bucket counts are rebuilt every frame; zero them first. */
    for (int b = 0; b <= kDepthBuckets; ++b) {
        g_bucket_count[b] = 0;
    }

    const float min_z = kCameraZ - 1.15f;
    const float depth_span = 2.30f;
    const float bucket_scale = static_cast<float>(kDepthBuckets - 1) / depth_span;

    for (int t = 0; t < KF_SOFT3D_MESH_TRIS; ++t) {
        g_submitted++;

        const int i0 = kf_soft3d_mesh_indices[t * 3 + 0];
        const int i1 = kf_soft3d_mesh_indices[t * 3 + 1];
        const int i2 = kf_soft3d_mesh_indices[t * 3 + 2];

        /* Back-face cull, done on the PROJECTED triangle rather than on a
         * model-space normal: the signed area of the screen-space triangle is
         * the perspective-correct test, and it costs two subtractions and two
         * multiplies.
         *
         * A FRONT FACE HAS POSITIVE SIGNED AREA HERE, and that sign was
         * derived rather than guessed -- the first version had it backwards
         * AND had a mesh generator emitting inward normals, which cancelled
         * far enough to produce a picture that was recognisably a blob and
         * completely wrong (100 of 288 triangles surviving, almost every
         * face unlit). The derivation: the mesh's cross(v1-v0, v2-v0) points
         * OUTWARD (asserted over all 288 triangles when the mesh was
         * generated), rotation preserves that, the camera sits at the origin
         * looking down +z so a front face's outward normal has negative z,
         * and the projection flips screen y. Work a unit triangle through
         * that and the front-facing case comes out positive. */
        const float e1x = g_sx[i1] - g_sx[i0];
        const float e1y = g_sy[i1] - g_sy[i0];
        const float e2x = g_sx[i2] - g_sx[i0];
        const float e2y = g_sy[i2] - g_sy[i0];
        const float area = e1x * e2y - e2x * e1y;
        if (area <= 0.0f) {
            continue;
        }

        /* Off-viewport reject, on the triangle's bounding box. Cheap, and it
         * is what keeps a 120x120 window from paying for geometry that lands
         * outside it. */
        float minx = g_sx[i0], maxx = g_sx[i0];
        float miny = g_sy[i0], maxy = g_sy[i0];
        if (g_sx[i1] < minx) minx = g_sx[i1];
        if (g_sx[i1] > maxx) maxx = g_sx[i1];
        if (g_sx[i2] < minx) minx = g_sx[i2];
        if (g_sx[i2] > maxx) maxx = g_sx[i2];
        if (g_sy[i1] < miny) miny = g_sy[i1];
        if (g_sy[i1] > maxy) maxy = g_sy[i1];
        if (g_sy[i2] < miny) miny = g_sy[i2];
        if (g_sy[i2] > maxy) maxy = g_sy[i2];
        if (maxx <= static_cast<float>(viewport.x0) ||
            minx >= static_cast<float>(viewport.x1) ||
            maxy <= static_cast<float>(viewport.y0) ||
            miny >= static_cast<float>(viewport.y1)) {
            continue;
        }

        /* Lambert against the one directional light, using the VIEW-space
         * face normal. Computed here rather than interpolated per pixel:
         * this is flat shading, which is one normalise per triangle instead
         * of one per fragment, and on a low-poly blob it reads as faceted
         * rather than wrong. */
        const float ax = g_vx[i1] - g_vx[i0];
        const float ay = g_vy[i1] - g_vy[i0];
        const float az = g_vz[i1] - g_vz[i0];
        const float bx = g_vx[i2] - g_vx[i0];
        const float by = g_vy[i2] - g_vy[i0];
        const float bz = g_vz[i2] - g_vz[i0];
        float nx = ay * bz - az * by;
        float ny = az * bx - ax * bz;
        float nz = ax * by - ay * bx;
        const float len2 = nx * nx + ny * ny + nz * nz;
        float lambert = 0.0f;
        if (len2 > 0.0f) {
            const float inv_len = 1.0f / std::sqrt(len2);
            nx *= inv_len;
            ny *= inv_len;
            nz *= inv_len;
            /* The screen-space area test above already established this
             * triangle faces the camera, so this outward normal is oriented
             * towards -z and no second orientation test is needed. That is
             * an assumption worth naming rather than trusting silently: it
             * is exactly what was wrong the first time, and the symptom was
             * a creature lit from inside itself -- every face at the ambient
             * floor, which still passes any timing check. The shade-count
             * assertion in headless_main.cpp's stress_run_3d() is what now
             * makes that failure loud. */
            lambert = nx * g_light_x + ny * g_light_y + nz * g_light_z;
            if (lambert < 0.0f) {
                lambert = 0.0f;
            }
        }

        const float avg_z = (g_vz[i0] + g_vz[i1] + g_vz[i2]) * 0.3333333f;
        float bucket_f = (avg_z - min_z) * bucket_scale;
        if (bucket_f < 0.0f) {
            bucket_f = 0.0f;
        }
        int bucket = static_cast<int>(bucket_f);
        if (bucket > kDepthBuckets - 1) {
            bucket = kDepthBuckets - 1;
        }

        g_tri_color[g_visible_count] = shade_color(lambert);
        g_tri_bucket[g_visible_count] = static_cast<uint8_t>(bucket);
        g_visible[g_visible_count] = static_cast<uint16_t>(t);
        g_visible_count++;
        g_bucket_count[bucket]++;
    }

    /* Counting sort, emitted FAR bucket first: painter's algorithm draws back
     * to front, and larger view-space z is further away. Prefix sums run from
     * the top bucket down so the highest bucket lands at offset 0. */
    uint16_t running = 0;
    for (int b = kDepthBuckets - 1; b >= 0; --b) {
        const uint16_t n = g_bucket_count[b];
        g_bucket_count[b] = running;
        running = static_cast<uint16_t>(running + n);
    }
    for (uint32_t v = 0; v < g_visible_count; ++v) {
        const int bucket = g_tri_bucket[v];
        g_order[g_bucket_count[bucket]] = static_cast<uint16_t>(v);
        g_bucket_count[bucket]++;
    }
}

void kf_soft3d_rasterize(kf_rect viewport, kf_soft3d_stats *out) {
    kf_soft3d_stats stats = {};
    stats.tris_submitted = g_submitted;
    stats.tris_drawn = g_visible_count;

    kf_rect clip = viewport;
    if (clip.x0 < 0) clip.x0 = 0;
    if (clip.y0 < 0) clip.y0 = 0;
    if (clip.x1 > KF_DISPLAY_WIDTH) clip.x1 = static_cast<int16_t>(KF_DISPLAY_WIDTH);
    if (clip.y1 > KF_DISPLAY_HEIGHT) clip.y1 = static_cast<int16_t>(KF_DISPLAY_HEIGHT);

    for (uint32_t k = 0; k < g_visible_count; ++k) {
        const uint16_t v = g_order[k];
        const uint16_t t = g_visible[v];
        const int i0 = kf_soft3d_mesh_indices[t * 3 + 0];
        const int i1 = kf_soft3d_mesh_indices[t * 3 + 1];
        const int i2 = kf_soft3d_mesh_indices[t * 3 + 2];
        fill_triangle(g_sx[i0], g_sy[i0], g_sx[i1], g_sy[i1], g_sx[i2],
                      g_sy[i2], g_tri_color[v], clip, &stats);
    }

    /* One rectangle for the whole window. Every pixel of it is being
     * repainted, so a tighter set of rectangles would only add
     * KF_DISPLAY_RECT_OVERHEAD_BYTES of addressing for no fewer pixels.
     *
     * Charged to the OPAQUE draw-counter bucket, not the keyed one. kf/blit.h
     * is explicit that the bucket reflects cost SHAPE rather than whether a
     * colour key is involved: the span loop above is a run of stores of one
     * constant with no per-pixel test, which is the same shape as
     * kf_fill_rect()'s rows. The per-triangle setup this bucket does NOT
     * cover is the whole reason kf_soft3d_transform() is a separate,
     * separately-timeable call. */
    if (clip.x1 > clip.x0 && clip.y1 > clip.y0) {
        kf_fb_mark_dirty(clip);
    }
    kf_draw_count_pixels(false, stats.pixels_written);

    if (out != nullptr) {
        *out = stats;
    }
}

void kf_soft3d_render(kf_rect viewport, float yaw_deg, float pitch_deg,
                      kf_soft3d_stats *out) {
    kf_soft3d_transform(viewport, yaw_deg, pitch_deg);
    kf_soft3d_rasterize(viewport, out);
}
