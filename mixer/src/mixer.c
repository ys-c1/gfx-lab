#include "mixer.h"
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int mx_init(MixBus *b, int cap_frames, int channels) {
    if (!b) return MX_ERR_ARG;
    if (channels <= 0) return MX_ERR_CHANNELS;
    if (cap_frames <= 0) return MX_ERR_CAPACITY;
    if ((size_t)cap_frames > SIZE_MAX / (size_t)channels) return MX_ERR_CAPACITY;
    b->buf = (float*)calloc((size_t)cap_frames * (size_t)channels, sizeof(float));
    if (!b->buf) return MX_ERR_MEM;
    b->frames = 0; b->channels = channels; b->cap = cap_frames;
    return 0;
}

void mx_free(MixBus *b) { if (b) { free(b->buf); b->buf = NULL; b->frames = 0; b->cap = 0; } }

int mx_add(MixBus *b, const float *src, int frames, float gain) {
    if (!b || !b->buf || !src || frames <= 0) return MX_ERR_ARG;
    if (b->channels <= 0) return MX_ERR_CHANNELS;
    int n = frames;
    if (b->frames + n > b->cap) n = b->cap - b->frames;
    for (int i = 0; i < n; ++i)
        for (int c = 0; c < b->channels; ++c)
            b->buf[(size_t)(b->frames + i) * b->channels + c] += src[(size_t)i * b->channels + c] * gain;
    b->frames += n;
    return n;
}

const char *mx_error_field(int err) {
    switch (err) {
        case MX_ERR_ARG:      return "argument";
        case MX_ERR_CHANNELS: return "channels";
        case MX_ERR_DELAY:    return "delay";
        case MX_ERR_CAPACITY: return "capacity";
        case MX_ERR_IO:       return "path";
        case MX_ERR_MEM:      return "memory";
        default:              return "";
    }
}

/* Validate the per-channel delay array and return the largest value. */
static int delay_span(const MixBus *b, const int *delays, int *max_delay) {
    int m = 0;
    for (int c = 0; c < b->channels; ++c) {
        if (delays[c] < 0) return MX_ERR_DELAY;
        if (delays[c] > MX_MAX_DELAY) return MX_ERR_DELAY;
        if (delays[c] > m) m = delays[c];
    }
    *max_delay = m;
    return 0;
}

static int check_table(const MixBus *b, const MxAlignTable *table) {
    return (table && b->channels > MX_MAX_CHANNELS) ? MX_ERR_CHANNELS : 0;
}

int mx_align(const MixBus *b, const float *src, int frames, const int *delays,
             float *dst, MxAlignTable *table) {
    if (!b || !src || !delays || !dst || frames <= 0) return MX_ERR_ARG;
    if (b->channels <= 0) return MX_ERR_CHANNELS;
    int rc = check_table(b, table);
    if (rc != 0) return rc;
    if (table) memset(table, 0, sizeof(*table));

    int max_delay = 0;
    rc = delay_span(b, delays, &max_delay);
    if (rc != 0) return rc;

    if (frames > INT_MAX - max_delay) return MX_ERR_DELAY;
    int aligned = frames + max_delay;
    if ((size_t)aligned > SIZE_MAX / (size_t)b->channels) return MX_ERR_CAPACITY;

    size_t total = (size_t)aligned * (size_t)b->channels;
    for (size_t i = 0; i < total; ++i) dst[i] = 0.0f;

    int compensated = 0;
    for (int c = 0; c < b->channels; ++c) {
        int comp = max_delay - delays[c];
        compensated += comp;
        for (int i = 0; i < frames; ++i)
            dst[(size_t)(i + comp) * b->channels + c] = src[(size_t)i * b->channels + c];
        if (table) {
            table->delay[c] = delays[c];
            table->comp[c] = comp;
        }
    }

    if (table) {
        table->channels = b->channels;
        table->max_delay = max_delay;
        table->compensated = compensated;
    }
    return aligned;
}

int mx_add_aligned(MixBus *b, const float *aligned, int aligned_frames, float gain) {
    if (!b || !b->buf || !aligned || aligned_frames <= 0) return MX_ERR_ARG;
    if (b->channels <= 0) return MX_ERR_CHANNELS;
    if (aligned_frames > b->cap - b->frames) return MX_ERR_CAPACITY;
    for (int i = 0; i < aligned_frames; ++i)
        for (int c = 0; c < b->channels; ++c)
            b->buf[(size_t)(b->frames + i) * b->channels + c] += aligned[(size_t)i * b->channels + c] * gain;
    b->frames += aligned_frames;
    return aligned_frames;
}

int mx_add_delayed(MixBus *b, const float *src, int frames, const int *delays,
                   float gain, MxAlignTable *table) {
    if (!b || !src || !delays || frames <= 0) return MX_ERR_ARG;
    if (b->channels <= 0) return MX_ERR_CHANNELS;
    int rc = check_table(b, table);
    if (rc != 0) return rc;
    if (table) memset(table, 0, sizeof(*table));

    int max_delay = 0;
    rc = delay_span(b, delays, &max_delay);
    if (rc != 0) return rc;

    if (frames > INT_MAX - max_delay) return MX_ERR_DELAY;

    /* The delay has to fit the bus, otherwise the tail of the block would be
     * dropped and the channels would drift apart again. */
    int aligned = frames + max_delay;
    if (aligned > b->cap - b->frames) return MX_ERR_DELAY;

    if ((size_t)aligned > SIZE_MAX / (size_t)b->channels) return MX_ERR_CAPACITY;
    float *tmp = (float*)malloc((size_t)aligned * (size_t)b->channels * sizeof(float));
    if (!tmp) return MX_ERR_MEM;

    int n = mx_align(b, src, frames, delays, tmp, table);
    if (n < 0) { free(tmp); return n; }
    n = mx_add_aligned(b, tmp, n, gain);
    free(tmp);
    return n;
}

int mx_write_wav(const MixBus *b, const char *path) {
    if (!b || !path) return MX_ERR_ARG;
    FILE *f = fopen(path, "wb");
    if (!f) return MX_ERR_IO;
    unsigned int data_bytes = (unsigned int)(b->frames * b->channels * 2);
    unsigned int riff = 36 + data_bytes;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    unsigned int sz = 16; fwrite(&sz, 4, 1, f);
    unsigned short fmt = 1, ch = (unsigned short)b->channels;
    unsigned int rate = 48000, byte_rate = rate * ch * 2;
    unsigned short align = (unsigned short)(ch * 2), bits = 16;
    fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f);
    fwrite(&byte_rate, 4, 1, f); fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data_bytes, 4, 1, f);
    for (int i = 0; i < b->frames; ++i)
        for (int c = 0; c < b->channels; ++c) {
            float v = b->buf[(size_t)i * b->channels + c];
            if (v > 1.0f) v = 1.0f;
            if (v < -1.0f) v = -1.0f;
            short s = (short)lrintf(v * 32767.0f);
            fwrite(&s, 2, 1, f);
        }
    fclose(f);
    return 0;
}
