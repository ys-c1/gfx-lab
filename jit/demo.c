#define _POSIX_C_SOURCE 199309L
#include "jit.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define RATE 48000
#define N_VOICES 64
#define PERIOD  4800   /* 100 ms 重复周期 */
#define JIT_AMP 8.0    /* ±8 采样抖动 */
#define SEED    20261122u

/* 混合正弦素材，峰值归一化到 1 */
static float *make_material(int n) {
    float *s = (float*)malloc((size_t)n * sizeof(float));
    if (!s) return NULL;
    double mx = 0.0;
    for (int i = 0; i < n; ++i) {
        double t = (double)i / RATE;
        double v = 0.55 * sin(2 * M_PI * 220.0 * t)
                 + 0.30 * sin(2 * M_PI * 660.0 * t)
                 + 0.15 * sin(2 * M_PI * 1320.0 * t);
        s[i] = (float)v;
        double a = fabs(v);
        if (a > mx) mx = a;
    }
    for (int i = 0; i < n; ++i) s[i] = (float)(s[i] / mx);
    return s;
}

static double peak_abs(const float *b, int n) {
    double m = 0.0;
    for (int i = 0; i < n; ++i) { double a = fabs((double)b[i]); if (a > m) m = a; }
    return m;
}

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static int run_case(const char *tag, int src_len, FILE *rep) {
    float *src = make_material(src_len);
    if (!src) { fprintf(rep, "[%s] material allocation failed\n", tag); return 1; }

    /* ---- 无抖动基线 ---- */
    JitPlan base;
    if (jit_plan_init(&base, N_VOICES, src_len, PERIOD, 0.0, SEED) != JIT_OK) return 1;
    int L = jit_out_len(&base);
    float *ob = (float*)calloc((size_t)L, sizeof(float));
    double t0 = now_sec();
    jit_offsets(&base); jit_quantize(&base);
    int rc = jit_synthesize(&base, src, ob, L);
    double tbase = now_sec() - t0;
    if (rc != JIT_OK) { fprintf(rep, "[%s] base synth failed: %s\n", tag, jit_last_error()); return 1; }
    JitCombMetric mb;
    jit_comb_metric(&base, &mb);

    /* ---- 抖动（整数取整，本模块主路径）---- */
    JitPlan jp;
    if (jit_plan_init(&jp, N_VOICES, src_len, PERIOD, JIT_AMP, SEED) != JIT_OK) return 1;
    int Lj = jit_out_len(&jp);
    float *oj = (float*)calloc((size_t)Lj, sizeof(float));
    float *of = (float*)calloc((size_t)Lj, sizeof(float));
    t0 = now_sec();
    jit_offsets(&jp);
    jit_quantize(&jp);
    rc = jit_synthesize(&jp, src, oj, Lj);
    double tjit = now_sec() - t0;
    if (rc != JIT_OK) { fprintf(rep, "[%s] jit synth failed: %s\n", tag, jit_last_error()); return 1; }
    jit_synthesize_frac(&jp, src, of, Lj);
    JitCombMetric mj;
    jit_comb_metric(&jp, &mj);

    /* ---- 量峰值误差：整数取整 vs 连续参考，只比较共同有效区 ---- */
    int common = L < Lj ? L : Lj;
    double peak_b = peak_abs(ob, L), peak_j = peak_abs(oj, Lj), peak_f = peak_abs(of, Lj);
    double diff_max = 0.0, diff_sum = 0.0;
    for (int i = 0; i < common; ++i) {
        double d = fabs((double)oj[i] - (double)of[i]);
        if (d > diff_max) diff_max = d;
        diff_sum += d;
    }
    double peak_err = fabs(peak_j - peak_f);

    int jcount = jit_jitter_count(&jp);

    /* ---- 导出抖动序列 ---- */
    char csv[128];
    snprintf(csv, sizeof(csv), "demo_jitter_%s.csv", tag);
    FILE *cf = fopen(csv, "w");
    jit_export_csv(&jp, cf);
    fclose(cf);

    fprintf(rep, "================ case %s ================\n", tag);
    fprintf(rep, "material length   : %d samples (%.3f s)\n", src_len, (double)src_len / RATE);
    fprintf(rep, "voices N          : %d, period P = %d samples (%.1f ms)\n",
            N_VOICES, PERIOD, 1000.0 * PERIOD / RATE);
    fprintf(rep, "jitter amplitude A: %.1f samples (jitter length in output = 2*ceil(A) = %d samples)\n",
            JIT_AMP, 2 * (int)ceil(JIT_AMP));
    fprintf(rep, "output length     : base %d / jittered %d samples\n", L, Lj);
    fprintf(rep, "jitter count      : %d of %d voices moved after rounding (%.1f%%)\n",
            jcount, N_VOICES, 100.0 * jcount / N_VOICES);
    fprintf(rep, "peak |base|       : %.6f\n", peak_b);
    fprintf(rep, "peak |integer|    : %.6f\n", peak_j);
    fprintf(rep, "peak |fractional| : %.6f\n", peak_f);
    fprintf(rep, "peak error (int-frac): %.6e (%.3f dB rel. to frac peak)\n",
            peak_err, 20.0 * log10((peak_err + 1e-300) / peak_f));
    fprintf(rep, "max |int-frac| sample diff over %d common samples: %.6e, mean: %.6e\n",
            common, diff_max, diff_sum / common);
    fprintf(rep, "elapsed base      : %.3f ms\n", tbase * 1e3);
    fprintf(rep, "elapsed jittered  : %.3f ms (stages 1-3, incl. offsets+quantize)\n", tjit * 1e3);
    fprintf(rep, "comb metric base  : crest=%7.2f dB  peak_bin=%4d  above_half=%4d/%d  flatness=%.5f\n",
            mb.crest_db, mb.peak_bin, mb.above_half, mb.bins, mb.flatness);
    fprintf(rep, "comb metric jitter: crest=%7.2f dB  peak_bin=%4d  above_half=%4d/%d  flatness=%.5f\n",
            mj.crest_db, mj.peak_bin, mj.above_half, mj.bins, mj.flatness);
    fprintf(rep, "comb reduction    : crest %.2f -> %.2f dB (delta %.2f dB), flatness x%.2f\n",
            mb.crest_db, mj.crest_db, mb.crest_db - mj.crest_db,
            mj.flatness / (mb.flatness > 1e-12 ? mb.flatness : 1e-12));
    fprintf(rep, "sequence export   : %s (%d rows)\n\n", csv, N_VOICES);

    printf("[%s] jcount=%d/%d peak_err=%.3e base_crest=%.1fdB jit_crest=%.1fdB time=%.2fms\n",
           tag, jcount, N_VOICES, peak_err, mb.crest_db, mj.crest_db, tjit * 1e3);

    free(src); free(ob); free(oj); free(of);
    jit_plan_free(&base); jit_plan_free(&jp);
    return 0;
}

int main(void) {
    FILE *rep = fopen("demo_report.txt", "w");
    if (!rep) { printf("cannot open report\n"); return 1; }
    fprintf(rep, "jit module demo — jittered overlay vs periodic stacking\n");
    fprintf(rep, "rate=%d Hz, seed=%u, generated by demo.c\n\n", RATE, SEED);
    int rc = 0;
    rc |= run_case("1s", RATE, rep);
    rc |= run_case("10s", RATE * 10, rep);
    fclose(rep);
    printf("report written to demo_report.txt\n");
    return rc;
}
