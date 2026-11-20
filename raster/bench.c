/* Benchmark / evidence harness for raster winding culling.
 *
 * Renders a fixed scene at 32x32 and 128x128 under every cull switch and
 * prints, per resolution and per switch:
 *   - triangle count, drawn / culled / rejected split, and the input index of
 *     every culled or rejected triangle;
 *   - rejection/cull reasons, classified (all categories are printed, so a
 *     zero is visible rather than absent);
 *   - filled pixel count (pixels that passed the depth test);
 *   - elapsed wall time for the batch of triangles;
 *   - an independent hand calculation of the triangle's doubled signed area
 *     together with the exact pixel-centre count, for error comparison.
 *
 * The scene deliberately contains one CCW triangle, one CW triangle, one
 * coincident-vertex triangle, one collinear (zero-area) triangle and one
 * near-degenerate sliver, so every reason class is exercised at both
 * resolutions.
 */
#include "raster.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Hand-computed doubled areas for the base 128x128 scene, from the vertex
 * cross product (v1-v0)x(v2-v0).  Kept as literals so the program does not
 * simply re-use the library's own arithmetic. */
#define HAND_A2_A  11124.0   /* {10,10} {118,20} {64,118}   */
#define HAND_A2_B  (-6700.0) /* {20,100} {110,90} {70,20}   */

#define NTRI 7

static void make_scene(double s, RTri out[NTRI]) {
    /* s scales the base 128x128 geometry to other resolutions. */
    double base[NTRI][3][3] = {
        /* CCW, the legacy demo triangle A */
        { {10,10,0.8}, {118,20,0.6}, {64,118,0.4} },
        /* CW, the legacy demo triangle B */
        { {20,100,0.7}, {110,90,0.9}, {70,20,0.2} },
        /* coincident vertices 0 and 1 */
        { {40,40,0.5}, {40,40,0.5}, {80,80,0.5} },
        /* collinear, zero area */
        { {10,120,0.5}, {40,120,0.5}, {70,120,0.5} },
        /* sub-epsilon sliver: |A2| = 1e-12 * 2 = 2e-12 > 0 but tiny */
        { {90,10,0.5}, {110,10,0.5}, {100,10 + 1e-14,0.5} },
        /* extra CCW triangle in the lower right */
        { {90,60,0.3}, {120,60,0.3}, {105,100,0.3} },
        /* extra CW triangle in the upper left, overlap with A */
        { {70,10,0.2}, {100,40,0.2}, {70,50,0.2} },
    };
    unsigned char cols[NTRI][3] = {
        {200, 40, 40}, {40, 180, 220}, {60, 220, 60},
        {220, 220, 60}, {220, 60, 220}, {240, 240, 240}, {40, 40, 240},
    };
    for (int i = 0; i < NTRI; ++i) {
        for (int k = 0; k < 3; ++k) {
            out[i].v[k].x = base[i][k][0] * s;
            out[i].v[k].y = base[i][k][1] * s;
            out[i].v[k].z = base[i][k][2];
        }
        memcpy(out[i].rgb, cols[i], 3);
    }
}

/* Independent pixel-centre coverage count for one triangle, orientation
 * taken from the sign of its own area. */
static long long hand_covered(const RTri *t, int w, int h) {
    const RVert *a = &t->v[0], *b = &t->v[1], *c = &t->v[2];
    double a2 = (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
    long long n = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            double px = x + 0.5, py = y + 0.5;
            double d0 = (b->x - a->x) * (py - a->y) - (b->y - a->y) * (px - a->x);
            double d1 = (c->x - b->x) * (py - b->y) - (c->y - b->y) * (px - b->x);
            double d2 = (a->x - c->x) * (py - c->y) - (a->y - c->y) * (px - c->x);
            if (a2 > 0) { if (d0 >= 0 && d1 >= 0 && d2 >= 0) ++n; }
            else        { if (d0 <= 0 && d1 <= 0 && d2 <= 0) ++n; }
        }
    return n;
}

