#define _POSIX_C_SOURCE 199309L
#include "mixer.h"
#include "deess.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

#define SR 48000
#define FRAMES 19200
#define PI 3.14159265358979323846

static double now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static void report(const char *tag, const char *item, double analytic, double measured) {
    printf("  [%s] %-16s analytic=%10.6f measured=%10.6f err=%.6f\n",
           tag, item, analytic, measured, fabs(analytic - measured));
}

/* 解析解: 稳态时 band==full(纯带内正弦), ratio=1, g=thresh/ratio 的 slope 次幂倒数 */
static double band_f0(const DsCfg *c) { return sqrt((double)c->band_lo_hz * c->band_hi_hz); }

static void test_validate(void) {
    char errs[16][96];
    DsCfg good = ds_default_cfg(SR);
    CK(ds_validate(&good, errs, 16) == 0);

    DsCfg bad = good;
    bad.band_lo_hz = 8000.0f; bad.band_hi_hz = 5000.0f;   /* 带空 */
    bad.thresh_ratio = 1.5f;                              /* 越界 */
    bad.max_att_db = -3.0f;                               /* 非正 */
    bad.hang_ms = -1.0f;                                  /* 负 */
    int n = ds_validate(&bad, errs, 16);
    CK(n == 4);                                           /* 冲突项必须全部列出 */
    int seen_lo = 0, seen_th = 0, seen_att = 0, seen_hang = 0;
    for (int i = 0; i < n && i < 16; ++i) {
        printf("  [validate] conflict: %s\n", errs[i]);
        if (strstr(errs[i], "band_hi_hz")) ++seen_lo;
        if (strstr(errs[i], "thresh_ratio")) ++seen_th;
        if (strstr(errs[i], "max_att_db")) ++seen_att;
        if (strstr(errs[i], "hang_ms")) ++seen_hang;
    }
    CK(seen_lo && seen_th && seen_att && seen_hang);
    CK(ds_create(&bad, 2) == NULL);                       /* 非法组合必须拒绝 */

    DsCfg nyq = good; nyq.band_hi_hz = 24000.0f;          /* 触 Nyquist */
    CK(ds_validate(&nyq, errs, 16) == 1);
    CK(ds_create(&nyq, 2) == NULL);
}

static void test_passthrough(void) {
    DsCfg c = ds_default_cfg(SR);
    c.enabled = 0;
    c.band_lo_hz = -1.0f; c.band_hi_hz = -2.0f;           /* 旁通时垃圾参数也不得影响 */
    DsCtx *d = ds_create(&c, 2);
    CK(d != NULL);
    float in[512], out[512];
    for (int i = 0; i < 512; ++i) in[i] = (float)((i * 2654435761u) % 2000) / 1000.0f - 1.0f;
    DsStats st; DsSeg segs[4];
    CK(ds_process(d, in, out, 256, segs, 4, &st) == 0);
    CK(memcmp(in, out, sizeof(in)) == 0);                 /* 不开启时逐位一致 */
    CK(st.segments_total == 0 && st.peak_in == 0.0f && st.peak_out == 0.0f);
    ds_destroy(d);

    /* 开启但无齿音(200Hz 正弦): 增益恒 1, 输出同样逐位一致 */
    DsCfg e = ds_default_cfg(SR);
    DsCtx *d2 = ds_create(&e, 2);
    CK(d2 != NULL);
    for (int i = 0; i < 256; ++i) {
        float v = 0.4f * sinf(2.0f * (float)PI * 200.0f * (float)i / SR);
        in[i*2] = v; in[i*2+1] = v;
    }
    CK(ds_process(d2, in, out, 256, segs, 4, &st) == 0);
    CK(memcmp(in, out, 256 * 2 * sizeof(float)) == 0);
    CK(st.segments_total == 0);
    ds_destroy(d2);
}

