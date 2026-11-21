#include "mixer.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static float bus_peak(const MixBus *b) {
    float p = 0.0f;
    size_t n = (size_t)b->frames * (size_t)b->channels;
    for (size_t i = 0; i < n; ++i) {
        float a = fabsf(b->buf[i]);
        if (a > p) p = a;
    }
    return p;
}

static void err_add(char *err, int errcap, int *nconf, const char *fmt, ...) {
    ++*nconf;
    if (!err || errcap <= 0) return;
    size_t used = strlen(err);
    if ((int)used >= errcap - 1) return;
    if (used > 0) {
        strncat(err, "; ", (size_t)(errcap - 1 - (int)used));
        used = strlen(err);
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err + used, (size_t)(errcap - (int)used), fmt, ap);
    va_end(ap);
}

int mx_crossfeed_apply(MixBus *b, const MxCrossfeedCfg *cfg,
                       MxCrossfeedStats *stats, char *err, int errcap) {
    if (err && errcap > 0) err[0] = '\0';
    if (stats) memset(stats, 0, sizeof *stats);
    if (!b || !cfg) {
        int n = 0;
        err_add(err, errcap, &n, "conflict: %s is NULL", !b ? "bus" : "cfg");
        return -1;
    }
    if (!cfg->enabled) return 0; /* 旁路：缓冲区逐位不变 */

    /* 非法组合：收集全部冲突项，一次列出 */
    int nconf = 0;
    if (!b->buf || b->frames <= 0)
        err_add(err, errcap, &nconf, "conflict: bus is empty (frames=%d)", b->frames);
    if (b->channels <= 0 || b->channels % 2 != 0)
        err_add(err, errcap, &nconf, "conflict: channels=%d not even (need L/R pairs)", b->channels);
    if (!(cfg->feed > 0.0f))
        err_add(err, errcap, &nconf, "conflict: feed=%g <= 0 while enabled", (double)cfg->feed);
    if (cfg->feed > 1.0f)
        err_add(err, errcap, &nconf, "conflict: feed=%g > 1", (double)cfg->feed);
    if (cfg->delay_ms < 0.0f)
        err_add(err, errcap, &nconf, "conflict: delay_ms=%g < 0", (double)cfg->delay_ms);
    if (cfg->delay_ms > 30.0f)
        err_add(err, errcap, &nconf, "conflict: delay_ms=%g > 30", (double)cfg->delay_ms);
    if (cfg->sample_rate < 8000 || cfg->sample_rate > 192000)
        err_add(err, errcap, &nconf, "conflict: sample_rate=%d outside [8000,192000]", cfg->sample_rate);
    if (nconf) return -1;

    /* 第一段：按延迟算对侧信号（折算成采样点位移） */
    const int d = (int)lroundf(cfg->delay_ms * (float)cfg->sample_rate / 1000.0f);
    const int ch = b->channels, frames = b->frames, pairs = ch / 2;
    const float feed = cfg->feed;

    float peak_before = bus_peak(b);

    float *t = (float *)malloc((size_t)frames * (size_t)ch * sizeof(float));
    if (!t) {
        int n = 0;
        err_add(err, errcap, &n, "conflict: out of memory");
        return -1;
    }
    memcpy(t, b->buf, (size_t)frames * (size_t)ch * sizeof(float));

    /* 第二段：按衰减混入（每个 L/R 对：本侧 += feed * 对侧[i-d]） */
    for (int i = 0; i < frames; ++i) {
        int j = i - d;
        for (int p = 0; p < pairs; ++p) {
            float crossL = (j >= 0) ? t[(size_t)j * ch + 2 * p + 1] : 0.0f;
            float crossR = (j >= 0) ? t[(size_t)j * ch + 2 * p]     : 0.0f;
            b->buf[(size_t)i * ch + 2 * p]     = t[(size_t)i * ch + 2 * p]     + feed * crossL;
            b->buf[(size_t)i * ch + 2 * p + 1] = t[(size_t)i * ch + 2 * p + 1] + feed * crossR;
        }
    }
    free(t);

    /* 第三段：检查峰值 */
    if (stats) {
        stats->delay_samples = d;
        stats->peak_before = peak_before;
        stats->peak_after = bus_peak(b);
        stats->clipped = stats->peak_after > 1.0f;
    }
    return 0;
}
