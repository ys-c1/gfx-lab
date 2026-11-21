/*
 * Two-stage dither comparison: 32-code vs 256-code output on the demo light
 * field, plus a hand-computed check of the exact same arithmetic.
 *
 * Build/run: make -C shadow bench
 */
#include "shadow.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define W 128
#define H 128
#define REPEAT 5

typedef struct {
    ShDitherStats st;
    double stage1_best, stage2_best;
} Run;

/* Run both stages REPEAT times and keep the best times; the output of every
 * repetition is identical, so timing noise is the only variation. */
static int run(const ShadowMap *m, const char *path, const ShDither *d,
               double *comp, Run *r) {
    char err[256];
    ShDitherStats st;
    double best1 = 1e30, best2 = 1e30;
    for (int i = 0; i < REPEAT; ++i) {
        if (sh_dither_error(m, d, comp, &st, err, sizeof err) != 0) {
            fprintf(stderr, "stage 1: %s\n", err);
            return -1;
        }
        double t1 = st.stage1_ms;
        if (sh_write_pgm_comp(m, path, d, comp, &st, err, sizeof err) != 0) {
            fprintf(stderr, "stage 2: %s\n", err);
            return -1;
        }
        if (t1 < best1) best1 = t1;
        if (st.stage2_ms < best2) best2 = st.stage2_ms;
    }
    r->st = st;
    r->stage1_best = best1;
    r->stage2_best = best2;
    return 0;
}

static void report(const char *name, const Run *r) {
    const ShDitherStats *s = &r->st;
    printf("%-8s levels=%-3d bands=%-5lld lf=%.5f comp(mean=%+.4f abs=%.4f rms=%.4f) "
           "err(mean=%+.6f rms=%.6f code_rms=%.4f) t=%.3f+%.3f ms\n",
           name, s->levels, s->ring_bands, s->err_lf_rms,
           s->comp_mean, s->comp_absmean, s->comp_rms,
           s->err_mean, s->err_rms, s->err_code_rms,
           r->stage1_best, r->stage2_best);
}

/* Run both code depths, with and without dithering, and print the summary.
 * Returns non-zero if any stage fails. */
static int compare_field(const ShadowMap *m, double *comp) {
    Run r32, r256, r32plain, r256plain;
    ShDither d32 = {1, 32, 1.0}, d256 = {1, 256, 1.0};
    ShDither p32 = {1, 32, 0.0}, p256 = {1, 256, 0.0};

    if (run(m, "/tmp/shadow_bench32.pgm",   &d32, comp, &r32)  != 0 ||
        run(m, "/tmp/shadow_bench256.pgm",  &d256, comp, &r256) != 0 ||
        run(m, "/tmp/shadow_bench32p.pgm",  &p32, comp, &r32plain)  != 0 ||
        run(m, "/tmp/shadow_bench256p.pgm", &p256, comp, &r256plain) != 0)
        return -1;

    printf("bands = 2-row-thick constant-code runs spanning >=0.5 code\n"
           "lf = RMS of quantization error after %d px box blur (the banding signal)\n",
           SH_BAND_BLUR);
    printf("-- dither on ---------------------------------------------------------------\n");
    report("32-level", &r32);
    report("256-level", &r256);
    printf("-- dither off, same code depth (banding baseline) --------------------------\n");
    report("plain32", &r32plain);
    report("plain256", &r256plain);

    printf("-- 32 vs 256, dither on ----------------------------------------------------\n");
    printf("  bands:      %lld -> %lld\n", r32.st.ring_bands, r256.st.ring_bands);
    printf("  lf rms:     %.5f -> %.5f  (%.2fx smaller)\n", r32.st.err_lf_rms, r256.st.err_lf_rms,
           r32.st.err_lf_rms / (r256.st.err_lf_rms > 1e-12 ? r256.st.err_lf_rms : 1e-12));
    printf("  err rms:    %.6f -> %.6f  (%.2fx smaller)\n", r32.st.err_rms, r256.st.err_rms,
           r32.st.err_rms / (r256.st.err_rms ? r256.st.err_rms : 1.0));
    printf("  code rms:   %.4f -> %.4f codes\n", r32.st.err_code_rms, r256.st.err_code_rms);
    printf("  comp rms:   %.4f -> %.4f codes\n", r32.st.comp_rms, r256.st.comp_rms);
    printf("  stage1:     %.3f -> %.3f ms  (%.2fx)\n", r32.stage1_best, r256.stage1_best,
           r256.stage1_best / (r32.stage1_best ? r32.stage1_best : 1.0));
    printf("  stage2:     %.3f -> %.3f ms  (%.2fx)\n", r32.stage2_best, r256.stage2_best,
           r256.stage2_best / (r32.stage2_best ? r32.stage2_best : 1.0));
    printf("  bands (dither off vs on): 32 %lld/%lld, 256 %lld/%lld\n",
           r32plain.st.ring_bands, r32.st.ring_bands,
           r256plain.st.ring_bands, r256.st.ring_bands);
    printf("  lf rms (dither off vs on): 32 %.5f/%.5f, 256 %.5f/%.5f\n",
           r32plain.st.err_lf_rms, r32.st.err_lf_rms,
           r256plain.st.err_lf_rms, r256.st.err_lf_rms);
    return 0;
}

