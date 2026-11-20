#define _POSIX_C_SOURCE 200809L
#include "raster.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* One oblique ground triangle in camera space (camera at origin, +z
   forward). The ground plane y = -1 stretches from z=2 to z=20, so the
   screen-space footprint is strongly foreshortened: affine interpolation
   visibly smears the texture toward the far vertex. */
static RCamTri ground_triangle(void) {
    RCamTri ct = {{
        { -1.5, -1.0,  2.0, 0.0, 0.0 },
        {  1.5, -1.0,  2.0, 1.0, 0.0 },
        {  0.0, -1.0, 20.0, 0.5, 1.0 },
    }};
    return ct;
}

static void reset_target(RTexTarget *t) {
    size_t n = (size_t)t->w * (size_t)t->h;
    for (size_t i = 0; i < n; ++i) { t->z[i] = 1e30f; t->u[i] = 0.0; t->v[i] = 0.0; }
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
}

static double timed_render(RTexTarget *t, const RTexTri *tri, int correct, int reps) {
    double best = 1e300;
    for (int r = 0; r < reps; ++r) {
        reset_target(t);
        double t0 = now_ms();
        int n = rtt_triangle(t, tri, correct);
        double t1 = now_ms();
        if (n <= 0) return -1.0;
        double dt = t1 - t0;
        if (dt < best) best = dt;
    }
    return best;
}

/* stage 3 reference: per-pixel inverse solve over the whole target. */
static void inverse_check(const RTexTarget *t, const RCamTri *ct,
                          double fx, double fy, double cx, double cy,
                          double *maxerr, double *meanerr, int *mismatch) {
    *maxerr = 0.0; *meanerr = 0.0; *mismatch = 0;
    long hits = 0;
    size_t n = (size_t)t->w * (size_t)t->h;
    for (size_t i = 0; i < n; ++i) {
        double px = (double)(i % (size_t)t->w) + 0.5;
        double py = (double)(i / (size_t)t->w) + 0.5;
        double guv[2];
        int hit = rtt_unproject(ct, fx, fy, cx, cy, px, py, guv);
        int filled = t->z[i] < 1e29f;
        if (hit != filled) { ++*mismatch; continue; }
        if (!hit) continue;
        double du = t->u[i] - guv[0], dv = t->v[i] - guv[1];
        double e = fabs(du) > fabs(dv) ? fabs(du) : fabs(dv);
        if (e > *maxerr) *maxerr = e;
        *meanerr += e;
        ++hits;
    }
    if (hits > 0) *meanerr /= (double)hits;
}

static void write_checker(const RTexTarget *t, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fprintf(f, "P6\n%d %d\n255\n", t->w, t->h);
    size_t n = (size_t)t->w * (size_t)t->h;
    for (size_t i = 0; i < n; ++i) {
        unsigned char r, g, b;
        if (t->z[i] >= 1e29f) { r = g = b = 30; }
        else {
            int cx = (int)(t->u[i] * 8.0), cy = (int)(t->v[i] * 8.0);
            int on = ((cx + cy) & 1) != 0;
            r = on ? 230 : 60; g = on ? 90 : 200; b = on ? 90 : 200;
        }
        fputc(r, f); fputc(g, f); fputc(b, f);
    }
    fclose(f);
}

int main(void) {
    const int res[2] = {64, 256};
    const int reps[2] = {2000, 200};
    printf("perspective-correct vs affine interpolation, per-pixel inverse check\n");
    printf("%-5s %-9s %-14s %-14s %-9s %-10s %-10s\n",
           "res", "mode", "max|uv err|", "mean|uv err|", "filled", "mismatch", "ms/call");
    for (int k = 0; k < 2; ++k) {
        int R = res[k];
        double fx = R * 0.5, fy = R * 0.5, cx = R * 0.5, cy = R * 0.5;
        RCamTri ct = ground_triangle();
        RTexTri tri;
        tri.rgb[0] = 200; tri.rgb[1] = 120; tri.rgb[2] = 40;
        for (int j = 0; j < 3; ++j)
            if (rtt_project(&ct.v[j], fx, fy, cx, cy, &tri.v[j]) != RT_OK) return 1;

        for (int mode = 1; mode >= 0; --mode) {
            RTexTarget t;
            if (rtt_init(&t, R, R) != RT_OK) return 1;
            double ms = timed_render(&t, &tri, mode, reps[k]);
            int filled = 0;
            size_t n = (size_t)R * (size_t)R;
            for (size_t i = 0; i < n; ++i) if (t.z[i] < 1e29f) ++filled;
            double maxerr, meanerr; int mismatch;
            inverse_check(&t, &ct, fx, fy, cx, cy, &maxerr, &meanerr, &mismatch);
            printf("%-5d %-9s %-14.3e %-14.3e %-9d %-10d %-10.4f\n",
                   R, mode ? "correct" : "affine",
                   maxerr, meanerr, filled, mismatch, ms);
            if (R == 256)
                write_checker(&t, mode ? "bench_correct.ppm" : "bench_affine.ppm");
            rtt_free(&t);
        }
    }
    return 0;
}
