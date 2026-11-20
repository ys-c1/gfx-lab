/* 闪避基准：10s 与 60s 立体声素材，对照手算包络，报压缩深度误差、峰值、耗时。 */
#define _POSIX_C_SOURCE 199309L
#include "mixer.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

static float bg_sample(int i, float rate) {
    return 0.3f * sinf(2.0f * 3.14159265f * 440.0f * (float)i / rate);
}

static void run(int seconds, float rate, const MxDuck *cfg) {
    int frames = (int)(seconds * rate);
    float *main_trk = malloc((size_t)frames * 2 * sizeof(float));
    float *bg = malloc((size_t)frames * 2 * sizeof(float));
    if (!main_trk || !bg) { fprintf(stderr, "oom\n"); exit(1); }

    /* 素材：旁白为 0.9 幅度的 220Hz 短句（0.6s 说 / 0.4s 停），背景为 0.3 的 440Hz 持续音 */
    for (int i = 0; i < frames; ++i) {
        float t = (float)i / rate;
        float phase = t - floorf(t / 1.0f) * 1.0f;
        float m = phase < 0.6f ? 0.9f * sinf(2.0f * 3.14159265f * 220.0f * t) : 0.0f;
        main_trk[i*2+0] = m; main_trk[i*2+1] = m;
        bg[i*2+0] = bg[i*2+1] = bg_sample(i, rate);
    }

    float depth_db = -1.0f;
    double t0 = now_ms();
    int rc = mx_duck(bg, main_trk, frames, 2, rate, cfg, &depth_db);
    double elapsed = now_ms() - t0;
    if (rc != MX_DUCK_OK) { fprintf(stderr, "mx_duck rc=%d\n", rc); exit(1); }

    /* 手算包络对照：同一公式逐样本重算，得期望深度、期望输出与实测峰值 */
    float coef = expf(-1.0f / (cfg->release_ms * 0.001f * rate));
    float env = 0.0f, gmin = 1.0f, maxdiff = 0.0f, peak = 0.0f;
    for (int i = 0; i < frames; ++i) {
        float m = fabsf(main_trk[i*2]);
        env = m > env * coef ? m : env * coef;
        float x = env / cfg->level; if (x > 1.0f) x = 1.0f;
        float g = 1.0f - (1.0f - cfg->gain) * x;
        if (g < gmin) gmin = g;
        float expect = bg_sample(i, rate) * g;
        for (int c = 0; c < 2; ++c) {
            float diff = fabsf(bg[i*2+c] - expect);
            if (diff > maxdiff) maxdiff = diff;
            float a = fabsf(bg[i*2+c]);
            if (a > peak) peak = a;
        }
    }
    float expect_db = -20.0f * log10f(gmin);
    printf("%2ds  frames=%7d  depth=%8.4f dB  expect=%8.4f dB  err=%.2e dB  "
           "peak=%.4f  maxdiff=%.2e  time=%.2f ms\n",
           seconds, frames, depth_db, expect_db, fabsf(depth_db - expect_db),
           peak, maxdiff, elapsed);

    free(main_trk); free(bg);
}

int main(void) {
    const float rate = 48000.0f;
    MxDuck cfg = {0.5f, 0.2f, 200.0f, 1};
    run(10, rate, &cfg);
    run(60, rate, &cfg);
    return 0;
}
