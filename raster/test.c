#include "raster.h"
#include <stdio.h>
#include <string.h>
static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

static double edge_(double ax, double ay, double bx, double by, double px, double py) {
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

/* per-pixel hand-computed baseline: count samples inside the triangle,
 * split by clip-rect membership */
static void baseline(const RTarget *t, const RTri *tri,
                     int cx, int cy, int cw, int ch,
                     int *filled, int *clipped) {
    *filled = 0; *clipped = 0;
    for (int y = 0; y < t->h; ++y) {
        for (int x = 0; x < t->w; ++x) {
            double px = x + 0.5, py = y + 0.5;
            double w0 = edge_(tri->v[1].x, tri->v[1].y, tri->v[2].x, tri->v[2].y, px, py);
            double w1 = edge_(tri->v[2].x, tri->v[2].y, tri->v[0].x, tri->v[0].y, px, py);
            double w2 = edge_(tri->v[0].x, tri->v[0].y, tri->v[1].x, tri->v[1].y, px, py);
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            if (x >= cx && x < cx + cw && y >= cy && y < cy + ch) ++*filled;
            else ++*clipped;
        }
    }
}

int main(void) {
    RTarget t;
    CK(rt_init(&t, 32, 32) == 0);
    RTri tri = {{ {4,4,0.5}, {28,4,0.5}, {16,28,0.5} }, {255, 0, 0}};
    int n = rt_triangle(&t, &tri);
    CK(n > 200);
    CK(t.z[16*32+16] < 1.0f);
    CK(t.rgb[(16*32+16)*3] == 255);
    RTri flat = {{ {0,0,0}, {8,8,0}, {16,16,0} }, {0,255,0}};
    CK(rt_triangle(&t, &flat) == -2);
    rt_free(&t);

    /* --- clip rect validation: each failing item gets its own code --- */
    CK(rt_init(&t, 32, 32) == 0);
    CK(rt_set_clip(NULL, 0, 0, 4, 4) == -1);
    CK(rt_set_clip(&t, 0, 0, 0, 8) == -2);   /* empty: zero width  */
    CK(rt_set_clip(&t, 0, 0, 8, 0) == -2);   /* empty: zero height */
    CK(rt_set_clip(&t, 0, 0, -1, 8) == -3);  /* negative width     */
    CK(rt_set_clip(&t, 0, 0, 8, -1) == -3);  /* negative height    */
    CK(rt_set_clip(&t, -1, 0, 8, 8) == -4);  /* off left edge      */
    CK(rt_set_clip(&t, 0, -1, 8, 8) == -4);  /* off top edge       */
    CK(rt_set_clip(&t, 30, 0, 8, 8) == -4);  /* past right edge    */
    CK(rt_set_clip(&t, 0, 30, 8, 8) == -4);  /* past bottom edge   */
    CK(rt_set_clip(&t, 4, 4, 16, 16) == 0);
    CK(t.clip_on == 1);
    CK(rt_clip_strerror(-2) != NULL && strstr(rt_clip_strerror(-2), "empty") != NULL);
    CK(rt_clip_strerror(-3) != NULL && strstr(rt_clip_strerror(-3), "negative") != NULL);
    CK(rt_clip_strerror(-4) != NULL && strstr(rt_clip_strerror(-4), "exceeds") != NULL);
    rt_clear_clip(&t);
    CK(t.clip_on == 0);
    rt_free(&t);

    /* --- clip disabled vs full-buffer clip: pixel-identical --- */
    RTarget a, b;
    CK(rt_init(&a, 32, 32) == 0);
    CK(rt_init(&b, 32, 32) == 0);
    RTri t1 = {{ {2,3,0.6}, {29,5,0.4}, {15,30,0.8} }, {200, 30, 30}};
    RTri t2 = {{ {5,25,0.5}, {28,28,0.9}, {20,2,0.3} }, {30, 200, 220}};
    rt_triangle(&a, &t1); rt_triangle(&a, &t2);
    CK(rt_set_clip(&b, 0, 0, 32, 32) == 0);
    RTriStat s1, s2;
    rt_triangle_ex(&b, &t1, &s1);
    rt_triangle_ex(&b, &t2, &s2);
    CK(memcmp(a.z, b.z, sizeof(float) * 32 * 32) == 0);
    CK(memcmp(a.rgb, b.rgb, 32 * 32 * 3) == 0);
    CK(s1.clipped == 0 && s2.clipped == 0);
    rt_free(&a); rt_free(&b);

    /* --- two-step clip: counts match the per-pixel hand baseline --- */
    CK(rt_init(&t, 32, 32) == 0);
    RTri big = {{ {-6,4,0.5}, {38,6,0.5}, {16,40,0.5} }, {0, 255, 0}};
    CK(rt_set_clip(&t, 8, 8, 16, 16) == 0);
    RTriStat st;
    int rc = rt_triangle_ex(&t, &big, &st);
    CK(rc == st.filled);
    CK(rt_triangle(&t, &flat) == -2);   /* degenerate still reported under clip */
    int bf, bc;
    baseline(&t, &big, 8, 8, 16, 16, &bf, &bc);
    CK(st.filled == bf);
    CK(st.clipped == bc);
    CK(st.filled > 0 && st.clipped > 0);
    CK(st.filled + st.clipped > 200);
    /* nothing written outside the clip rect */
    int outside = 0;
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x)
            if ((x < 8 || x >= 24 || y < 8 || y >= 24) && t.rgb[(y*32+x)*3+1] != 0)
                ++outside;
    CK(outside == 0);
    /* every pixel the baseline expects inside the rect was written */
    int missing = 0;
    for (int y = 8; y < 24; ++y)
        for (int x = 8; x < 24; ++x) {
            double px = x + 0.5, py = y + 0.5;
            double w0 = edge_(big.v[1].x, big.v[1].y, big.v[2].x, big.v[2].y, px, py);
            double w1 = edge_(big.v[2].x, big.v[2].y, big.v[0].x, big.v[0].y, px, py);
            double w2 = edge_(big.v[0].x, big.v[0].y, big.v[1].x, big.v[1].y, px, py);
            if (w0 >= 0 && w1 >= 0 && w2 >= 0 && t.rgb[(y*32+x)*3+1] != 255)
                ++missing;
        }
    CK(missing == 0);
    rt_free(&t);

    /* --- clear_clip restores the unclipped path exactly --- */
    CK(rt_init(&a, 32, 32) == 0);
    CK(rt_init(&t, 32, 32) == 0);
    rt_triangle(&a, &big);
    CK(rt_set_clip(&t, 8, 8, 16, 16) == 0);
    rt_triangle(&t, &big);
    rt_clear_clip(&t);
    rt_clear(&t);
    rt_triangle(&t, &big);
    CK(memcmp(a.z, t.z, sizeof(float) * 32 * 32) == 0);
    CK(memcmp(a.rgb, t.rgb, 32 * 32 * 3) == 0);
    rt_free(&a); rt_free(&t);

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