/* Hand-computed cross-check.
 *
 * 4x4 field, every row 0.10 / 0.35 / 0.60 / 0.85, levels=4 (codes 0..3),
 * amount=1, serpentine Floyd-Steinberg. Targets in code units are
 * 0.30 / 1.05 / 1.80 / 2.55.
 *
 * Row 0 walks left to right:
 *   x0 0.30              -> code 0, residual +0.30
 *   x1 1.05 + 0.30*7/16  -> code 1, residual +0.18125
 *   x2 1.80 + 0.18125*7/16 -> code 2, residual -0.120703125
 *   x3 2.55 - 0.120703125*7/16 -> code 2
 * Row 1 walks right to left and receives
 *   x0 += 0.55*5/16 (5/16 straight down) + 0.55*1/16 (1/16 down-right)
 * from x0 above and its 3/16 down-left term from x1, and its own rounding,
 * which yields codes 0,1,2,3. Rows 2 and 3 repeat rows 0 and 1 because the
 * input is row-invariant and the serpentine schedule alternates.
 *
 * Expected codes:      row0 0 1 2 2      row1 0 1 2 3
 *                      row2 0 1 2 2      row3 0 1 2 3
 * Expected row-0 comp: 0, 0.13125, 0.079296875, -0.0528076171875
 */
static int hand_check(void) {
    enum { L = 4 };
    ShadowMap t;
    if (sh_init(&t, L, L) != 0) return -1;
    const float v[L] = {0.10f, 0.35f, 0.60f, 0.85f};
    for (int y = 0; y < L; ++y)
        for (int x = 0; x < L; ++x) t.buf[y*L+x] = v[x];

    double comp[L*L];
    ShDitherStats st;
    char err[256];
    ShDither d = {1, 4, 1.0};
    if (sh_dither_error(&t, &d, comp, &st, err, sizeof err) != 0) {
        fprintf(stderr, "hand: %s\n", err);
        sh_free(&t);
        return -1;
    }

    const int want_code[L][L] = {{0,1,2,2},{0,1,2,3},{0,1,2,2},{0,1,2,3}};
    const double want_comp0[L] = {0.0, 0.13125, 0.079296875, -0.0528076171875};
    /* The code at (x,y) is recoverable from the compensation map and the
     * input via the same rounding stage 2 performs. */
    int bad = 0;
    printf("\nhand check, 4x4, levels=4, amount=1 (serpentine FS)\n");
    printf("  row  x0          x1          x2          x3        codes (got / want)\n");
    for (int y = 0; y < L; ++y) {
        printf("  %d  ", y);
        for (int x = 0; x < L; ++x) {
            double vv = (double)v[x];
            int got = (int)floor(vv * 3.0 + comp[y*L+x] + 0.5);
            if (got < 0) got = 0;
            if (got > 3) got = 3;
            printf("%+.8f  ", comp[y*L+x]);
            if (got != want_code[y][x]) bad = 1;
        }
        for (int x = 0; x < L; ++x) {
            double vv = (double)v[x];
            int got = (int)floor(vv * 3.0 + comp[y*L+x] + 0.5);
            if (got < 0) got = 0;
            if (got > 3) got = 3;
            printf(" %d/%d", got, want_code[y][x]);
        }
        printf("\n");
    }
    printf("  row0 comp hand values:");
    for (int x = 0; x < L; ++x) {
        int ok = fabs(comp[x] - want_comp0[x]) < 1e-6;
        if (!ok) bad = 1;
        printf(" %.9f%s", want_comp0[x], ok ? "" : " MISMATCH");
    }
    printf("\n  -> %s\n", bad ? "MISMATCH" : "all 16 codes and 4 row-0 compensations match");
    sh_free(&t);
    return bad ? -1 : 0;
}

int main(void) {
    ShadowMap m;
    if (sh_init(&m, W, H) != 0) return 1;
    ShLight l = {64.0f, 10.0f, 40.0f, 1.0f};
    ShSeg segs[2] = {{20.0f, 60.0f, 60.0f, 60.0f}, {70.0f, 70.0f, 110.0f, 70.0f}};
    if (sh_render(&m, &l, segs, 2) != 0) { sh_free(&m); return 1; }

    /* A second field: a shallow vertical ramp over most of the range. This
     * is the case where 8-bit output visibly bands, and where the two code
     * depths differ, so it is where the comparison is meaningful. */
    ShadowMap ramp;
    if (sh_init(&ramp, W, H) != 0) { sh_free(&m); return 1; }
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            ramp.buf[y*W+x] = 0.45f + 0.10f * (float)y / (float)(H - 1);

    double *comp = (double*)malloc((size_t)W * H * sizeof(double));

    printf("field A: demo light (radial falloff + occluders)\n");
    if (compare_field(&m, comp) != 0) { free(comp); sh_free(&m); sh_free(&ramp); return 1; }
    printf("\nfield B: shallow vertical ramp 0.45..0.55, the 8-bit banding case\n");
    if (compare_field(&ramp, comp) != 0) { free(comp); sh_free(&m); sh_free(&ramp); return 1; }

    int rc = hand_check();

    free(comp);
    sh_free(&m);
    sh_free(&ramp);
    return rc == 0 ? 0 : 1;
}
