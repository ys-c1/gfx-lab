#define _POSIX_C_SOURCE 199309L
#include "mixer.h"
#include "softclip.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

#define SR 48000
#define FRAMES 4800            /* 100 ms */
#define KNEE 0.8f
#define CEIL 1.0f
#define TABS 1024
#define ITERS 100
#define OUTDIR "../artifacts/softclip"

static float *make_signal(int frames, int ch, float level) {
    float *s = (float*)malloc((size_t)frames * ch * sizeof(float));
    if (!s) return NULL;
    for (int i = 0; i < frames; ++i) {
        float v = level * sinf(2.0f * 3.14159265f * 440.0f * (float)i / SR);
        for (int c = 0; c < ch; ++c) s[(size_t)i * ch + c] = v;
    }
    return s;
}

static float peak_of(const float *s, long n) {
    float p = 0.0f;
    for (long i = 0; i < n; ++i) {
        float a = fabsf(s[i]);
        if (a > p) p = a;
    }
    return p;
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void write_wav_path(const float *buf, int frames, int ch, const char *path) {
    MixBus tmp;
    if (mx_init(&tmp, frames, ch) != 0) return;
    mx_add(&tmp, buf, frames, 1.0f);
    mx_write_wav(&tmp, path);
    mx_free(&tmp);
}

static void run_round(const SoftClip *sc, float level, int ch, FILE *rep) {
    long n = (long)FRAMES * ch;
    float *sig = make_signal(FRAMES, ch, level);
    float *work = (float*)malloc((size_t)n * sizeof(float));
    if (!sig || !work) { free(sig); free(work); return; }

    /* ---- 关闭路径 (直通) ---- */
    memcpy(work, sig, (size_t)n * sizeof(float));
    SoftClip off = {0};
    sc_apply(&off, work, FRAMES, ch);
    float peak_in = peak_of(work, n);
    char path[256];
    snprintf(path, sizeof path, "%s/round_lvl%.1f_ch%d_off.wav",
             OUTDIR, level, ch);
    write_wav_path(work, FRAMES, ch, path);

    /* ---- 开启路径: 计时 ITERS 轮 sc_apply (每轮恢复输入) ---- */
    double best = 1e9, t0, t1;
    for (int it = 0; it < ITERS; ++it) {
        memcpy(work, sig, (size_t)n * sizeof(float));
        t0 = now_s();
        sc_apply(sc, work, FRAMES, ch);
        t1 = now_s();
        if (t1 - t0 < best) best = t1 - t0;
    }
    float peak_out = peak_of(work, n);

    /* 逐样本 LUT vs 解析解 量误差 + int16 量化误差 */
    double lut_max = 0.0, lut_sse = 0.0, q16_max = 0.0, q16_sse = 0.0;
    long clip_samples = 0;
    for (long i = 0; i < n; ++i) {
        float ya = sc_analytic(KNEE, CEIL, sig[i]);
        float d = fabsf(work[i] - ya);
        if (d > lut_max) lut_max = d;
        lut_sse += (double)d * d;

        short q = (short)lrintf(work[i] * 32767.0f);
        float back = (float)q / 32767.0f;
        float dq = fabsf(back - work[i]);
        if (dq > q16_max) q16_max = dq;
        q16_sse += (double)dq * dq;

        if (fabsf(sig[i]) >= KNEE) ++clip_samples;
    }

    /* 削波段数: 每声道连续 |x| >= knee 的游程; 拐点位置: 首个越界帧 */
    long segments = 0;
    int first_frame = -1;
    for (int c = 0; c < ch; ++c) {
        int in_seg = 0;
        for (int i = 0; i < FRAMES; ++i) {
            int over = fabsf(sig[(size_t)i * ch + c]) >= KNEE;
            if (over && !in_seg) { ++segments; in_seg = 1;
                if (first_frame < 0 || i < first_frame) first_frame = i;
            } else if (!over) in_seg = 0;
        }
    }

    snprintf(path, sizeof path, "%s/round_lvl%.1f_ch%d_on.wav",
             OUTDIR, level, ch);
    write_wav_path(work, FRAMES, ch, path);

    double us = best * 1e6, ns_per = best * 1e9 / (double)n;
    printf("level=%.1f ch=%d | peak in=%.6f out=%.6f | knee T=%.2f first@%d (%.3f ms) | "
           "clip samples=%ld segments=%ld | LUT err max=%.3e rms=%.3e | "
           "int16 q err max=%.3e rms=%.3e | %.1f us total, %.2f ns/sample\n",
           level, ch, peak_in, peak_out, KNEE, first_frame,
           first_frame >= 0 ? 1000.0 * first_frame / SR : 0.0,
           clip_samples, segments,
           lut_max, sqrt(lut_sse / (double)n),
           q16_max, sqrt(q16_sse / (double)n), us, ns_per);
    fprintf(rep, "level=%.1f ch=%d peak_in=%.6f peak_out=%.6f knee=%.2f "
            "first_frame=%d first_ms=%.3f clip_samples=%ld segments=%ld "
            "lut_max=%.6e lut_rms=%.6e q16_max=%.6e q16_rms=%.6e "
            "time_us=%.3f ns_per_sample=%.3f\n",
            level, ch, peak_in, peak_out, KNEE, first_frame,
            first_frame >= 0 ? 1000.0 * first_frame / SR : 0.0,
            clip_samples, segments,
            lut_max, sqrt(lut_sse / (double)n),
            q16_max, sqrt(q16_sse / (double)n), us, ns_per);

    free(sig); free(work);
}

int main(void) {
    /* ---- 兼容部分: 与旧 demo 完全相同的信号与输出 (软削波关闭) ---- */
    MixBus b;
    if (mx_init(&b, 4800, 2) != 0) return 1;
    float legacy[960];
    for (int i = 0; i < 480; ++i) {
        float s = 0.3f * sinf(2.0f * 3.14159265f * 440.0f * (float)i / 48000.0f);
        legacy[i*2+0] = s; legacy[i*2+1] = s;
    }
    SoftClip off = {0};
    sc_apply(&off, legacy, 480, 2);          /* 未启用: 直通, 缓冲不变 */
    mx_add(&b, legacy, 480, 1.0f);
    int rc = mx_write_wav(&b, "demo.wav");
    mx_free(&b);
    if (rc != 0) return 1;
    printf("legacy demo.wav written (soft clip disabled, passthrough)\n");

    SoftClip sc;
    unsigned conflicts = 0;
    if (sc_build_curve(&sc, KNEE, CEIL, TABS, &conflicts) != 0) {
        printf("curve build failed, conflicts=0x%x\n", conflicts);
        return 1;
    }
#ifdef _WIN32
    _mkdir(OUTDIR);
#else
    mkdir(OUTDIR, 0755);
#endif

    FILE *rep = fopen(OUTDIR "/report.txt", "w");
    if (!rep) { sc_free(&sc); return 1; }

    /* 两档输入各跑一轮, 每档 2 与 8 声道 */
    float levels[2] = {0.6f, 2.0f};
    int chans[2] = {2, 8};
    for (int li = 0; li < 2; ++li)
        for (int ci = 0; ci < 2; ++ci)
            run_round(&sc, levels[li], chans[ci], rep);

    /* 曲线表导出: 稠密采样, 含解析解与 LUT 两列 */
    sc_export_csv(&sc, OUTDIR "/curve.csv", 2048, 4.0f);
    printf("curve table -> %s/curve.csv (2048 rows, x in [-4,4])\n", OUTDIR);

    fclose(rep);
    sc_free(&sc);
    return 0;
}