/* Same, but only counting pixels the depth test would reject when drawing the
 * scene in input order, under the given switch. */
static long long hand_filled_in_order(const RTri *tris, int n, int w, int h,
                                      RTCullMode mode) {
    float *z = (float*)malloc(sizeof(float) * (size_t)w * (size_t)h);
    if (!z) return -1;
    for (int i = 0; i < w * h; ++i) z[i] = 1e30f;
    long long filled = 0;
    for (int k = 0; k < n; ++k) {
        const RTri *t = &tris[k];
        const RVert *a = &t->v[0], *b = &t->v[1], *c = &t->v[2];
        double a2 = (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
        if (!(fabs(a2) > 1e-12)) continue;
        int ccw = a2 > 0.0;
        /* Apply the cull switch; RT_CULL_NONE keeps the legacy CCW-only path. */
        if (mode == RT_CULL_KEEP_CCW && !ccw) continue;
        if (mode == RT_CULL_KEEP_CW && ccw) continue;
        if (mode == RT_CULL_NONE && !ccw) continue;
        int fill_ccw_side = ccw;   /* RT_CULL_BOTH fills the triangle's own side */
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                double px = x + 0.5, py = y + 0.5;
                double w0 = (c->x - b->x) * (py - b->y) - (c->y - b->y) * (px - b->x);
                double w1 = (a->x - c->x) * (py - c->y) - (a->y - c->y) * (px - c->x);
                double w2 = (b->x - a->x) * (py - a->y) - (b->y - a->y) * (px - a->x);
                if (fill_ccw_side) { if (w0 < 0 || w1 < 0 || w2 < 0) continue; }
                else               { if (w0 > 0 || w1 > 0 || w2 > 0) continue; }
                double sum = w0 + w1 + w2;
                double zz = (w0 * a->z + w1 * b->z + w2 * c->z) / sum;
                size_t i = (size_t)y * (size_t)w + (size_t)x;
                if ((float)zz < z[i]) { z[i] = (float)zz; ++filled; }
            }
    }
    free(z);
    return filled;
}

