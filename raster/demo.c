#define _POSIX_C_SOURCE 199309L
#include "raster.h"
#include <stdio.h>
#include <time.h>

/* Scene in normalized [0,1] coordinates; scaled per target resolution. */
static RTri scene_make(int idx, double scale) {
    static const RTri base[5] = {
        /* 0: fully inside the near plane */
        {{ {0.05,0.10,0.90}, {0.95,0.08,0.80}, {0.50,0.90,0.70} }, {200, 40, 40}},
        /* 1: one vertex behind the near plane (1 inside side) */
        {{ {0.10,0.20,0.60}, {0.90,0.30,0.50}, {0.55,0.85,0.10} }, {40, 180, 220}},
        /* 2: one vertex behind, on the opposite side */
        {{ {0.20,0.70,0.40}, {0.80,0.75,0.35}, {0.45,0.15,0.05} }, {60, 200, 90}},
        /* 3: two vertices behind -> fully removed by clipping */
        {{ {0.15,0.15,0.20}, {0.85,0.20,0.05}, {0.50,0.80,0.15} }, {210, 210, 60}},
        /* 4: fully inside, small triangle */
        {{ {0.30,0.30,0.55}, {0.70,0.35,0.55}, {0.50,0.70,0.50} }, {200, 120, 220}},
    };
    RTri r = base[idx];
    for (int k = 0; k < 3; ++k) { r.v[k].x *= scale; r.v[k].y *= scale; }
    return r;
}

static double run(int res, int iters, RStats *acc, const char *ppm) {
    RTri tris[5];
    for (int i = 0; i < 5; ++i) tris[i] = scene_make(i, (double)res);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int it = 0; it < iters; ++it) {
        RTarget t;
        if (rt_init(&t, res, res) != 0) return -1.0;
        t.clip = 1;
        t.near_z = 0.25;
        RStats st = {0};
        int rc = rt_draw(&t, tris, 5, &st);
        if (rc != RE_OK && rc != RE_EMPTY) { rt_free(&t); return -1.0; }
        if (it == iters - 1) {
            *acc = st;
            if (ppm) rt_write_ppm(&t, ppm);
        }
        rt_free(&t);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    return (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
}

int main(void) {
    struct { int res; int iters; const char *ppm; } cfg[2] = {
        {32, 1000, "demo32.ppm"},
        {256, 20, "demo256.ppm"},
    };
    printf("%-5s %-8s %-8s %-10s %-12s %-12s\n",
           "res", "tris_in", "tris_out", "new_verts", "pixels", "ms/frame");
    long ref_verts = -1;
    for (int c = 0; c < 2; ++c) {
        RStats st = {0};
        double secs = run(cfg[c].res, cfg[c].iters, &st, cfg[c].ppm);
        double ms = secs * 1000.0 / cfg[c].iters;
        printf("%-5d %-8ld %-8ld %-10ld %-12ld %-12.4f\n",
               cfg[c].res, st.tris_in, st.tris_out, st.new_verts, st.pixels, ms);
        if (ref_verts < 0) ref_verts = st.new_verts;
        else if (ref_verts != st.new_verts)
            printf("WARNING: new_verts differs across resolutions\n");
    }
    printf("clipped-vertex count is resolution-independent; pixels scale ~%.1fx with area (256/32)^2.\n",
           (256.0 * 256.0) / (32.0 * 32.0));
    return 0;
}
