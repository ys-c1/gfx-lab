#include "room.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RATE 48000
#define NTAPS 6

static const int DELAY[NTAPS] = {191, 269, 373, 449, 541, 631};
static const float FB[NTAPS] = {0.60f, 0.50f, 0.45f, 0.40f, 0.35f, 0.30f};
static const float DECAY[NTAPS] = {0.90f, 0.80f, 0.70f, 0.60f, 0.50f, 0.40f};
static const float DRY = 1.0f, WET = 0.35f;

/* 手推脉冲响应（闭式）：out[n] = dry*[n==0] + wet * sum_t decay_t * fb_t^(n/d_t - 1) */
static double hand_computed(int n) {
    double v = (n == 0) ? DRY : 0.0;
    for (int t = 0; t < NTAPS; ++t) {
        if (n > 0 && n % DELAY[t] == 0) {
            int k = n / DELAY[t] - 1;
            double c = DECAY[t];
            for (int i = 0; i < k; ++i) c *= FB[t];
            v += WET * c;
        }
    }
    return v;
}

static int write_wav(const char *path, const float *buf, int frames, int channels) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    unsigned int data_bytes = (unsigned int)(frames * channels * 2);
    unsigned int riff = 36 + data_bytes;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    unsigned int sz = 16; fwrite(&sz, 4, 1, f);
    unsigned short fmt = 1, ch = (unsigned short)channels;
    unsigned int rate = RATE, byte_rate = rate * ch * 2;
    unsigned short align = (unsigned short)(ch * 2), bits = 16;
    fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f);
    fwrite(&byte_rate, 4, 1, f); fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data_bytes, 4, 1, f);
    for (int i = 0; i < frames * channels; ++i) {
        float v = buf[i];
        if (v > 1.0f) v = 1.0f;
        if (v < -1.0f) v = -1.0f;
        short s = (short)lrintf(v * 32767.0f);
        fwrite(&s, 2, 1, f);
    }
    fclose(f);
    return 0;
}

static int run_case(int channels, int frames, double *peak_err, int *arrival, double *elapsed_ms) {
    Room r;
    if (room_init(&r, channels, NTAPS, DELAY, FB, DECAY, NULL) != ROOM_OK) return 1;
    room_set_mix(&r, DRY, WET);

    size_t n = (size_t)frames * (size_t)channels;
    float *in = (float*)calloc(n, sizeof(float));
    float *out = (float*)malloc(n * sizeof(float));
    if (!in || !out) { free(in); free(out); room_free(&r); return 1; }
    for (int c = 0; c < channels; ++c) in[c] = 1.0f; /* 全声道单位脉冲 */

    clock_t t0 = clock();
    int rc = room_process(&r, in, out, frames, channels);
    clock_t t1 = clock();
    if (rc != ROOM_OK) { free(in); free(out); room_free(&r); return 1; }
    *elapsed_ms = 1000.0 * (double)(t1 - t0) / (double)CLOCKS_PER_SEC;

    *arrival = -1;
    *peak_err = 0.0;
    for (int i = 0; i < frames; ++i) {
        double expect = hand_computed(i);
        for (int c = 0; c < channels; ++c) {
            double e = fabs((double)out[(size_t)i * channels + c] - expect);
            if (e > *peak_err) *peak_err = e;
        }
        if (*arrival < 0 && i > 0 && out[(size_t)i * channels] != 0.0f) *arrival = i;
    }
    free(in); free(out);
    room_free(&r);
    return 0;
}

int main(void) {
    int frames = RATE / 2; /* 0.5 s，覆盖全部延迟线 */
    printf("taps=%d dry=%.2f wet=%.2f frames=%d @%dHz\n", NTAPS, DRY, WET, frames, RATE);
    printf("%-10s %-12s %-14s %-12s\n", "channels", "arrival(smp)", "peak_err", "elapsed(ms)");
    for (int ci = 0; ci < 2; ++ci) {
        int channels = ci == 0 ? 2 : 6;
        double peak_err = 0.0, elapsed = 0.0;
        int arrival = -1;
        if (run_case(channels, frames, &peak_err, &arrival, &elapsed) != 0) return 1;
        printf("%-10d %-12d %-14.3g %-12.3f\n", channels, arrival, peak_err, elapsed);
    }

    /* 演示产物：2 声道短促敲击过房间 */
    Room r;
    if (room_init(&r, 2, NTAPS, DELAY, FB, DECAY, NULL) != ROOM_OK) return 1;
    room_set_mix(&r, DRY, WET);
    int dframes = RATE;
    float *in = (float*)calloc((size_t)dframes * 2, sizeof(float));
    float *out = (float*)malloc((size_t)dframes * 2 * sizeof(float));
    if (!in || !out) return 1;
    for (int i = 0; i < 480; ++i) { /* 10 ms 衰减敲击 */
        float s = 0.8f * sinf(2.0f * 3.14159265f * 1800.0f * (float)i / RATE)
                * expf(-(float)i / 120.0f);
        in[i * 2 + 0] = s;
        in[i * 2 + 1] = s * 0.9f;
    }
    if (room_process(&r, in, out, dframes, 2) != ROOM_OK) return 1;
    int rc = write_wav("demo.wav", out, dframes, 2);
    free(in); free(out);
    room_free(&r);
    printf("wrote demo.wav (%d frames, 2ch)\n", dframes);
    return rc == 0 ? 0 : 1;
}
