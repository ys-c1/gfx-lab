#include "mixer.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* internal ceiling stays below the requested threshold by more than one
 * int16 LSB (1/32767 ~= 3.05e-5), so adjacent peak samples cannot quantize
 * to the same 32767 value: no overflow and no plateau in the rendered WAV */
#define MX_CEIL_MARGIN 2.0e-4f
#define MX_PLATEAU_EPS 1e-7f

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

int mx_add_ex(MixBus *b, const float *src, int frames, int channels, float gain) {
    if (!b || !src) return MX_ERR_NULL;
    if (frames <= 0 || channels <= 0) return MX_ERR_PARAM;
    if (b->channels != channels) return MX_ERR_CHANNELS;
    int n = frames;
    if (b->frames + n > b->cap) n = b->cap - b->frames;
    for (int i = 0; i < n; ++i)
        for (int c = 0; c < channels; ++c)
            b->buf[(size_t)(b->frames + i) * channels + c] += src[(size_t)i * channels + c] * gain;
    b->frames += n;
    return n;
}

const char *mx_err_str(int code) {
    switch (code) {
        case MX_OK:             return "ok";
        case MX_ERR_NULL:       return "null pointer (b/src/cfg)";
        case MX_ERR_PARAM:      return "parameter out of range (threshold/sample_rate/attack_ms/release_ms/frames/channels)";
        case MX_ERR_CHANNELS:   return "channels mismatch (src channels != bus channels)";
        default:                return "unknown error";
    }
}

static int cfg_validate(const MxLimitCfg *cfg) {
    if (!cfg) return MX_ERR_NULL;
    if (!(cfg->threshold > 0.0f) || !(cfg->threshold <= 1.0f)) return MX_ERR_PARAM;
    if (cfg->sample_rate <= 0) return MX_ERR_PARAM;
    if (!(cfg->attack_ms > 0.0f) || !(isnormal(cfg->attack_ms))) return MX_ERR_PARAM;
    if (!(cfg->release_ms > 0.0f) || !(isnormal(cfg->release_ms))) return MX_ERR_PARAM;
    return MX_OK;
}

int mx_limit(MixBus *b, const MxLimitCfg *cfg, MxLimitReport *rep) {
    if (!b) return MX_ERR_NULL;
    int vr = cfg_validate(cfg);
    if (vr != MX_OK) return vr;
    if (b->channels <= 0 || b->frames < 0) return MX_ERR_PARAM;

    if (!cfg->enabled) return MX_OK;   /* bypass: buffer untouched, report untouched */

    const int N = b->frames, C = b->channels;
    const float ceil_v = cfg->threshold * (1.0f - MX_CEIL_MARGIN);
    const int La = (int)lroundf(cfg->attack_ms * 0.001f * (float)cfg->sample_rate);
    const int look = La > 0 ? La : 1;
    const float rel_coef = expf(-1.0f /
        (cfg->release_ms * 0.001f * (float)cfg->sample_rate));

    float *g = (float*)malloc((size_t)N * sizeof(float));
    if (!g) return MX_ERR_NULL;

    float peak_before = 0.0f, peak_after = 0.0f;
    int plateau_runs = 0, plateau_samples = 0, limited = 0;

    for (int c = 0; c < C; ++c) {
        /* forward: required instantaneous gain from local peak inside window */
        for (int i = 0; i < N; ++i) {
            float pk = 0.0f;
            int jmax = i + look; if (jmax > N) jmax = N;
            for (int j = i; j < jmax; ++j) {
                float v = fabsf(b->buf[(size_t)j * C + c]);
                if (v > pk) pk = v;
            }
            g[i] = pk > ceil_v ? ceil_v / pk : 1.0f;
        }
        /* backward: smooth so gain only moves at the release rate */
        float env = 1.0f;
        for (int i = N - 1; i >= 0; --i)
            env = g[i] < env ? g[i] : (rel_coef * env + (1.0f - rel_coef) * g[i]),
            g[i] = env;

        int run = 0;
        for (int i = 0; i < N; ++i) {
            float *p = &b->buf[(size_t)i * C + c];
            float x = *p;
            float ax = fabsf(x);
            if (ax > peak_before) peak_before = ax;
            if (!isfinite(x)) x = 0.0f;   /* defensive: NaN/Inf cannot be limited */
            float y = x * g[i];
            if (g[i] < 1.0f) ++limited;
            *p = y;
            float ay = fabsf(y);
            if (ay > peak_after) peak_after = ay;
            /* plateau accounting: consecutive samples sitting on the ceiling */
            if (ay >= cfg->threshold - MX_PLATEAU_EPS) {
                ++run;
            } else {
                if (run >= 2) { ++plateau_runs; plateau_samples += run; }
                run = 0;
            }
        }
        if (run >= 2) { ++plateau_runs; plateau_samples += run; }
    }
    free(g);

    if (rep) {
        rep->peak_before = peak_before;
        rep->peak_after = peak_after;
        rep->plateau_runs = plateau_runs;
        rep->plateau_samples = plateau_samples;
        rep->limited_samples = limited;
    }
    return MX_OK;
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
