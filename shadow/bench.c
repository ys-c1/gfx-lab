/* Benchmark: three-stage soft shadow vs per-pixel Monte Carlo ground truth.
   Metrics at 64 and 256: penumbra-bandwidth error, light samples, time, RMSE. */
#define _POSIX_C_SOURCE 199309L
#include "shadow.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define REF_SAMPLES 256
#define TIMING_REPS 5

typedef struct {
    const char *name;
    int mode;       /* 0 hard, 1 fixed/adaptive ex, 2 reference */
    int samples;
} Variant;

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
}

static double median(double *v, int n) {
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j)
            if (v[j] < v[i]) { double t = v[i]; v[i] = v[j]; v[j] = t; }
    return v[n / 2];
}

/* width between the 10% and 90% visibility crossings of the LEFT edge,
   measured sub-pixel along the receiver row. Visibility is the shadow value
   normalised by the fully-lit value so falloff does not bias the thresholds. */
static float edge_width(const ShadowMap *m, int row, const float *lit) {
    size_t base = (size_t)row * m->w;
    const float *p = &m->buf[base];
    const float *z = &lit[base];
    float xlo = -1.0f, xhi = -1.0f;
    for (int x = 1; x < m->w; ++x) {
        float v0 = z[x - 1] > 1e-6f ? p[x - 1] / z[x - 1] : 1.0f;
        float v1 = z[x]     > 1e-6f ? p[x]     / z[x]     : 1.0f;
        if (v0 >= 0.9f && v1 < 0.9f && xhi < 0.0f)
            xhi = (float)x - 1.0f + (0.9f - v0) / (v1 - v0);
        if (v0 >= 0.1f && v1 < 0.1f && xlo < 0.0f)
            xlo = (float)x - 1.0f + (0.1f - v0) / (v1 - v0);
    }
    if (xlo < 0.0f || xhi < 0.0f) return 0.0f;
    return xlo - xhi;
}

static float rmse_to(const ShadowMap *a, const ShadowMap *b) {
    size_t n = (size_t)a->w * a->h;
    double s = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double d = (double)a->buf[i] - b->buf[i];
        s += d * d;
    }
    return (float)sqrt(s / (double)n);
}

