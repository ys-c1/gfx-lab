#define _POSIX_C_SOURCE 200809L
#include "mixer.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define RATE     48000
#define FRAMES   512
#define HIT_LEN  96
#define CAP      (FRAMES + MX_MAX_DELAY)
#define TRIALS   20000

/* A short decaying 1 kHz burst stands in for one drum hit.  Its waveform is
 * identical on every channel, so its peak index moves by exactly the number
 * of samples a channel is shifted -- that makes the alignment measurable. */
static void add_hit(float *buf, int frames, int channels, int ch, int at, float amp) {
    for (int i = 0; i < HIT_LEN; ++i) {
        int f = at + i;
        if (f < 0 || f >= frames) continue;
        float env = expf(-6.0f * (float)i / (float)HIT_LEN);
        buf[(size_t)f * channels + ch] += amp * env * sinf(2.0f * 3.14159265f * 1000.0f * (float)i / RATE);
    }
}

/* One block: channel c carries its hit "delays[c]" samples late. */
static void build_input(float *buf, int frames, int channels, const int *delays, int true_frame) {
    memset(buf, 0, (size_t)frames * channels * sizeof(float));
    for (int c = 0; c < channels; ++c)
        add_hit(buf, frames, channels, c, true_frame + delays[c], 0.5f);
}

/* Index of the largest-magnitude sample in one channel. */
static int peak_pos(const float *buf, int frames, int channels, int ch) {
    int best = -1;
    float bestv = 0.0f;
    for (int i = 0; i < frames; ++i) {
        float v = fabsf(buf[(size_t)i * channels + ch]);
        if (v > bestv) { bestv = v; best = i; }
    }
    return best;
}

static float mono_peak(const float *buf, int frames, int channels) {
    float peak = 0.0f;
    for (int i = 0; i < frames; ++i) {
        float m = 0.0f;
        for (int c = 0; c < channels; ++c) m += buf[(size_t)i * channels + c];
        if (fabsf(m) > peak) peak = fabsf(m);
    }
    return peak;
}

static double us_between(const struct timespec *a, const struct timespec *b) {
    return (double)(b->tv_sec - a->tv_sec) * 1e6 + (double)(b->tv_nsec - a->tv_nsec) / 1e3;
}

static void report(int channels, const int *delays, int true_frame) {
    int max_delay = 0;
    for (int c = 0; c < channels; ++c) if (delays[c] > max_delay) max_delay = delays[c];

    static float src[FRAMES * 8], aligned[CAP * 8];
    build_input(src, FRAMES, channels, delays, true_frame);

    printf("\n=== %d channels, block %d frames, hit written at frame %d ===\n",
           channels, FRAMES, true_frame);
    printf("delay cap MX_MAX_DELAY = %d samples (%.2f ms at %d Hz)\n",
           MX_MAX_DELAY, 1000.0 * MX_MAX_DELAY / RATE, RATE);

    /* --- unaligned reference: the old output path, mx_add straight into a bus --- */
    MixBus raw;
    if (mx_init(&raw, CAP, channels) != 0) { printf("init failed\n"); return; }
    int nraw = mx_add(&raw, src, FRAMES, 1.0f);
    int lo = 1 << 30, hi = -1;
    printf("\nunaligned (mx_add):\n  peak index:");
    for (int c = 0; c < channels; ++c) {
        int o = peak_pos(raw.buf, nraw, channels, c);
        printf(" ch%d=%d", c, o);
        if (o < lo) lo = o;
        if (o > hi) hi = o;
    }
    printf("\n  peak spread = %d samples, mono peak = %.3f\n",
           hi - lo, mono_peak(raw.buf, nraw, channels));

    /* --- stage 1: pad each channel; stage 2: sum the aligned block --- */
    MixBus bus;
    if (mx_init(&bus, CAP, channels) != 0) { printf("init failed\n"); mx_free(&raw); return; }
    MxAlignTable table;
    memset(&table, 0, sizeof(table));

    int na = mx_align(&bus, src, FRAMES, delays, aligned, &table);
    if (na < 0) { printf("align failed: %d (%s)\n", na, mx_error_field(na)); return; }
    int n = mx_add_aligned(&bus, aligned, na, 1.0f);
    if (n < 0) { printf("add failed: %d (%s)\n", n, mx_error_field(n)); return; }

    printf("\naligned (stage 1 mx_align -> stage 2 mx_add_aligned):\n");
    printf("  delay[]      :");
    for (int c = 0; c < channels; ++c) printf(" %d", table.delay[c]);
    printf("\n  comp[]       :");
    for (int c = 0; c < channels; ++c) printf(" %d", table.comp[c]);
    printf("   (= max_delay - delay[c])\n");
    printf("  max_delay    : %d samples\n", table.max_delay);
    printf("  compensated  : %d samples inserted in total\n", table.compensated);
    printf("  output frames: %d (input %d + max_delay %d)\n", n, FRAMES, table.max_delay);

    /* Source peak + comp[c] is the hand calculation; compare with what the
     * aligned output actually shows. */
    printf("\n  ch | src peak | comp | hand = src+comp | measured | err\n");
    int worst = 0, lo2 = 1 << 30, hi2 = -1;
    for (int c = 0; c < channels; ++c) {
        int sp = peak_pos(src, FRAMES, channels, c);
        int hand = sp + table.comp[c];
        int got = peak_pos(bus.buf, n, channels, c);
        int err = got - hand;
        if (err < 0) err = -err;
        if (err > worst) worst = err;
        if (got < lo2) lo2 = got;
        if (got > hi2) hi2 = got;
        printf("  %2d | %8d | %4d | %15d | %8d | %d\n", c, sp, table.comp[c], hand, got, err);
    }
    printf("  alignment err: max %d sample(s), peak spread = %d, mono peak = %.3f\n",
           worst, hi2 - lo2, mono_peak(bus.buf, n, channels));
    printf("  note: on every channel the hit now peaks at frame %d = %d + max_delay %d\n",
           hi2, peak_pos(src, FRAMES, channels, 0) - delays[0], table.max_delay);

    /* --- timing: both stages vs. the unaligned path --- */
    struct timespec t0, t1;
    int sink = 0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int k = 0; k < TRIALS; ++k) {
        bus.frames = 0;
        int m = mx_align(&bus, src, FRAMES, delays, aligned, &table);
        sink += mx_add_aligned(&bus, aligned, m, 1.0f);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double us_aligned = us_between(&t0, &t1) / TRIALS;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int k = 0; k < TRIALS; ++k) {
        raw.frames = 0;
        sink += mx_add(&raw, src, FRAMES, 1.0f);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double us_raw = us_between(&t0, &t1) / TRIALS;
    printf("  time per block: unaligned %.3f us, aligned %.3f us (x%.2f, sink %d)\n",
           us_raw, us_aligned, us_aligned / us_raw, sink);

    if (channels == 2)
        printf("  demo.wav: %s\n", mx_write_wav(&bus, "demo.wav") == 0 ? "written" : "FAILED");
    mx_free(&raw);
    mx_free(&bus);
}

