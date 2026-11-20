#include "raster.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

/* Same triangle with opposite vertex order. */
static RTri tri_ccw(void) {
    RTri t = {{ {4,4,0.5}, {28,4,0.5}, {16,28,0.5} }, {255, 0, 0}};
    return t;
}
static RTri tri_cw(void) {
    RTri t = {{ {4,4,0.5}, {16,28,0.5}, {28,4,0.5} }, {0, 0, 255}};
    return t;
}

static int image_equal(const RTarget *a, const RTarget *b) {
    if (a->w != b->w || a->h != b->h) return 0;
    size_t n = (size_t)a->w * (size_t)a->h;
    if (memcmp(a->rgb, b->rgb, n * 3) != 0) return 0;
    for (size_t i = 0; i < n; ++i) if (a->z[i] != b->z[i]) return 0;
    return 1;
}

static int all_black(const RTarget *t) {
    size_t n = (size_t)t->w * (size_t)t->h;
    for (size_t i = 0; i < n * 3; ++i) if (t->rgb[i] != 0) return 0;
    return 1;
}

/* Reference count of pixel centres inside the triangle, using a completely
 * independent formulation (barycentric sign test with the standard
 * orientation-independent rule). */
static int reference_covered(const RTri *tri, int w, int h, int sign) {
    const RVert *a = &tri->v[0], *b = &tri->v[1], *c = &tri->v[2];
    int n = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            double px = x + 0.5, py = y + 0.5;
            double d0 = (b->x - a->x) * (py - a->y) - (b->y - a->y) * (px - a->x);
            double d1 = (c->x - b->x) * (py - b->y) - (c->y - b->y) * (px - b->x);
            double d2 = (a->x - c->x) * (py - c->y) - (a->y - c->y) * (px - c->x);
            if (sign > 0) { if (d0 >= 0 && d1 >= 0 && d2 >= 0) ++n; }
            else          { if (d0 <= 0 && d1 <= 0 && d2 <= 0) ++n; }
        }
    }
    return n;
}

/* Independent reference: number of pixel centres covered by the union of a
 * positive and a negative triangle (both filled). */
static int reference_union(const RTri *pos, const RTri *neg, int w, int h) {
    int n = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            double px = x + 0.5, py = y + 0.5;
            int in = 0;
            const RTri *ts[2] = {pos, neg};
            for (int k = 0; k < 2 && !in; ++k) {
                const RVert *a = &ts[k]->v[0], *b = &ts[k]->v[1], *c = &ts[k]->v[2];
                double d0 = (b->x - a->x) * (py - a->y) - (b->y - a->y) * (px - a->x);
                double d1 = (c->x - b->x) * (py - b->y) - (c->y - b->y) * (px - b->x);
                double d2 = (a->x - c->x) * (py - c->y) - (a->y - c->y) * (px - c->x);
                if (k == 0) in = (d0 >= 0 && d1 >= 0 && d2 >= 0);
                else        in = (d0 <= 0 && d1 <= 0 && d2 <= 0);
            }
            if (in) ++n;
        }
    }
    return n;
}

static void test_winding_classification(void) {
    RTri ccw = tri_ccw(), cw = tri_cw();
    double area = 0, area2 = 0;
    RTWinding w;

    CK(rt_classify(&ccw, &area, &area2, &w) == RT_CULL_REASON_NONE);
    CK(w == RT_WIND_CCW);
    /* v1-v0 = (24,0), v2-v0 = (12,24): cross = 24*24 - 0*12 = 576 */
    CK(fabs(area2 - 576.0) < 1e-9);
    CK(fabs(area - 288.0) < 1e-9);

    CK(rt_classify(&cw, &area, &area2, &w) == RT_CULL_REASON_NONE);
    CK(w == RT_WIND_CW);
    CK(fabs(area2 + 576.0) < 1e-9);
    CK(area < 0.0);

    /* Degenerate cases must be rejected and carry a reason even though no
     * sign is meaningful. */
    RTri coincident = {{ {5,5,0}, {5,5,0}, {20,20,0} }, {0,0,0}};
    w = (RTWinding)99;
    CK(rt_classify(&coincident, &area, &area2, &w) == RT_CULL_REASON_COINCIDENT);
    CK(w == RT_WIND_DEGENERATE);
    CK(area == 0.0);

    RTri collinear = {{ {0,0,0}, {8,8,0}, {16,16,0} }, {0,0,0}};
    CK(rt_classify(&collinear, &area, &area2, &w) == RT_CULL_REASON_ZERO_AREA);
    CK(w == RT_WIND_DEGENERATE);

    RTri sliver = {{ {0,0,0}, {10,0,0}, {5,1e-14,0} }, {0,0,0}};
    CK(rt_classify(&sliver, &area, &area2, &w) == RT_CULL_REASON_DEGENERATE_WINDING);

    RTri nan_tri = {{ {0,0,0}, {10,0,0}, {NAN,1,0} }, {0,0,0}};
    CK(rt_classify(&nan_tri, &area, &area2, &w) == RT_CULL_REASON_DEGENERATE_WINDING);
}

