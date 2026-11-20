#define _POSIX_C_SOURCE 199309L
#include "atlas.h"
#include <stdio.h>
#include <time.h>

#define N 8
#define REPEAT 100000

static long long now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void run_width(int width, const AtlasRect *r) {
    const AtlasOrient *tab = atlas_orient_table();
    AtlasPos p[N];
    AtlasStats best;
    AtlasStats passes[ATLAS_ORIENTS];

    printf("== page width %d ==\n", width);
    printf("orient-table:");
    for (int k = 0; k < ATLAS_ORIENTS; ++k)
        printf(" [%d deg swap=%d q=%d]", tab[k].angle, tab[k].swap,
               tab[k].quadrant);
    printf("\n");

    if (atlas_pack_best(r, N, width, 1, p, &best, passes) != ATLAS_OK) {
        printf("PACK FAILED err_index=%d\n", best.err_index);
        return;
    }

    long long sweep_ns = 0;
    printf("passes:\n");
    for (int k = 0; k < ATLAS_ORIENTS; ++k) {
        AtlasStats *st = &passes[k];
        sweep_ns += st->elapsed_ns;
        printf("  angle=%3d shelves=%d H=%3d used=%lld area=%lld "
               "waste=%lld rate=%.4f gap=%lld slack=%lld "
               "bound=%lld bound_rate=%.4f time=%.3fus\n",
               st->angle, st->shelves, st->height, (long long)st->used_px,
               st->area, st->waste_px, st->waste_rate, st->gap_px,
               st->slack_px, st->bound_px, st->bound_rate,
               st->elapsed_ns / 1000.0);
    }
    printf("sweep time=%.3fus  best angle=%d  waste_rate=%.4f  "
           "bound_rate=%.4f (upper bound)\n",
           sweep_ns / 1000.0, best.angle, best.waste_rate, best.bound_rate);

    /* Stable timing: the full four-orientation sweep, repeated REPEAT times
     * so the per-sweep cost is measurable instead of clock-noise dominated. */
    AtlasPos bp2[N];
    AtlasStats bs2, ps2[ATLAS_ORIENTS];
    long long t0 = now_ns();
    for (int rep = 0; rep < REPEAT; ++rep)
        atlas_pack_best(r, N, width, 1, bp2, &bs2, ps2);
    long long dt = now_ns() - t0;
    printf("timed sweep: %.3f us/call over %d calls "
           "(no-rotate baseline: ", (double)dt / 1000.0 / REPEAT, REPEAT);
    t0 = now_ns();
    for (int rep = 0; rep < REPEAT; ++rep)
        atlas_pack_best(r, N, width, 0, bp2, &bs2, 0);
    dt = now_ns() - t0;
    printf("%.3f us/call)\n", (double)dt / 1000.0 / REPEAT);
    /* Procedural guarantee: the 0-degree run is always a valid upper bound
     * on the chosen waste, since the sweep includes angle 0. */
    printf("0deg reference waste_rate=%.4f >= best: %s\n",
           passes[0].waste_rate,
           passes[0].waste_px >= best.waste_px ? "yes" : "NO");

    printf("placement (angle=%d):\n", best.angle);
    for (int i = 0; i < N; ++i) {
        int rw = (best.angle == 90 || best.angle == 270) ? r[i].h : r[i].w;
        int rh = (best.angle == 90 || best.angle == 270) ? r[i].w : r[i].h;
        printf("  block %d: x=%3d y=%3d footprint=%dx%d\n", i, p[i].x, p[i].y,
               rw, rh);
    }

    /* No-rotation run: layout must equal the legacy packer. */
    AtlasPos np[N], gp[N];
    AtlasStats ns, gs;
    atlas_pack_best(r, N, width, 0, np, &ns, 0);
    atlas_pack(r, N, width, gp, &gs);
    int same = 1;
    for (int i = 0; i < N; ++i)
        if (np[i].x != gp[i].x || np[i].y != gp[i].y) same = 0;
    printf("no-rotate matches legacy: %s (angle=%d shelves=%d rate=%.4f)\n\n",
           same ? "yes" : "NO", ns.angle, ns.shelves, ns.waste_rate);
}

int main(void) {
    AtlasRect r[N];
    for (int i = 0; i < N; ++i) {
        r[i].w = 8 + i * 4;
        r[i].h = 8 + (i % 3) * 8;
    }
    printf("blocks (w x h):");
    for (int i = 0; i < N; ++i) printf(" %dx%d", r[i].w, r[i].h);
    printf("\n\n");
    run_width(128, r);
    run_width(512, r);
    return 0;
}