int main(void) {
    printf("mixer: per-channel delay alignment\n");
    printf("input is one drum hit on every channel, each arriving delays[c]\n");
    printf("samples late; alignment pads channel c by comp[c] = max_delay -\ndelays[c] so all hits land on the same frame.\n");

    int d2[2] = {0, 3};
    report(2, d2, 100);

    int d6[6] = {3, 0, 5, 1, 4, 2};
    report(6, d6, 100);

    /* --- zero delay keeps the old output --- */
    printf("\n=== zero delay ===\n");
    MixBus a, b;
    static float src[FRAMES * 2];
    int zero[2] = {0, 0};
    build_input(src, FRAMES, 2, zero, 100);
    mx_init(&a, CAP, 2);
    mx_init(&b, CAP, 2);
    int na = mx_add(&a, src, FRAMES, 1.0f);
    MxAlignTable t;
    memset(&t, 0, sizeof(t));
    int nb = mx_add_delayed(&b, src, FRAMES, zero, 1.0f, &t);
    printf("frames: mx_add=%d, mx_add_delayed=%d; output identical: %s\n",
           na, nb, (na == nb && memcmp(a.buf, b.buf, sizeof(float) * (size_t)na * 2) == 0) ? "yes" : "no");
    printf("compensated samples: %d, max_delay: %d\n", t.compensated, t.max_delay);
    mx_free(&a);
    mx_free(&b);

    /* --- rejected inputs name the field --- */
    printf("\n=== errors ===\n");
    MixBus e;
    int rc = mx_init(&e, 8, 0);
    printf("mx_init(channels=0)        -> %d (%s)\n", rc, mx_error_field(rc));
    mx_init(&e, 8, 2);
    static float s2[FRAMES * 2];
    int negd[2] = {0, -1}, bigd[2] = {0, MX_MAX_DELAY + 1}, over[2] = {0, 1};
    MxAlignTable te;
    memset(&te, 0, sizeof(te));
    rc = mx_align(&e, s2, FRAMES, negd, src, &te);
    printf("delay < 0                  -> %d (%s)\n", rc, mx_error_field(rc));
    rc = mx_align(&e, s2, FRAMES, bigd, src, &te);
    printf("delay > MX_MAX_DELAY (%d)   -> %d (%s)\n", MX_MAX_DELAY, rc, mx_error_field(rc));
    rc = mx_add_delayed(&e, s2, FRAMES, over, 1.0f, &te);
    printf("delay overflows bus buffer -> %d (%s)\n", rc, mx_error_field(rc));
    mx_free(&e);
    return 0;
}