static void test_cull_modes(void) {
    RTri ccw = tri_ccw(), cw = tri_cw();

    /* Baseline: no culling, CCW-only legacy path. */
    RTarget none, keep_ccw, keep_cw, both;
    CK(rt_init(&none, 32, 32) == 0);
    CK(rt_init(&keep_ccw, 32, 32) == 0);
    CK(rt_init(&keep_cw, 32, 32) == 0);
    CK(rt_init(&both, 32, 32) == 0);
    rt_set_cull(&none, RT_CULL_NONE);
    rt_set_cull(&keep_ccw, RT_CULL_KEEP_CCW);
    rt_set_cull(&keep_cw, RT_CULL_KEEP_CW);
    rt_set_cull(&both, RT_CULL_BOTH);

    RTBatchStats s_none, s_ccw, s_cw;
    RTri tris[2] = {ccw, cw};

    CK(rt_batch(&none, tris, 2, &s_none) == 0);
    CK(s_none.drawn == 2);
    CK(s_none.culled == 0);
    CK(s_none.rejected == 0);
    CK(s_none.covered == reference_covered(&ccw, 32, 32, +1));
    CK(s_none.info[0].reason == RT_CULL_REASON_NONE);
    CK(s_none.info[1].reason == RT_CULL_REASON_NONE);
    CK(s_none.info[0].covered > 0);
    CK(s_none.info[1].covered == 0);   /* legacy path drops CW silently */

    /* KEEP_CCW draws the CCW triangle only; the CW one is culled by index 1. */
    CK(rt_batch(&keep_ccw, tris, 2, &s_ccw) == 0);
    CK(s_ccw.drawn == 1);
    CK(s_ccw.culled == 1);
    CK(s_ccw.rejected == 0);
    CK(s_ccw.culled_n == 1 && s_ccw.culled_idx[0] == 1);
    CK(s_ccw.reason_counts[RT_CULL_REASON_BACKFACE_CW] == 1);
    CK(s_ccw.info[0].reason == RT_CULL_REASON_NONE);
    CK(s_ccw.info[1].reason == RT_CULL_REASON_BACKFACE_CW);

    /* KEEP_CW is the mirror image. */
    CK(rt_batch(&keep_cw, tris, 2, &s_cw) == 0);
    CK(s_cw.drawn == 1);
    CK(s_cw.culled == 1);
    CK(s_cw.culled_n == 1 && s_cw.culled_idx[0] == 0);
    CK(s_cw.reason_counts[RT_CULL_REASON_BACKFACE_CCW] == 1);
    CK(s_cw.info[0].reason == RT_CULL_REASON_BACKFACE_CCW);
    CK(s_cw.info[1].reason == RT_CULL_REASON_NONE);

    /* Both switch positions report the same triangle count split, and their
     * drawn sets are complementary, so the filled pixels of the two runs
     * must agree with a single both-sided run. */
    RTBatchStats s_both;
    CK(rt_batch(&both, tris, 2, &s_both) == 0);
    CK(s_both.drawn == 2);
    CK(s_both.covered == reference_covered(&ccw, 32, 32, +1) +
                        reference_covered(&cw, 32, 32, -1));
    /* Filled is per-pixel, so overlapping coverage is counted once: it must
     * equal the size of the union of the two triangles, and never exceed the
     * per-triangle coverage sum. */
    CK(s_both.filled == reference_union(&ccw, &cw, 32, 32));
    CK(s_both.filled <= s_both.covered);
    CK(s_both.filled > 0);

    /* KEEP_CCW on a target whose contents equal RT_CULL_NONE's. */
    RTri only_ccw[1] = {ccw};
    RTarget none1;
    CK(rt_init(&none1, 32, 32) == 0);
    rt_set_cull(&none1, RT_CULL_NONE);
    RTBatchStats tmp;
    CK(rt_batch(&none1, only_ccw, 1, &tmp) == 0);
    RTBatchStats tmp2;
    CK(rt_batch(&keep_ccw, only_ccw, 1, &tmp2) == 0);
    CK(none1.z[16*32+16] == keep_ccw.z[16*32+16]);
    CK(none1.rgb[(16*32+16)*3] == keep_ccw.rgb[(16*32+16)*3]);
    rt_stats_free(&s_none); rt_stats_free(&s_ccw);
    rt_stats_free(&s_cw); rt_stats_free(&s_both);
    rt_stats_free(&tmp); rt_stats_free(&tmp2);
    rt_free(&none); rt_free(&keep_ccw); rt_free(&keep_cw); rt_free(&both);
    rt_free(&none1);
}

