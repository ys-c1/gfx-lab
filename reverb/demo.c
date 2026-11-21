#define _POSIX_C_SOURCE 200809L
#include "reverb.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RATE    48000
#define NTAPS   6
#define DECAY   0.35f          /* RT60, seconds                       */
#define CHANNELS 2

static const int   DELAY[NTAPS] = {191, 269, 373, 449, 541, 631};
static const float GAIN[NTAPS]  = {0.90f, 0.80f, 0.70f, 0.60f, 0.50f, 0.40f};
static const float DRY = 0.8f, WET = 0.6f;

static double now_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e6 + (double)t.tv_nsec * 1e-3;
}

static float peak_abs(const float *buf, size_t n) {
    float p = 0.0f;
    for (size_t i = 0; i < n; ++i)
        if (fabsf(buf[i]) > p) p = fabsf(buf[i]);
    return p;
}

/* A 10 ms decaying 1.8 kHz burst: one snare-ish hit, identical on both
 * channels.  `frames` is the whole input block, so a small block cuts the
 * hit short on purpose. */
static void build_input(float *in, int frames, int channels) {
    memset(in, 0, (size_t)frames * (size_t)channels * sizeof(float));
    int hit = frames < 480 ? frames : 480;
    for (int i = 0; i < hit; ++i) {
        float s = 0.9f * sinf(2.0f * 3.14159265f * 1800.0f * (float)i / RATE)
                * expf(-(float)i / 120.0f);
        for (int c = 0; c < channels; ++c)
            in[(size_t)i * channels + c] = c == 0 ? s : 0.9f * s;
    }
}

/* Hand table value for the wet path at frame n, from the public fields:
 * hand(n) = wet * sum_t gain_t * fb_t^(n/delay_t - 1)  where delay_t | n. */
static double hand_value(const Reverb *r, int n) {
    double v = 0.0;
    if (n > 0)
        for (int t = 0; t < r->taps; ++t) {
            int d = r->delay[t];
            if (d <= 0 || n % d != 0) continue;
            v += (double)r->gain[t] * pow((double)r->fb[t], (double)(n / d - 1));
        }
    return (double)r->wet * v;
}

static int write_wav(const char *path, const float *buf, int frames, int channels) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    unsigned int data_bytes = (unsigned int)((size_t)frames * (size_t)channels * 2);
    unsigned int riff = 36 + data_bytes;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    unsigned int sz = 16; fwrite(&sz, 4, 1, f);
    unsigned short fmt = 1, ch = (unsigned short)channels;
    unsigned int rate = RATE, byte_rate = rate * ch * 2;
    unsigned short align = (unsigned short)(ch * 2), bits = 16;
    fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f);
    fwrite(&byte_rate, 4, 1, f); fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data_bytes, 4, 1, f);
    for (size_t i = 0; i < (size_t)frames * (size_t)channels; ++i) {
        float v = buf[i];
        if (v > 1.0f) v = 1.0f;
        if (v < -1.0f) v = -1.0f;
        short s = (short)lrintf(v * 32767.0f);
        fwrite(&s, 2, 1, f);
    }
    fclose(f);
    return 0;
}

static int make(Reverb *r) {
    return rv_init(r, CHANNELS, RATE, NTAPS, DELAY, GAIN, DECAY);
}

