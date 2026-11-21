/* 交叉馈送指标：2 声道与 8 声道分别测误差与耗时，
 * 馈送量、延迟、峰值、串音逐项与解析解对照，最后单列两档差异。 */
#define _POSIX_C_SOURCE 199309L
#include "mixer.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RATE 48000
#define FRAMES 4800
#define FEED 0.30f
#define DELAY_MS 0.5f                 /* => 24 采样点 */
#define DELAY_SAMPLES 24
#define AMP 0.8f
#define FREQ 1000.0f                  /* 48k 下周期 48 点，采样必过峰值 */
#define REPS 200

static double now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}

static float sig(int i) {
    return AMP * sinf(2.0f * 3.14159265f * FREQ * (float)i / (float)RATE);
}

typedef struct {
    int channels;
    double apply_us;                    /* 单次耗时 */
    float feed_meas, feed_err;          /* 馈送量：实测 vs 解析 */
    int delay_meas, delay_err;          /* 延迟：实测 vs 解析 */
    float peak_meas, peak_ana, peak_err;/* 峰值 */
    float xtalk_meas_db, xtalk_ana_db, xtalk_err_db; /* 串音 */
    float sample_max_err;               /* 全缓冲逐项最大误差 */
} Report;

static void fill_sine(MixBus *b, float *src) {
    /* 所有 L/R 对的 L 放正弦、R 放 0；8 声道即 4 对并行 */
    for (int i = 0; i < FRAMES; ++i)
        for (int c = 0; c < b->channels; ++c)
            src[(size_t)i * b->channels + c] = (c % 2 == 0) ? sig(i) : 0.0f;
    memset(b->buf, 0, (size_t)b->cap * b->channels * sizeof(float)); /* mx_add 是累加，先清零 */
    b->frames = 0;
    mx_add(b, src, FRAMES, 1.0f);
}

static Report run(int channels, float *src) {
    Report r; memset(&r, 0, sizeof r);
    r.channels = channels;
    MixBus b; mx_init(&b, FRAMES, channels);
    MxCrossfeedCfg cfg = { 1, FEED, DELAY_MS, RATE };
    MxCrossfeedStats st;

    /* --- 正弦主测：逐项对照解析解 --- */
    fill_sine(&b, src);
    mx_crossfeed_apply(&b, &cfg, &st, NULL, 0);
    r.delay_meas = st.delay_samples;
    r.delay_err = st.delay_samples - DELAY_SAMPLES;
    double sumL = 0, sumR = 0;
    float peak_ana = 0.0f;
    for (int i = 0; i < FRAMES; ++i) {
        float x = sig(i), xd = (i >= DELAY_SAMPLES) ? sig(i - DELAY_SAMPLES) : 0.0f;
        if (fabsf(x) > peak_ana) peak_ana = fabsf(x);
        float aR = FEED * xd;
        if (fabsf(aR) > peak_ana) peak_ana = fabsf(aR);
        for (int p = 0; p < channels / 2; ++p) {
            float mL = b.buf[(size_t)i * channels + 2 * p];
            float mR = b.buf[(size_t)i * channels + 2 * p + 1];
            float eL = fabsf(mL - x), eR = fabsf(mR - aR);
            if (eL > r.sample_max_err) r.sample_max_err = eL;
            if (eR > r.sample_max_err) r.sample_max_err = eR;
            if (i >= 2 * DELAY_SAMPLES && p == 0) {
                sumL += (double)mL * mL;
                sumR += (double)mR * mR;
            }
        }
    }
    r.peak_meas = st.peak_after;
    r.peak_ana = peak_ana;
    r.peak_err = fabsf(r.peak_meas - r.peak_ana);
    float xtalk = (float)sqrt(sumR / sumL);
    r.xtalk_meas_db = 20.0f * log10f(xtalk);
    r.xtalk_ana_db = 20.0f * log10f(FEED);
    r.xtalk_err_db = fabsf(r.xtalk_meas_db - r.xtalk_ana_db);

    /* --- 冲激测馈送量与延迟（解析：out_R[d]=feed） --- */
    memset(src, 0, (size_t)FRAMES * channels * sizeof(float));
    src[0] = 1.0f;
    memset(b.buf, 0, (size_t)b.cap * channels * sizeof(float));
    b.frames = 0;
    mx_add(&b, src, FRAMES, 1.0f);
    mx_crossfeed_apply(&b, &cfg, &st, NULL, 0);
    int k = 0;
    for (int i = 1; i < FRAMES; ++i)
        if (fabsf(b.buf[(size_t)i * channels + 1]) > fabsf(b.buf[(size_t)k * channels + 1])) k = i;
    r.feed_meas = b.buf[(size_t)k * channels + 1];
    r.feed_err = fabsf(r.feed_meas - FEED);
    if (k != DELAY_SAMPLES) r.delay_err = k - DELAY_SAMPLES; /* 冲激法复核延迟 */

    /* --- 耗时：只计 mx_crossfeed_apply，每次先恢复快照 --- */
    fill_sine(&b, src);
    size_t bytes = (size_t)FRAMES * channels * sizeof(float);
    float *clean = malloc(bytes);
    memcpy(clean, b.buf, bytes);
    double acc = 0.0;
    for (int rep = 0; rep < REPS; ++rep) {
        memcpy(b.buf, clean, bytes);
        double t0 = now_us();
        mx_crossfeed_apply(&b, &cfg, &st, NULL, 0);
        acc += now_us() - t0;
    }
    r.apply_us = acc / REPS;
    free(clean);
    mx_free(&b);
    return r;
}

static void print_report(const Report *r) {
    printf("== %d 声道 ==\n", r->channels);
    printf("  馈送量  实测 %.6f  解析 %.6f  误差 %.2e\n", r->feed_meas, FEED, r->feed_err);
    printf("  延迟    实测 %d 采样点  解析 %d 采样点  误差 %d\n",
           r->delay_meas, DELAY_SAMPLES, r->delay_err);
    printf("  峰值    实测 %.6f  解析 %.6f  误差 %.2e\n", r->peak_meas, r->peak_ana, r->peak_err);
    printf("  串音    实测 %.2f dB  解析 %.2f dB  误差 %.2e dB\n",
           r->xtalk_meas_db, r->xtalk_ana_db, r->xtalk_err_db);
    printf("  采样逐项最大误差 %.2e\n", r->sample_max_err);
    printf("  耗时    %.1f us/次（%d 帧 x %d 声道，%d 次平均）\n",
           r->apply_us, FRAMES, r->channels, REPS);
}

int main(void) {
    float *src = malloc((size_t)FRAMES * 8 * sizeof(float));
    if (!src) return 1;
    Report r2 = run(2, src);
    Report r8 = run(8, src);
    print_report(&r2);
    print_report(&r8);
    printf("== 2 声道 vs 8 声道 差异 ==\n");
    printf("  耗时比 8ch/2ch      %.2fx（差 %.1f us）\n",
           r8.apply_us / r2.apply_us, r8.apply_us - r2.apply_us);
    printf("  逐项最大误差差      %.2e\n", r8.sample_max_err - r2.sample_max_err);
    printf("  馈送量误差差        %.2e\n", r8.feed_err - r2.feed_err);
    printf("  峰值误差差          %.2e\n", r8.peak_err - r2.peak_err);
    printf("  串音误差差          %.2e dB\n", r8.xtalk_err_db - r2.xtalk_err_db);
    printf("  延迟误差差          %d 采样点\n", r8.delay_err - r2.delay_err);
    free(src);
    return 0;
}
