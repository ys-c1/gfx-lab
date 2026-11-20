/* Benchmark: two-step clip+rasterize vs per-pixel hand-computed baseline.
 * Compares filled/clipped pixel counts and runtime at 32x32 and 128x128. */
#define _POSIX_C_SOURCE 199309L
#include "raster.h"
#include <stdio.h>
#include <time.h>

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static double edge_(double ax, double ay, double bx, double by, double px, double py) {
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

/* per-pixel hand baseline: walk every pixel, test the triangle edges and
 * the clip rect directly */
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

static int run(int res, int iters) {
    RTarget t;
    if (rt_init(&t, res, res) != 0) return 1;
    /* triangle pokes out of the clip rect on all four sides and past the
     * target edges; clip rect is the centered quarter */
    RTri tri = {{
        { -0.20 * res,  0.30 * res, 0.5 },
        {  1.20 * res,  0.25 * res, 0.5 },
        {  0.50 * res,  1.30 * res, 0.5 },
    }, { 255, 0, 0 }};
    int cx = res / 4, cy = res / 4, cw = res / 2, ch = res / 2;
    if (rt_set_clip(&t, cx, cy, cw, ch) != 0) return 1;

    /* counts: two-step clip+rasterize */
    RTriStat st;
    rt_triangle_ex(&t, &tri, &st);

    /* counts: per-pixel hand baseline */
    int bf, bc;
    baseline(&t, &tri, cx, cy, cw, ch, &bf, &bc);

    /* timing: clip + rasterize (target cleared each iteration) */
    double t0 = now_s();
    for (int i = 0; i < iters; ++i) {
        rt_clear(&t);
        rt_triangle_ex(&t, &tri, NULL);
    }
    double t1 = now_s();

    /* timing: hand baseline */
    int sink = 0;
    for (int i = 0; i < iters; ++i) {
        int f, c;
        baseline(&t, &tri, cx, cy, cw, ch, &f, &c);
        sink += f + c;
    }
    double t2 = now_s();
    if (sink == -1) printf("impossible\n");

    int ok = (st.filled == bf) && (st.clipped == bc);
    printf("%4dx%-4d  filled %6d vs %6d   clipped %6d vs %6d   "
           "clip+raster %8.2f us   baseline %8.2f us   %s\n",
           res, res, st.filled, bf, st.clipped, bc,
           (t1 - t0) * 1e6 / iters, (t2 - t1) * 1e6 / iters,
           ok ? "MATCH" : "MISMATCH");
    rt_free(&t);
    return ok ? 0 : 1;
}

int main(void) {
    printf("%-10s  %-22s  %-22s  %-36s\n",
           "res", "filled (impl vs base)", "clipped (impl vs base)", "time per iter");
    int rc = 0;
    rc |= run(32, 20000);
    rc |= run(128, 2000);
    return rc;
}
