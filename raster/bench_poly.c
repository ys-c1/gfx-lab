#define _POSIX_C_SOURCE 199309L
#include "raster.h"
#include "rpf.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec * 1e-6;
}

static RVert V(double x, double y) { RVert v = { x, y, 0.0 }; return v; }

static int make_bowtie(RVert *v, int n) {
    /* true self-crossing bowtie, same vertex order as test_poly.c; scaled
     * from the 16x16 hand-calc geometry and offset by half a pixel. */
    double s = (n - 1) / 15.0;
    double off = 0.5;
    v[0] = V(2 * s + off, 2 * s + off);    /* top-left */
    v[1] = V(14 * s + off, 14 * s + off);  /* bottom-right */
    v[2] = V(14 * s + off, 2 * s + off);   /* top-right */
    v[3] = V(2 * s + off, 14 * s + off);   /* bottom-left */
    return 4;
}

static int make_star(RVert *v, int n) {
    double c = n / 2.0, R = n * 0.44, r = n * 0.19;
    for (int i = 0; i < 10; ++i) {
        double rad = (i % 2 == 0) ? R : r;
        double ang = -M_PI / 2 + i * M_PI / 5;
        v[i] = V(c + rad * cos(ang), c + rad * sin(ang));
    }
    return 10;
}

int main(int argc, char **argv) {
    const char *outdir = argc > 1 ? argv[1] : ".";
    const int grids[2] = { 32, 256 };
    const int iters[2] = { 200, 20 };
    unsigned char eo[3] = { 30, 90, 220 }, pos[3] = { 220, 60, 60 }, neg[3] = { 40, 180, 90 };
    char path[256];
    snprintf(path, sizeof(path), "%s/bench.json", outdir);
    FILE *j = fopen(path, "w");
    if (!j) { perror(path); return 1; }
    fprintf(j, "[\n");
    int first = 1;

    for (int gi = 0; gi < 2; ++gi) {
        int n = grids[gi], rep = iters[gi];
        for (int shape = 0; shape < 2; ++shape) {
            RVert verts[10];
            int nv = shape == 0 ? make_bowtie(verts, n) : make_star(verts, n);
            RPoly p = { verts, nv };
            for (int rule = 0; rule < 2; ++rule) {
                for (int mode = 0; mode < 2; ++mode) {
                    RTarget t;
                    if (rt_init(&t, n, n) != 0) return 2;
                    RPFillOpts o = { rule, mode, 0.5, eo, pos, neg };
                    RPFStats st;
                    /* timed repetitions on fresh targets */
                    double best = 1e9;
                    int fill = 0;
                    for (int k = 0; k < rep; ++k) {
                        double a = now_ms();
                        fill = rpf_polygon(&t, &p, &o, &st);
                        double dt = now_ms() - a;
                        if (dt < best) best = dt;
                        if (k < rep - 1) rpf_stats_free(&st);
                    }
                    const char *sn = shape == 0 ? "bowtie" : "star";
                    const char *rn = rule == RPF_RULE_EVEN_ODD ? "evenodd" : "nonzero";
                    const char *mn = mode == RPF_MODE_OFF ? "off" : "on";
                    if (!first) fprintf(j, ",\n");
                    first = 0;
                    fprintf(j,
                        " {\"grid\":%d,\"shape\":\"%s\",\"rule\":\"%s\",\"mode\":\"%s\","
                        "\"verts\":%d,\"ntris\":%d,\"covered\":%d,\"eo_fill\":%d,"
                        "\"nz_fill\":%d,\"fill_written\":%d,\"overflow\":%d,"
                        "\"nstates\":%d,\"best_ms\":%.4f,\"phase1_ms\":%.4f,"
                        "\"phase2_ms\":%.4f,\"iters\":%d}",
                        n, sn, rn, mn, nv, st.ntris, st.covered_pixels,
                        st.eo_fill_pixels, st.nz_fill_pixels, fill, st.overflow_pixels,
                        st.nstates, best, st.phase1_ms, st.phase2_ms, rep);
                    /* per-pixel coverage dump from the OFF run: shading is
                     * skipped but phase-1 records are fully populated, so the
                     * dump is independent of the chosen fill rule. */
                    if (mode == RPF_MODE_OFF) {
                        snprintf(path, sizeof(path), "%s/%s_%d_cover.txt",
                                 outdir, sn, n);
                        FILE *cf = fopen(path, "w");
                        if (cf) {
                            for (int yy = 0; yy < n; ++yy)
                                for (int xx = 0; xx < n; ++xx) {
                                    RPFCover cv;
                                    rpf_pixel_get(&t, &st, xx, yy, &cv);
                                    fprintf(cf, "%d %d %d %d %d\n",
                                            xx, yy, cv.parity, cv.winding,
                                            cv.ntris);
                                }
                            fclose(cf);
                        }
                    }
                    /* artifacts for one representative enabled run */
                    if (mode == RPF_MODE_ENABLED) {
                        snprintf(path, sizeof(path), "%s/%s_%d_%s", outdir, sn, n, rn);
                        char f[300];
                        snprintf(f, sizeof(f), "%s.ppm", path); rt_write_ppm(&t, f);
                        snprintf(f, sizeof(f), "%s_parity.pgm", path);
                        rpf_write_parity_pgm(&t, &st, f);
                        snprintf(f, sizeof(f), "%s_winding.pgm", path);
                        rpf_write_winding_pgm(&t, &st, f);
                        snprintf(f, sizeof(f), "%s_report.txt", path);
                        rpf_write_report(&t, &st, &p, &o, f);
                    }
                    rt_free(&t);
                    rpf_stats_free(&st);
                }
            }
        }
    }
    fprintf(j, "\n]\n");
    fclose(j);
    printf("benchmark artifacts written under %s\n", outdir);
    return 0;
}
