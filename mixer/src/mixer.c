#include "mixer.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

int mx_init(MixBus *b, int cap_frames, int channels) {
    if (!b || cap_frames <= 0 || channels <= 0) return -1;
    b->buf = (float*)calloc((size_t)cap_frames * (size_t)channels, sizeof(float));
    if (!b->buf) return -1;
    b->frames = 0; b->channels = channels; b->cap = cap_frames;
    return 0;
}

void mx_free(MixBus *b) { if (b) { free(b->buf); b->buf = NULL; b->frames = 0; b->cap = 0; } }

int mx_add(MixBus *b, const float *src, int frames, float gain) {
    if (!b || !src || frames <= 0) return -1;
    int n = frames;
    if (b->frames + n > b->cap) n = b->cap - b->frames;
    for (int i = 0; i < n; ++i)
        for (int c = 0; c < b->channels; ++c)
            b->buf[(size_t)(b->frames + i) * b->channels + c] += src[(size_t)i * b->channels + c] * gain;
    b->frames += n;
    return n;
}

int mx_write_wav(const MixBus *b, const char *path) {
    if (!b || !path) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) return -2;
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