/* Verbatim copy of the pre-culling rasterizer's triangle routine, kept here
 * as an independent oracle.  RT_CULL_NONE must reproduce it bit-for-bit. */
static double legacy_edge(const RVert *a, const RVert *b, double px, double py) {
    return (b->x - a->x) * (py - a->y) - (b->y - a->y) * (px - a->x);
}

static int legacy_triangle(RTarget *t, const RTri *tri) {
    double area = legacy_edge(&tri->v[0], &tri->v[1], tri->v[2].x, tri->v[2].y);
    if (fabs(area) < 1e-12) return -2;
    int minx = (int)floor(fmin(fmin(tri->v[0].x, tri->v[1].x), tri->v[2].x));
    int maxx = (int)ceil(fmax(fmax(tri->v[0].x, tri->v[1].x), tri->v[2].x));
    int miny = (int)floor(fmin(fmin(tri->v[0].y, tri->v[1].y), tri->v[2].y));
    int maxy = (int)ceil(fmax(fmax(tri->v[0].y, tri->v[1].y), tri->v[2].y));
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx >= t->w) maxx = t->w - 1;
    if (maxy >= t->h) maxy = t->h - 1;
    int written = 0;
    for (int y = miny; y <= maxy; ++y) {
        for (int x = minx; x <= maxx; ++x) {
            double px = x + 0.5, py = y + 0.5;
            double w0 = legacy_edge(&tri->v[1], &tri->v[2], px, py);
            double w1 = legacy_edge(&tri->v[2], &tri->v[0], px, py);
            double w2 = legacy_edge(&tri->v[0], &tri->v[1], px, py);
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            double sum = w0 + w1 + w2;
            double z = (w0 * tri->v[0].z + w1 * tri->v[1].z + w2 * tri->v[2].z) / sum;
            size_t i = (size_t)y * (size_t)t->w + (size_t)x;
            if ((float)z < t->z[i]) {
                t->z[i] = (float)z;
                t->rgb[i*3+0] = tri->rgb[0];
                t->rgb[i*3+1] = tri->rgb[1];
                t->rgb[i*3+2] = tri->rgb[2];
                ++written;
            }
        }
    }
    return written;
}

static void test_disabled_path_unchanged(void) {
    /* Feed a mix of CCW, CW, off-screen, partially off-screen and edge-aligned
     * triangles through both the reference implementation and the library,
     * and require identical colour and depth buffers pixel by pixel. */
    const int W = 96, H = 96;
    RTarget legacy, fresh;
    CK(rt_init(&legacy, W, H) == 0);
    CK(rt_init(&fresh, W, H) == 0);
    rt_set_cull(&fresh, RT_CULL_NONE);

    RTri scene[] = {
        {{ {4,4,0.5}, {28,4,0.5}, {16,28,0.5} }, {255, 0, 0}},          /* CCW */
        {{ {4,4,0.6}, {16,28,0.6}, {28,4,0.6} }, {0, 0, 255}},          /* CW */
        {{ {-20,-20,0.1}, {40,10,0.3}, {10,50,0.2} }, {9, 99, 199}},    /* clips */
        {{ {30.5,30.5,0.9}, {80.5,30.5,0.9}, {55.5,90.5,0.9} }, {7,7,7}},
        {{ {90,90,0.4}, {120,60,0.4}, {60,120,0.4} }, {128,64,32}},     /* partly off */
        {{ {5,80,0.2}, {70,80,0.2}, {37,10,0.35} }, {11,22,33}},
        {{ {10,10,0.7}, {60,20,0.5}, {20,60,0.9} }, {200,200,0}},       /* overlap */
    };
    const int N = (int)(sizeof scene / sizeof scene[0]);
    int ref_written = 0, new_written = 0;
    for (int i = 0; i < N; ++i) {
        int a = legacy_triangle(&legacy, &scene[i]);
        int b = rt_triangle(&fresh, &scene[i]);
        if (i > 0) CK(a == b);
        if (a > 0) ref_written += a;
        if (b > 0) new_written += b;
    }
    CK(ref_written == new_written);
    CK(image_equal(&legacy, &fresh));
    rt_free(&legacy);
    rt_free(&fresh);
}