/* One size: small (shorter than the tail) or large (much longer). */
static int run_case(const char *label, int frames) {
    int ch = CHANNELS;
    size_t in_n = (size_t)frames * (size_t)ch;
    float *in = (float*)malloc(in_n * sizeof(float));
    float *old = (float*)malloc(in_n * sizeof(float));
    if (!in || !old) { free(in); free(old); return 1; }
    build_input(in, frames, ch);

    Reverb r;
    if (make(&r) != RV_OK) { free(in); free(old); return 1; }
    rv_set_mix(&r, DRY, WET);

    int tail = 0;
    int rc = rv_tail_len(RATE, DECAY, &tail);
    if (rc != RV_OK) { printf("tail_len failed: %s\n", rv_strerror(rc)); return 1; }
    size_t out_n = (size_t)(frames + tail) * (size_t)ch;
    float *full = (float*)malloc(out_n * sizeof(float));
    float *staged = (float*)malloc(out_n * sizeof(float));
    if (!full || !staged) { free(in); free(old); free(full); free(staged); return 1; }

    /* --- old behaviour: input only, tail dropped --- */
    double t0 = now_us();
    rc = rv_process_legacy(&r, in, frames, old);
    if (rc != frames) { printf("legacy failed: %s\n", rv_strerror(rc)); return 1; }
    rv_reset(&r);

    /* --- three stages, explicitly --- */
    float *padded = (float*)malloc(out_n * sizeof(float));
    if (!padded) { free(in); free(old); free(full); free(staged); return 1; }
    t0 = now_us();
    int np = rv_pad(in, frames, ch, tail, padded);                 /* stage 2 */
    int sr = rv_sum(&r, padded, np, staged);                       /* stage 3 */
    if (np != frames + tail || sr != RV_OK) {
        printf("stages failed: %s\n", rv_strerror(np < 0 ? np : sr));
        return 1;
    }
    rv_reset(&r);

    /* --- the one-call path must agree with the staged path exactly --- */
    int derived = 0;
    t0 = now_us();
    rc = rv_process(&r, in, frames, full, &derived);               /* 1+2+3 */
    if (rc != frames + tail || derived != tail) {
        printf("process failed: %s\n", rv_strerror(rc));
        return 1;
    }
    int staged_identical = memcmp(full, staged, out_n * sizeof(float)) == 0;
    int prefix_identical = memcmp(full, old, in_n * sizeof(float)) == 0;

    /* --- what the old path truncated --- */
    int cut_frames = 0, cut_loud = 0;
    float cut_peak = 0.0f;
    for (int n = frames; n < frames + tail; ++n) {
        float a = 0.0f;
        for (int c = 0; c < ch; ++c) {
            float v = fabsf(full[(size_t)n * ch + c]);
            if (v > a) a = v;
        }
        if (a > 0.0f) ++cut_frames;
        if (a > 1e-3f) ++cut_loud;      /* audible: > -60 dBFS */
        if (a > cut_peak) cut_peak = a;
    }

    /* --- decay curve: impulse response vs the closed-form hand table ---
     * The closed form is only defined for an impulse, so this part runs its own
     * impulse through the same network (dry = 0, wet = 1) and compares every
     * row and both channels with rv_export_curve. */
    int irn = tail + 1;
    float *ir = (float*)malloc((size_t)irn * (size_t)ch * sizeof(float));
    float *imp = (float*)calloc((size_t)irn * (size_t)ch, sizeof(float));
    if (!ir || !imp) return 1;
    for (int c = 0; c < ch; ++c) imp[c] = 1.0f;     /* one impulse per channel */
    Reverb irr;
    if (make(&irr) != RV_OK) return 1;
    rv_set_mix(&irr, 0.0f, 1.0f);                   /* isolate the wet tail */
    if (rv_sum(&irr, imp, irn, ir) != RV_OK) return 1;

    char path[64];
    snprintf(path, sizeof(path), "tail_curve_%s.csv", label);
    RvCurveStats st;
    rc = rv_export_curve(&irr, ir, irn, path, 1e-6, &st);
    if (rc != RV_OK) { printf("curve export failed: %s\n", rv_strerror(rc)); return 1; }

    /* Per-tap hand values at the first few arrival frames, so the CSV can be
     * checked line by line: hand(n) = wet * sum gain_t * fb_t^(n/delay_t - 1). */
    int shown = 0;
    printf("  curve rows        : frame   hand        measured     taps\n");
    for (int n = 0; n < irn && shown < 6; ++n) {
        double hand = hand_value(&irr, n);
        if (fabs(hand) < 1e-12) continue;
        double measured = (double)ir[(size_t)n * ch];
        int taps = 0;
        for (int t = 0; t < irr.taps; ++t)
            if (irr.delay[t] > 0 && n % irr.delay[t] == 0) ++taps;
        printf("                      %6d  %.8f  %.8f   %d\n", n, hand, measured, taps);
        ++shown;
    }
    rv_free(&irr);

    /* --- per-block timing, averaged --- */
    int trials = frames > 100000 ? 8 : 400;
    t0 = now_us();
    for (int k = 0; k < trials; ++k) {
        rv_reset(&r);
        rv_process_legacy(&r, in, frames, old);
    }
    double us_old = (now_us() - t0) / trials;
    t0 = now_us();
    for (int k = 0; k < trials; ++k) {
        rv_reset(&r);
        rv_process(&r, in, frames, full, &derived);
    }
    double us_full = (now_us() - t0) / trials;

    printf("\n=== %s: %d frames (%.2f ms), %d ch ===\n",
           label, frames, 1000.0 * frames / RATE, ch);
    printf("  tail length       : %d frames (%.2f ms), from RT60 %.2f s @ %d Hz\n",
           tail, 1000.0 * tail / RATE, (double)DECAY, RATE);
    printf("  output frames     : %d (input %d + tail %d)\n", frames + tail, frames, tail);
    printf("  truncated tail    : %d frames carry non-zero samples (%d above -60 dBFS), "
           "peak %.4f\n", cut_frames, cut_loud, (double)cut_peak);
    printf("  truncated samples : %d of %zu samples in the %d-frame tail\n",
           cut_frames * ch, (size_t)tail * (size_t)ch, tail);
    printf("  curve %-18s: %d rows, %d mismatched vs hand (>1e-6), max abs err %.3g, "
           "max rel err %.3g\n",
           path, st.rows, st.mismatched, st.max_abs_err, st.max_rel_err);
    printf("  peak              : legacy %.6f, full %.6f (tail adds %.6f)\n",
           (double)peak_abs(old, in_n), (double)peak_abs(full, out_n),
           (double)peak_abs(full + in_n, out_n - in_n));
    printf("  time per block    : legacy %.1f us, three stages %.1f us (x%.2f)\n",
           us_old, us_full, us_full / us_old);
    printf("  equivalence       : staged == one-call %s, prefix == legacy %s\n",
           staged_identical ? "yes" : "NO", prefix_identical ? "yes" : "NO");

    if (frames <= 4096)
        printf("  demo.wav          : %s\n",
               write_wav("demo.wav", full, frames + tail, ch) == 0 ? "written" : "FAILED");
    else
        printf("  demo.wav          : skipped (large case)\n");

    free(in); free(old); free(full); free(staged); free(padded); free(ir); free(imp);
    rv_free(&r);
    return (staged_identical && prefix_identical && st.mismatched == 0) ? 0 : 1;
}