static void run_resolution(int S, FILE *rep, const char *artdir) {
    /* Geometry (fraction of S): light (0.5,0.08), blocker row 0.45 spanning
       [0.2,0.8], receiver row 0.62. At the receiver the umbra edge sits near
       x=0.062S with W ~= 0.046S, so the full transition stays inside the map
       and far from the segment endpoint cone. */
    ShLight l = {S * 0.5f, S * 0.08f, S * 0.7f, 1.0f, S * 0.05f};
    ShSeg seg = {S * 0.2f, S * 0.45f, S * 0.8f, S * 0.45f};
    int row = (int)(S * 0.62f);

    ShadowMap map, gt, lit;
    sh_init(&map, S, S);
    sh_init(&gt, S, S);
    sh_init(&lit, S, S);

    /* fully-lit baseline for normalising the visibility profile */
    sh_render(&lit, &l, NULL, 0);

    ShError err;
    if (sh_render_ref(&gt, &l, &seg, 1, REF_SAMPLES, &err) != 0) {
        printf("ref failed: %s (%s)\n", err.message, err.field);
        return;
    }

    /* analytic stage-1 bandwidth at the receiver row, under the edge.
       W is the full 0-100% penumbra; a linear ramp spans 0.8W between 10% and 90%. */
    float wa = sh_penumbra_width(&l, &seg, S * 0.3f, (float)row + 0.5f);
    float wa1090 = 0.8f * wa;
    float wgt = edge_width(&gt, row, lit.buf);
    int adapt_n = sh_adaptive_sample_count(&map, &l, &seg, 1);

    Variant vs[] = {
        {"hard(off)", 0, 0},
        {"n=4",   1, 4},
        {"n=8",   1, 8},
        {"n=16",  1, 16},
        {"n=32",  1, 32},
        {"adaptive", 1, SH_SAMPLES_ADAPTIVE},
        {"per-pixel(256)", 2, REF_SAMPLES},
    };
    int nv = (int)(sizeof(vs) / sizeof(vs[0]));

    printf("\n=== resolution %d x %d ===\n", S, S);
    printf("stage-1 analytic W(0-100%%) = %.2f px | expected 10-90%% width = %.2f px | "
           "ground-truth measured = %.2f px | analytic error = %.3f px (%.1f%%)\n",
           wa, wa1090, wgt, fabsf(wgt - wa1090), 100.0f * fabsf(wgt - wa1090) / wgt);
    printf("adaptive sample count chosen by stage 1: %d\n", adapt_n);
    printf("%-16s %8s %10s %12s %12s %14s\n",
           "variant", "samples", "time(ms)", "width(px)", "W error(px)", "RMSE vs GT");

    fprintf(rep, "\n=== resolution %d x %d ===\n", S, S);
    fprintf(rep, "stage-1 analytic W(0-100%%) = %.2f px | expected 10-90%% width = %.2f px | "
           "ground-truth measured = %.2f px | analytic error = %.3f px (%.1f%%)\n",
           wa, wa1090, wgt, fabsf(wgt - wa1090), 100.0f * fabsf(wgt - wa1090) / wgt);
    fprintf(rep, "adaptive sample count chosen by stage 1: %d\n", adapt_n);
    fprintf(rep, "%-16s %8s %10s %12s %12s %14s\n",
            "variant", "samples", "time(ms)", "width(px)", "W error(px)", "RMSE vs GT");

    for (int vi = 0; vi < nv; ++vi) {
        double times[TIMING_REPS];
        int ns = vs[vi].samples;
        for (int r = 0; r < TIMING_REPS; ++r) {
            double t0 = now_ms();
            if (vs[vi].mode == 0) {
                sh_render(&map, &l, &seg, 1);
                ns = 1;
            } else if (vs[vi].mode == 1) {
                ShPenumbra pen = {1, vs[vi].samples};
                sh_render_ex(&map, &l, &seg, 1, &pen, &err);
                ns = vs[vi].samples == SH_SAMPLES_ADAPTIVE ? adapt_n : vs[vi].samples;
            } else {
                sh_render_ref(&map, &l, &seg, 1, vs[vi].samples, &err);
            }
            times[r] = now_ms() - t0;
        }
        float wm = edge_width(&map, row, lit.buf);
        float werr = fabsf(wm - wa1090);
        float rms = rmse_to(&map, &gt);
        double med = median(times, TIMING_REPS);
        printf("%-16s %8d %10.3f %12.2f %12.3f %14.5f\n",
               vs[vi].name, ns, med, wm, werr, rms);
        fprintf(rep, "%-16s %8d %10.3f %12.2f %12.3f %14.5f\n",
                vs[vi].name, ns, med, wm, werr, rms);

        if (S == 256 && artdir) {
            char path[256];
            snprintf(path, sizeof(path), "%s/soft256_%s.pgm", artdir,
                     vs[vi].mode == 0 ? "hard" :
                     vs[vi].mode == 2 ? "ref" :
                     vs[vi].samples == SH_SAMPLES_ADAPTIVE ? "adaptive" : "nXX");
            if (vs[vi].mode == 1 && vs[vi].samples > 0)
                snprintf(path, sizeof(path), "%s/soft256_n%d.pgm", artdir, ns);
            sh_write_pgm(&map, path);
        }
    }

    sh_free(&map); sh_free(&gt); sh_free(&lit);
}

int main(int argc, char **argv) {
    const char *artdir = argc > 1 ? argv[1] : ".";
    char rpath[320];
    snprintf(rpath, sizeof(rpath), "%s/bench_report.txt", artdir);
    FILE *rep = fopen(rpath, "w");
    if (!rep) { perror(rpath); return 1; }

    printf("penumbra benchmark (per-pixel %d samples = ground truth)\n", REF_SAMPLES);
    fprintf(rep, "penumbra benchmark (per-pixel %d samples = ground truth)\n", REF_SAMPLES);
    run_resolution(64, rep, artdir);
    run_resolution(256, rep, artdir);
    fclose(rep);
    printf("\nreport written to %s\n", rpath);
    return 0;
}
