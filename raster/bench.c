#define _POSIX_C_SOURCE 199309L  /* clock_gettime under -std=c11 */
#include "raster.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* Benchmark + hand-check for the two wireframe stages.
 *
 * Stage 1 timing: exhaustive screen sweep calling rt_edge_dist() for all three
 * edges, with the result folded into a sum (not written to pixels).
 * Stage 2 timing: the full rt_wire() pass (distance + threshold + write + count).
 *
 * Line width is measured by probing a column through the horizontal edge and
 * counting covered pixel centres.  Because centres sit on a 1 px lattice, the
 * count changes with sub-pixel edge alignment, so we sweep the edge through
 * four quarter-pixel offsets and report the min/max.  Nominal width is 2*T.
 * That envelope is the honest error a mask rasteriser can offer. */

typedef struct { const char *name; int size; double threshold; RTri tri; } Case;

static const Case CASES[] = {
    {"32", 32, 0.5, {{ {4.5,4.5,0.5}, {28.5,4.5,0.5}, {16.5,28.5,0.5} }, {255,255,255}}},
    {"128", 128, 2.0, {{ {16.5,16.5,0.5}, {112.5,16.5,0.5}, {64.5,112.5,0.5} }, {255,255,255}}},
};

static volatile double sink;

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* Count how many centres along `col` lie within T of the horizontal segment. */
static int probe_width(const RTri *tri, int col, double threshold, int h) {
    int n = 0;
    for (int y = 0; y < h; ++y) {
        double d;
        if (rt_edge_dist(&tri->v[0], &tri->v[1], col, y, &d) == RT_WIRE_OK && d <= threshold)
            ++n;
    }
    return n;
}

/* Independent quadratic distance-to-segment; counts pixels whose mask bit
 * disagrees with the true predicate.  Catches any drift between stage 1 and
 * stage 2. */
static int brute_audit(const RTarget *t, const RTri *tri, double threshold) {
    int bad = 0;
    for (int y = 0; y < t->h; ++y) {
        for (int x = 0; x < t->w; ++x) {
            int expect = 0;
            for (int i = 0; i < 3 && !expect; ++i) {
                const RVert *a = &tri->v[i], *b = &tri->v[(i + 1) % 3];
                double px = x + 0.5, py = y + 0.5;
                double dx = b->x - a->x, dy = b->y - a->y;
                double L2 = dx * dx + dy * dy;
                double s = ((px - a->x) * dx + (py - a->y) * dy) / L2;
                if (s < 0) s = 0;
                if (s > 1) s = 1;
                double ex = px - (a->x + s * dx), ey = py - (a->y + s * dy);
                if (sqrt(ex * ex + ey * ey) <= threshold) expect = 1;
            }
            if (expect != (t->wire[(size_t)y * t->w + x] != 0)) ++bad;
        }
    }
    return bad;
}

static double time_stage1(const RTri *tri, int w, int h, int reps) {
    double t0 = now_s();
    for (int r = 0; r < reps; ++r) {
        double acc = 0.0;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                for (int i = 0; i < 3; ++i) {
                    double d;
                    rt_edge_dist(&tri->v[i], &tri->v[(i + 1) % 3], x, y, &d);
                    acc += d;
                }
        sink += acc;
    }
    return now_s() - t0;
}

static double time_full(RTarget *t, const RTri *tri, const RWire *w, int reps) {
    double t0 = now_s();
    for (int r = 0; r < reps; ++r) {
        RWireStats s;
        RWireErr e;
        RTri copy = *tri;
        copy.v[0].z = 0.5 + (double)(r & 1) * 1e-9; /* keep the call live */
        sink += rt_wire(t, &copy, w, &s, &e);
    }
    return now_s() - t0;
}

int main(void) {
    int rc = 0;
    for (size_t c = 0; c < sizeof CASES / sizeof CASES[0]; ++c) {
        const Case *k = &CASES[c];
        RTarget t;
        if (rt_init(&t, k->size, k->size) != 0) return 1;
        RWire w = {k->threshold, {0, 255, 0}, 1};
        RWireStats s;
        RWireErr e;

        int covered = rt_wire(&t, &k->tri, &w, &s, &e);
        if (covered < 0) { printf("%s: rt_wire error %d\n", k->name, covered); return 1; }

        /* Probe the horizontal edge over a quarter-pixel offset sweep. */
        int col = (int)((k->tri.v[0].x + k->tri.v[1].x) / 2.0);
        int wmin = 0x7fff, wmax = 0;
        for (int q = 0; q < 4; ++q) {
            RTri p = k->tri;
            p.v[0].y += q / 4.0;
            p.v[1].y += q / 4.0;
            int m = probe_width(&p, col, k->threshold, t.h);
            if (m < wmin) wmin = m;
            if (m > wmax) wmax = m;
        }
        double nominal = rt_wire_nominal_width(&w);
        /* Worst-case deviation of the sampled count from the continuous width. */
        double err_lo = fabs((double)wmin - nominal), err_hi = fabs((double)wmax - nominal);
        double err = err_lo > err_hi ? err_lo : err_hi;

        int reps = k->size <= 32 ? 2000 : 400;
        double t1 = time_stage1(&k->tri, k->size, k->size, reps) / reps * 1e6;
        double tf = time_full(&t, &k->tri, &w, reps) / reps * 1e6;

        int bad = brute_audit(&t, &k->tri, k->threshold);
        if (bad) rc = 1;
        printf("%-4s size=%3d T=%.2f nominal=%4.1f px measured=[%d,%d] px err<=%.2f px "
               "covered=%d edge=[%d %d %d] audit_bad=%d stage1=%.2f us full=%.2f us\n",
               k->name, k->size, k->threshold, nominal, wmin, wmax, err,
               s.covered, s.edge[0], s.edge[1], s.edge[2], bad, t1, tf);
        rt_free(&t);
    }
    return rc;
}