/* Smallest possible input: one sample.  The tail still has to be complete. */
static int run_one_frame(void) {
    Reverb r;
    if (make(&r) != RV_OK) return 1;
    rv_set_mix(&r, DRY, WET);
    int tail = 0;
    rv_tail_len(RATE, DECAY, &tail);
    float in[CHANNELS] = {1.0f, 0.5f};
    float *out = (float*)malloc((size_t)(1 + tail) * CHANNELS * sizeof(float));
    if (!out) return 1;
    int n = rv_process(&r, in, 1, out, &tail);
    printf("\n=== one frame (degenerate small) ===\n");
    printf("  output frames     : %d, peak %.6f\n", n, (double)peak_abs(out, (size_t)n * CHANNELS));
    int ok = n == 1 + tail;
    free(out);
    rv_free(&r);
    return ok ? 0 : 1;
}

int main(void) {
    printf("reverb: three-stage feedback-comb tail\n");
    printf("stage 1 rv_tail_len: tail = ceil(RT60 * rate) frames\n");
    printf("stage 2 rv_pad     : input followed by `tail` zero frames\n");
    printf("stage 3 rv_sum     : out = dry*in + wet*comb network, sample by sample\n");
    printf("legacy path        : rv_process_legacy, tail dropped (tail = 0)\n");

    int fails = 0;
    fails += run_case("small", 512);       /* ~10.7 ms: shorter than the tail */
    fails += run_case("large", 480000);    /* 10 s: much longer than the tail   */
    fails += run_one_frame();

    printf("\n=== empty input ===\n");
    Reverb r;
    if (make(&r) == RV_OK) {
        float out[8];
        int rc = rv_process(&r, out, 0, out, NULL);
        printf("  rv_process(frames=0) -> %d: %s\n", rc, rv_strerror(rc));
        if (rc != RV_ERR_EMPTY) ++fails;
        rc = rv_pad(out, 0, CHANNELS, 4, out);
        printf("  rv_pad(frames=0)     -> %d: %s\n", rc, rv_strerror(rc));
        if (rc != RV_ERR_EMPTY) ++fails;
        rv_free(&r);
    }

    printf("\n%s\n", fails == 0 ? "all cases match the hand table" : "MISMATCH");
    return fails ? 1 : 0;
}