static void run_case(const char *tag, int chs, float thresh,
                     void (*fill)(float *buf, double f0),
                     double exp_ratio_ch5 /* <0 表示无第二段 */) {
    DsCfg c = ds_default_cfg(SR);
    c.thresh_ratio = (float)thresh;
    double f0 = band_f0(&c);
    float *in = (float*)calloc((size_t)FRAMES * chs, sizeof(float));
    float *out = (float*)malloc((size_t)FRAMES * chs * sizeof(float));
    CK(in && out);
    fill(in, f0);

    DsCtx *d = ds_create(&c, chs);
    CK(d != NULL);
    DsSeg segs[16]; DsStats st;
    double t0 = now_ms();
    int rc = ds_process(d, in, out, FRAMES, segs, 16, &st);
    double t1 = now_ms();
    CK(rc == 0);

    /* 段2 解析解: att_db = 20*log10(ratio/thresh), g = thresh/ratio (slope=1) */
    double att0 = 20.0 * log10(1.0 / thresh);             /* ch0 纯带内: ratio=1 */
    double g0 = thresh;
    int exp_segs = exp_ratio_ch5 > 0.0 ? 2 : 1;
    printf("[%s %dch] segments=%d max_att=%.4fdB peak_in=%.4f peak_out=%.4f time=%.3fms (%.1f ns/frame/ch)\n",
           tag, chs, st.segments_total, st.max_att_db, st.peak_in, st.peak_out,
           t1 - t0, (t1 - t0) * 1e6 / FRAMES / chs);
    CK(st.segments_total == exp_segs);
    CK(st.segments == exp_segs);
    report(tag, "segments", exp_segs, st.segments_total);
    report(tag, "peak_in", 0.5, st.peak_in);
    CK(fabs(st.peak_in - 0.5) < 1e-4);
    CK(st.peak_out <= st.peak_in + 1e-6f);                /* de-esser 只压不抬 */

    const DsSeg *s0 = NULL, *s5 = NULL;
    for (int i = 0; i < st.segments; ++i) {
        if (segs[i].channel == 0) s0 = &segs[i];
        if (segs[i].channel == 5) s5 = &segs[i];
    }
    CK(s0 != NULL);
    if (s0) {
        report(tag, "ch0 att_db", att0, s0->att_db);
        CK(fabs(s0->att_db - att0) < 0.3);
        CK(s0->start_frame >= 4800 && s0->start_frame < 4800 + 960);
        CK(s0->end_frame > 9600);
    }
    /* 稳态区(6000..9000): out == in * g */
    double peak = 0.0, serr = 0.0;
    for (int i = 6000; i < 9000; ++i) {
        double o = out[(size_t)i * chs];
        double e = fabs(o - in[(size_t)i * chs] * g0);
        if (fabs(o) > peak) peak = fabs(o);
        if (e > serr) serr = e;
    }
    report(tag, "ch0 steady_peak", 0.5 * g0, peak);
    report(tag, "ch0 sample_err", 0.0, serr);
    CK(fabs(peak - 0.5 * g0) < 2e-3);
    CK(serr < 2e-3);

    if (exp_ratio_ch5 > 0.0) {                            /* 8ch: ch5 混合信号段 */
        double att5 = 20.0 * log10(exp_ratio_ch5 / thresh);
        double g5 = thresh / exp_ratio_ch5;
        CK(s5 != NULL);
        if (s5) {
            report(tag, "ch5 ratio", exp_ratio_ch5, s5->peak_ratio);
            report(tag, "ch5 att_db", att5, s5->att_db);
            CK(fabs(s5->peak_ratio - exp_ratio_ch5) < 0.02);
            CK(fabs(s5->att_db - att5) < 0.5);
        }
        double peak5 = 0.0;
        for (int i = 6000; i < 9000; ++i) {
            double o = fabs(out[(size_t)i * chs + 5]);
            if (o > peak5) peak5 = o;
        }
        report(tag, "ch5 steady_peak", 0.25 * g5 + 0.25, peak5);
        CK(fabs(peak5 - (0.25 * g5 + 0.25)) < 0.02);
    }

    /* 段外(齿音未检出区)输出必须与输入逐位一致 */
    int ident = 1;
    for (int i = 0; i < 4000 && ident; ++i)
        if (memcmp(&in[(size_t)i * chs], &out[(size_t)i * chs], (size_t)chs * sizeof(float)) != 0) ident = 0;
    CK(ident);

    ds_destroy(d);
    free(in); free(out);
}