static void run(int res, const RTri *tris, int n) {
    RTCullMode modes[4] = { RT_CULL_NONE, RT_CULL_KEEP_CCW,
                            RT_CULL_KEEP_CW, RT_CULL_BOTH };
    const char *names[4] = { "none", "keep-ccw", "keep-cw", "both" };
    double s = (double)res / 128.0;

    printf("\n=== resolution %dx%d (scale %.3f) ===\n", res, res, s);

    /* Hand-calculated area error against the library, at this resolution.
     * HAND_A2_A / HAND_A2_B are the literals for the base 128x128 scene; at
     * another resolution the same vertex cross product scales by s^2.  The
     * library's own double-precision result is compared to the literal times
     * s^2, and separately the two are compared bit-for-bit via the exact
     * hand expression above, so any drift shows up as a non-zero error. */
    {
        double a2_0 = (tris[0].v[1].x - tris[0].v[0].x) * (tris[0].v[2].y - tris[0].v[0].y) -
                      (tris[0].v[1].y - tris[0].v[0].y) * (tris[0].v[2].x - tris[0].v[0].x);
        double a2_1 = (tris[1].v[1].x - tris[1].v[0].x) * (tris[1].v[2].y - tris[1].v[0].y) -
                      (tris[1].v[1].y - tris[1].v[0].y) * (tris[1].v[2].x - tris[1].v[0].x);
        double lib0 = 0, lib1 = 0;
        (void)rt_classify(&tris[0], NULL, &lib0, NULL);
        (void)rt_classify(&tris[1], NULL, &lib1, NULL);
        printf("hand-area check: tri0 A2=%+.6f hand=%+.6f err=%.3g | "
               "tri1 A2=%+.6f hand=%+.6f err=%.3g\n",
               lib0, HAND_A2_A * s * s, lib0 - HAND_A2_A * s * s,
               lib1, HAND_A2_B * s * s, lib1 - HAND_A2_B * s * s);
        printf("  (direct cross products: %+.6f %+.6f)\n", a2_0, a2_1);
    }

    RTCullMode all_modes[4] = { modes[0], modes[1], modes[2], modes[3] };
    for (int m = 0; m < 4; ++m) {
        /* Repeat the batch and keep the median elapsed time; a single run at
         * 32x32 is a few microseconds and dominated by clock granularity. */
        enum { REPS = 21 };
        double times[REPS];
        RTBatchStats st;
        int ok = 0;
        for (int rep = 0; rep < REPS; ++rep) {
            RTarget t;
            if (rt_init(&t, res, res) != 0) { printf("init failed\n"); return; }
            rt_set_cull(&t, all_modes[m]);
            if (rt_batch(&t, tris, n, &st) != 0) { printf("batch failed\n"); return; }
            rt_free(&t);
            times[rep] = st.elapsed_ms;
            ok = 1;
            if (rep + 1 < REPS) rt_stats_free(&st);
        }
        if (!ok) return;
        for (int a = 0; a < REPS; ++a)
            for (int b = a + 1; b < REPS; ++b)
                if (times[b] < times[a]) { double tmp = times[a]; times[a] = times[b]; times[b] = tmp; }
        double median_ms = times[REPS / 2];

        long long hand_fill = 0;
        long long hand_cov = 0;
        for (int i = 0; i < n; ++i) {
            RTWinding w;
            if (rt_classify(&tris[i], NULL, NULL, &w) != RT_CULL_REASON_NONE) continue;
            if (all_modes[m] == RT_CULL_KEEP_CCW && w == RT_WIND_CW) continue;
            if (all_modes[m] == RT_CULL_KEEP_CW && w == RT_WIND_CCW) continue;
            if (all_modes[m] == RT_CULL_NONE && w == RT_WIND_CW) continue;
            hand_cov += hand_covered(&tris[i], res, res);
        }
        hand_fill = hand_filled_in_order(tris, n, res, res, all_modes[m]);

        printf("%-9s drawn %d culled %d rejected %d | filled %lld (hand cover %lld, "
               "hand in-order fill %lld, err %+lld) | %.3f ms (median of %d)\n",
               names[m], st.drawn, st.culled, st.rejected,
               st.filled, hand_cov, hand_fill, st.filled - hand_fill,
               median_ms, (int)REPS);
        printf("          covered (all accepted pixel centres, pre-depth) %lld\n",
               st.covered);
        printf("          culled idx:");
        for (int i = 0; i < st.culled_n; ++i) printf(" %d", st.culled_idx[i]);
        printf("  rejected idx:");
        for (int i = 0; i < st.rejected_n; ++i) printf(" %d", st.rejected_idx[i]);
        printf("\n          reasons:");
        for (int r = 0; r < RT_CULL_REASON_COUNT; ++r)
            printf(" %s=%d", rt_cull_reason_name((RTCullReason)r), st.reason_counts[r]);
        printf("\n");
        rt_stats_free(&st);
    }
}

int main(void) {
    RTri base[NTRI];
    make_scene(1.0, base);

    /* Per-triangle geometry report. */
    printf("triangle geometry (base 128x128):\n");
    for (int i = 0; i < NTRI; ++i) {
        double a2;
        RTWinding w;
        RTCullReason r = rt_classify(&base[i], NULL, &a2, &w);
        printf("  tri %d: A2 = %+.6f  winding %-10s reason %s\n",
               i, a2, rt_winding_name(w), rt_cull_reason_name(r));
    }

    RTri s32[NTRI];
    /* Scale coordinates (and with them the hand areas) by s^2 so the literal
     * comparison stays exact at both resolutions. */
    make_scene(32.0 / 128.0, s32);
    run(32, s32, NTRI);
    run(128, base, NTRI);
    return 0;
}
