/* benchmark: 16/64-track mix with limiter, raw f32 export for python cross-check */
#define _POSIX_C_SOURCE 200809L
#include "mixer.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define FRAMES 24000
#define CH 2
#define SR 48000

static unsigned long rng_state = 0x12345678UL;
static float rngf(void) {
    rng_state = rng_state * 1103515245UL + 12345UL;
    return (float)((rng_state >> 8) & 0xffffff) / (float)0x1000000 * 2.0f - 1.0f;
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static int dump_f32(const char *path, const float *p, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    if (fwrite(p, sizeof(float), n, f) != n) { fclose(f); return -1; }
    fclose(f);
    return 0;
}

static void run_case(int tracks, const MxLimitCfg *cfg) {
    size_t samples = (size_t)FRAMES * CH;
    MixBus bus, ref;
    if (mx_init(&bus, FRAMES, CH) || mx_init(&ref, FRAMES, CH)) { printf("init fail\n"); exit(1); }

    float *trk = (float*)malloc(samples * sizeof(float));
    float *mix = (float*)calloc(samples, sizeof(float));
    if (!trk || !mix) { printf("oom\n"); exit(1); }
    for (int t = 0; t < tracks; ++t) {
        double f0 = 220.0 * (1.0 + 0.07 * t);
        float phase = t * 0.37f;
        for (int i = 0; i < FRAMES; ++i) {
            float env = 0.8f + 0.2f * sinf(0.5f * t + i * 0.003f);
            /* strong shared coherent tone + weak per-track tone + noise;
             * the shared term makes the N-track sum scale with N and clip */
            float common = 0.08f * sinf(2.0f * (float)M_PI * 110.0f * i / SR);
            float own = 0.02f * sinf(2.0f * (float)M_PI * (float)f0 * i / SR + phase);
            float noise = 0.01f * rngf();
            float s = env * (common + own + noise);
            trk[i * CH] = s;
            trk[i * CH + 1] = s * (0.9f + 0.1f * rngf());
        }
        for (size_t k = 0; k < samples; ++k) mix[k] += trk[k];
    }
    /* one add: all tracks occupy the same frames and are truly summed */
    if (mx_add_ex(&bus, mix, FRAMES, CH, 1.0f) != FRAMES ||
        mx_add_ex(&ref, mix, FRAMES, CH, 1.0f) != FRAMES) {
        printf("add fail\n"); exit(1);
    }

    /* timing: median of REPS limiter runs on fresh copies */
    const int REPS = 11;
    double times[REPS];
    float *scratch = (float*)malloc(samples * sizeof(float));
    MxLimitReport rep = {0};
    for (int r = 0; r < REPS; ++r) {
        memcpy(scratch, ref.buf, samples * sizeof(float));
        MixBus work;
        mx_init(&work, FRAMES, CH);
        memcpy(work.buf, scratch, samples * sizeof(float));
        work.frames = FRAMES;
        double t0 = now_s();
        int rc = mx_limit(&work, cfg, &rep);
        double t1 = now_s();
        if (rc != MX_OK) { printf("limit fail: %s\n", mx_err_str(rc)); exit(1); }
        times[r] = t1 - t0;
        if (r == REPS - 1) memcpy(bus.buf, work.buf, samples * sizeof(float));
        mx_free(&work);
    }
    for (int i = 0; i < REPS; ++i)
        for (int j = i + 1; j < REPS; ++j)
            if (times[j] < times[i]) { double tmp = times[i]; times[i] = times[j]; times[j] = tmp; }

    /* bypass path must be bit-identical to the raw mix */
    MixBus chk;
    mx_init(&chk, FRAMES, CH);
    memcpy(chk.buf, ref.buf, samples * sizeof(float));
    chk.frames = FRAMES;
    MxLimitCfg off = *cfg; off.enabled = 0;
    if (mx_limit(&chk, &off, NULL) != MX_OK ||
        memcmp(chk.buf, ref.buf, samples * sizeof(float)) != 0) {
        printf("FAIL: bypass altered samples\n"); exit(1);
    }
    mx_free(&chk);

    char path[128];
    snprintf(path, sizeof(path), "build/mix_%d_in.f32", tracks);
    dump_f32(path, ref.buf, samples);
    snprintf(path, sizeof(path), "build/mix_%d_out.f32", tracks);
    dump_f32(path, bus.buf, samples);

    printf("tracks=%d  peak_before=%.6f  peak_after=%.6f  plateau_runs=%d "
           "plateau_samples=%d  limited=%d  median=%.3f ms\n",
           tracks, rep.peak_before, rep.peak_after,
           rep.plateau_runs, rep.plateau_samples, rep.limited_samples,
           times[REPS / 2] * 1e3);

    free(trk); free(mix); free(scratch);
    mx_free(&bus); mx_free(&ref);
}

int main(void) {
    MxLimitCfg cfg = { 1.0f, SR, 1.0f, 20.0f, 1 };
    FILE *meta = fopen("build/bench_meta.txt", "w");
    fprintf(meta, "frames=%d channels=%d sample_rate=%d threshold=%.6f "
            "attack_ms=%.3f release_ms=%.3f\n",
            FRAMES, CH, SR, cfg.threshold, cfg.attack_ms, cfg.release_ms);
    fclose(meta);
    run_case(16, &cfg);
    run_case(64, &cfg);
    return 0;
}