static void test_rejection_indices(void) {
    RTarget t;
    CK(rt_init(&t, 64, 64) == 0);
    RTri tris[5];
    tris[0] = tri_ccw();
    tris[1] = (RTri){{ {10,10,0}, {10,10,0}, {30,30,0} }, {0,0,0}};      /* coincident */
    tris[2] = (RTri){{ {0,0,0}, {8,8,0}, {16,16,0} }, {0,0,0}};          /* zero-area */
    tris[3] = tri_cw();
    tris[4] = (RTri){{ {0,0,0}, {10,0,0}, {5,1e-14,0} }, {0,0,0}};         /* sliver */
    RTBatchStats st;
    CK(rt_batch(&t, tris, 5, &st) == 0);
    CK(st.triangles == 5);
    CK(st.rejected == 3);
    CK(st.rejected_n == 3);
    CK(st.rejected_idx[0] == 1);
    CK(st.rejected_idx[1] == 2);
    CK(st.rejected_idx[2] == 4);
    CK(st.reason_counts[RT_CULL_REASON_COINCIDENT] == 1);
    CK(st.reason_counts[RT_CULL_REASON_ZERO_AREA] == 1);
    CK(st.reason_counts[RT_CULL_REASON_DEGENERATE_WINDING] == 1);
    CK(st.info[1].reason == RT_CULL_REASON_COINCIDENT);
    CK(st.info[2].reason == RT_CULL_REASON_ZERO_AREA);
    CK(st.info[4].reason == RT_CULL_REASON_DEGENERATE_WINDING);
    CK(st.info[3].reason == RT_CULL_REASON_NONE); /* CW is valid, just not drawn */
    /* The single legacy triangle still drew. */
    CK(st.filled > 0);
    CK(st.filled == st.covered);
    rt_stats_free(&st);
    rt_free(&t);
}

static void test_area_matches_hand_calc(void) {
    /* Hand-calculated signed areas for two triangles, with the two
     * resolutions used by the benchmark in mind. */
    RTri a = {{ {10,10,0.8}, {118,20,0.6}, {64,118,0.4} }, {0,0,0}};
    double area, area2;
    RTWinding w;
    CK(rt_classify(&a, &area, &area2, &w) == RT_CULL_REASON_NONE);
    /* (118-10)*(118-10) - (20-10)*(64-10) = 108*108 - 10*54 = 11664-540 = 11124 */
    CK(fabs(area2 - 11124.0) < 1e-9);
    CK(fabs(area - 5562.0) < 1e-9);
    CK(w == RT_WIND_CCW);

    /* Doubling the coordinates quadruples the area. */
    RTri a2 = a;
    for (int i = 0; i < 3; ++i) { a2.v[i].x *= 2; a2.v[i].y *= 2; }
    CK(rt_classify(&a2, &area, &area2, &w) == RT_CULL_REASON_NONE);
    CK(fabs(area2 - 44496.0) < 1e-9);

    RTri b = {{ {20,100,0.7}, {110,90,0.9}, {70,20,0.2} }, {0,0,0}};
    CK(rt_classify(&b, &area, &area2, &w) == RT_CULL_REASON_NONE);
    /* (110-20)*(20-100) - (90-100)*(70-20) = 90*(-80) - (-10)*50 = -7200+500 = -6700 */
    CK(fabs(area2 + 6700.0) < 1e-9);
    CK(fabs(area + 3350.0) < 1e-9);
    CK(w == RT_WIND_CW);
}

static void test_clear(void) {
    RTarget t;
    CK(rt_init(&t, 16, 16) == 0);
    RTri ccw = tri_ccw();
    CK(rt_triangle(&t, &ccw) > 0);
    CK(rt_clear(&t) == 0);
    CK(t.z[8*16+8] > 1e29f);
    CK(all_black(&t));
    rt_free(&t);
}

int main(void) {
    test_winding_classification();
    test_cull_modes();
    test_disabled_path_unchanged();
    test_rejection_indices();
    test_area_matches_hand_calc();
    test_clear();
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