static void fill_2ch(float *buf, double f0) {
    for (int i = 4800; i < 9600; ++i) {
        float v = 0.5f * (float)sin(2.0 * PI * f0 * (double)i / SR);
        buf[(size_t)i * 2] = v; buf[(size_t)i * 2 + 1] = 0.0f;
    }
}

static void fill_8ch(float *buf, double f0) {
    /* ch5: 带内 0.25 + 直流 0.25 -> 均方比 = (A^2/2)/(A^2/2+B^2) = 1/3, 比值 sqrt(1/3) */
    for (int i = 4800; i < 9600; ++i) {
        buf[(size_t)i * 8] = 0.5f * (float)sin(2.0 * PI * f0 * (double)i / SR);
        buf[(size_t)i * 8 + 5] = 0.25f * (float)sin(2.0 * PI * f0 * (double)i / SR) + 0.25f;
    }
}

static void test_export(void) {
    DsCfg c = ds_default_cfg(SR);
    double f0 = band_f0(&c);
    float *in = (float*)calloc((size_t)FRAMES * 2, sizeof(float));
    float *out = (float*)malloc((size_t)FRAMES * 2 * sizeof(float));
    fill_2ch(in, f0);
    DsCtx *d = ds_create(&c, 2);
    CK(d != NULL);
    CK(ds_process(d, in, out, FRAMES, NULL, 0, NULL) == 0);
    CK(ds_export_csv(d, "build/deess_dist.csv") == 0);
    ds_destroy(d);
    free(in); free(out);

    FILE *f = fopen("build/deess_dist.csv", "r");
    CK(f != NULL);
    if (!f) return;
    char line[256];
    long rows = 0;
    int checked = 0;
    while (fgets(line, sizeof(line), f)) {
        ++rows;
        int fr, ch; float be, fe, ra, ga;
        if (sscanf(line, "%d,%d,%f,%f,%f,%f", &fr, &ch, &be, &fe, &ra, &ga) == 6
            && fr == 7000 && ch == 0) {                   /* 稳态区逐样本对照 */
            report("csv", "ratio@f7000ch0", 1.0, ra);
            report("csv", "gain@f7000ch0", 0.5, ga);
            CK(fabs(ra - 1.0f) < 0.02f);
            CK(fabs(ga - 0.5f) < 0.01f);
            ++checked;
        }
    }
    fclose(f);
    CK(rows == 1 + (long)FRAMES * 2);                     /* 头 + 每帧每声道一行 */
    CK(checked == 1);
    printf("[export] build/deess_dist.csv rows=%ld (齿音分布已导出)\n", rows);
}

int main(void) {
    MixBus b;
    CK(mx_init(&b, 8, 2) == 0);
    float a[8], cc[8];
    for (int i = 0; i < 8; ++i) { a[i] = 0.25f; cc[i] = 0.5f; }
    CK(mx_add(&b, a, 4, 1.0f) == 4);
    CK(mx_add(&b, cc, 4, 0.5f) == 4);
    CK(fabsf(b.buf[0] - 0.25f) < 1e-6f);
    CK(fabsf(b.buf[8] - 0.25f) < 1e-6f);
    CK(mx_add(&b, a, 4, 1.0f) == 0);
    mx_free(&b);

    test_validate();
    test_passthrough();
    run_case("deess", 2, 0.5, fill_2ch, -1.0);
    run_case("deess", 8, 0.4, fill_8ch, 0.5773502691896258); /* sqrt(1/3) */
    test_export();

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
