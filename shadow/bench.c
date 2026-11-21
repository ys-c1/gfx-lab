#define _POSIX_C_SOURCE 199309L
#include "shadow.h"
#include <math.h>
#include <stdio.h>
#include <time.h>

/* horizontal-edge benchmark scene.
   light is directly above the centre column; the blocker runs the full map
   width at y = edge_y, so the shadow boundary is a perfectly horizontal
   analytic line and every probe reduces to a 1D decision. */
typedef struct { int w, h; float edge_y; } Scene;

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* analytic coverage of the continuous kernel window [cy-(K-1)/2, cy+(K-1)/2]
   cut by the edge at edge_y: probes below the edge are shadowed. */
static float analytic_coverage(float cy, const Scene *sc, int K) {
    float half = (float)(K - 1) * 0.5f;
    float lo = cy - half, hi = cy + half;          /* probe window in y */
    float c = (sc->edge_y - lo) / (hi - lo);       /* lit fraction */
    if (c < 0.0f) c = 0.0f;
    if (c > 1.0f) c = 1.0f;
    return c;
}

/* independent hand-built reference of the stratified SxS sampling along the
   centre column: a probe row is lit iff cy + o(i) < edge_y. */
static float discrete_coverage(float cy, const Scene *sc, int S, int K) {
    int lit = 0;
    for (int i = 0; i < S; ++i) {
        float o = ((float)i - (float)(S - 1) * 0.5f) * (float)K / (float)S;
        if (cy + o < sc->edge_y) ++lit;
    }
    return (float)lit / (float)S;
}

/* measured centre-column coverage recovered by dividing the rendered value by
   the centre pixel's falloff (the compositing multiplier). */
static float measured_coverage(const ShadowMap *m, int x, int y,
                               const ShLight *lt) {
    float cx = (float)x + 0.5f, cy = (float)y + 0.5f;
    float dx = cx - lt->x, dy = cy - lt->y;
    float fall = 1.0f / (1.0f + sqrtf(dx * dx + dy * dy) / lt->radius);
    float v = m->buf[(size_t)y * m->w + x];
    if (fall <= 0.0f) return 0.0f;
    float c = v / fall;
    if (c < 0.0f) c = 0.0f;
    if (c > 1.0f) c = 1.0f;
    return c;
}

/* centre position of the 0.5 coverage crossing, in continuous y coordinates. */
static float crossing_y(const ShadowMap *m, int x, const ShLight *lt,
                        const Scene *sc) {
    for (int y = 1; y < sc->h; ++y) {
        float a = measured_coverage(m, x, y - 1, lt);
        float b = measured_coverage(m, x, y, lt);
        if (a >= 0.5f && b < 0.5f) {
            float t = (a - 0.5f) / (a - b);
            return (float)(y - 1) + 0.5f + t;
        }
    }
    return -1.0f;
}

static void run(const Scene *sc, int S, int K, FILE *rep) {
    ShadowMap m;
    sh_init(&m, sc->w, sc->h);
    ShLight lt = {(float)sc->w * 0.5f, 8.0f, (float)sc->h, 1.0f};
    ShSeg seg = {-1000.0f, sc->edge_y, (float)sc->w + 1000.0f, sc->edge_y};
    ShPCF pcf = {S, K};

    int iters = 5;
    double t0 = now_sec();
    int rc = 0;
    for (int i = 0; i < iters; ++i)
        rc = sh_render_pcf(&m, &lt, &seg, 1, &pcf);
    double elapsed = (now_sec() - t0) / (double)iters;
    if (rc != 0) { printf("render failed: %s\n", sh_errstr(rc)); return; }

    int x = sc->w / 2;
    float emax_disc = 0.0f, esum_disc = 0.0f;
    float emax_cont = 0.0f, esum_cont = 0.0f;
    int nband = 0;
    for (int y = 0; y < sc->h; ++y) {
        float cy = (float)y + 0.5f;
        float meas = measured_coverage(&m, x, y, &lt);
        float ed = fabsf(meas - discrete_coverage(cy, sc, S, K));
        float ec = fabsf(meas - analytic_coverage(cy, sc, K));
        if (ed > emax_disc) emax_disc = ed;
        if (ec > emax_cont) emax_cont = ec;
        esum_disc += ed; esum_cont += ec;
        if (meas > 0.0f && meas < 1.0f) ++nband;
    }
    float edge_err = fabsf(crossing_y(&m, x, &lt, sc) - sc->edge_y);
    long probes = (long)sc->w * (long)sc->h * (long)(S * S);
    long segrays = probes; /* one ray-test against one segment per probe */
    float soft_px = (float)(K - 1);
    float soft_frac = soft_px / (float)sc->h;

    printf("resolution %dx%d  S=%d K=%d\n", sc->w, sc->h, S, K);
    printf("  probes/pixel          : %d (per-axis %d)\n", S * S, S);
    printf("  total probe rays/frame: %ld (segment tests %ld)\n", probes, segrays);
    printf("  render time           : %.3f ms/frame (mean of %d)\n",
           elapsed * 1e3, iters);
    printf("  softening width       : %.1f px (%.4f of map height), %d partial rows\n",
           soft_px, soft_frac, nband);
    printf("  edge position error   : %.4f px (0.5-coverage crossing vs analytic edge)\n",
           edge_err);
    printf("  coverage MAE / max    : discrete %.2e / %.2e, analytic %.4f / %.4f\n",
           esum_disc / sc->h, emax_disc, esum_cont / sc->h, emax_cont);

    fprintf(rep, "%dx%d,%d,%d,%ld,%.6f,%.1f,%.6f,%.4f,%.6f,%.6f\n",
            sc->w, sc->h, S, K, probes, elapsed * 1e3, soft_px, soft_frac,
            edge_err, esum_cont / sc->h, emax_cont);
    sh_free(&m);
}

int main(void) {
    Scene scenes[2] = {
        {128, 128, 40.0f},
        {512, 512, 40.0f},
    };
    const int S = 4, K = 5;

    FILE *rep = fopen("bench_report.csv", "w");
    if (!rep) return 1;
    fprintf(rep, "resolution,samples_per_axis,kernel,probe_rays,time_ms,"
                 "soft_width_px,soft_width_frac,edge_err_px,"
                 "coverage_mae_analytic,coverage_max_analytic\n");
    for (int i = 0; i < 2; ++i)
        run(&scenes[i], S, K, rep);
    fclose(rep);

    /* export the actual kernel table used by the benchmark */
    ShKernel kern;
    if (sh_kernel_build(&kern, S, K) == 0) {
        sh_kernel_write(&kern, "kernel_s4_k5.txt");
        sh_kernel_free(&kern);
    }

    /* kernel length -> softening width lookup table */
    FILE *wt = fopen("kernel_width_table.txt", "w");
    if (!wt) return 1;
    fprintf(wt, "# kernel length K vs softening width (odd kernel lengths)\n");
    fprintf(wt, "# soft_width_px = K - 1 (full penumbra band in output pixels)\n");
    fprintf(wt, "kernel_K soft_width_px\n");
    for (int k = 1; k <= 21; k += 2)
        fprintf(wt, "%d %.1f\n", k, (float)(k - 1));
    fclose(wt);

    printf("\nexported: bench_report.csv, kernel_s4_k5.txt, kernel_width_table.txt\n");
    return 0;
}
