#include "room.h"
#include <stdlib.h>
#include <string.h>

const char *room_strerror(int code) {
    switch (code) {
    case ROOM_OK:           return "no error";
    case ROOM_ERR_ARG:      return "invalid argument: null pointer or non-positive size";
    case ROOM_ERR_CHANNELS: return "invalid parameter 'channels': must be > 0 and match room_init";
    case ROOM_ERR_DELAY:    return "invalid parameter 'delay': every tap delay must be > 0 samples";
    case ROOM_ERR_FEEDBACK: return "invalid parameter 'feedback': every tap must satisfy |feedback| <= 1";
    case ROOM_ERR_NOMEM:    return "out of memory";
    default:                return "unknown error";
    }
}

int room_init(Room *r, int channels, int taps,
              const int *delay, const float *feedback, const float *decay,
              int *bad_tap) {
    if (bad_tap) *bad_tap = -1;
    if (!r || !delay || !feedback || taps <= 0) return ROOM_ERR_ARG;
    if (channels <= 0) return ROOM_ERR_CHANNELS;
    for (int t = 0; t < taps; ++t) {
        if (delay[t] <= 0) { if (bad_tap) *bad_tap = t; return ROOM_ERR_DELAY; }
        if (!(feedback[t] >= -1.0f && feedback[t] <= 1.0f)) {
            if (bad_tap) *bad_tap = t;
            return ROOM_ERR_FEEDBACK;
        }
    }
    memset(r, 0, sizeof(*r));
    r->channels = channels;
    r->taps = taps;
    r->dry = 1.0f;
    r->wet = 0.0f;
    r->delay = (int*)malloc((size_t)taps * sizeof(int));
    r->fb = (float*)malloc((size_t)taps * sizeof(float));
    r->decay = (float*)malloc((size_t)taps * sizeof(float));
    r->pos = (int*)calloc((size_t)taps, sizeof(int));
    r->line = (float**)calloc((size_t)taps, sizeof(float*));
    if (!r->delay || !r->fb || !r->decay || !r->pos || !r->line) { room_free(r); return ROOM_ERR_NOMEM; }
    for (int t = 0; t < taps; ++t) {
        r->delay[t] = delay[t];
        r->fb[t] = feedback[t];
        r->decay[t] = decay ? decay[t] : 1.0f;
        r->line[t] = (float*)calloc((size_t)delay[t] * (size_t)channels, sizeof(float));
        if (!r->line[t]) { room_free(r); return ROOM_ERR_NOMEM; }
    }
    return ROOM_OK;
}

void room_free(Room *r) {
    if (!r) return;
    if (r->line)
        for (int t = 0; t < r->taps; ++t) free(r->line[t]);
    free(r->line); free(r->delay); free(r->fb); free(r->decay); free(r->pos);
    memset(r, 0, sizeof(*r));
}

void room_set_mix(Room *r, float dry, float wet) {
    if (!r) return;
    r->dry = dry;
    r->wet = wet;
}

int room_reflect(Room *r, const float *in, float *tap_out, int frames, int channels) {
    if (!r || !in || !tap_out || frames <= 0) return ROOM_ERR_ARG;
    if (channels <= 0 || channels != r->channels) return ROOM_ERR_CHANNELS;
    for (int t = 0; t < r->taps; ++t) {
        float *ln = r->line[t];
        int d = r->delay[t], p = r->pos[t];
        float fb = r->fb[t];
        float *to = tap_out + (size_t)t * (size_t)frames * (size_t)channels;
        for (int i = 0; i < frames; ++i) {
            for (int c = 0; c < channels; ++c) {
                float rd = ln[(size_t)p * channels + c];
                to[(size_t)i * channels + c] = rd;
                ln[(size_t)p * channels + c] = in[(size_t)i * channels + c] + fb * rd;
            }
            if (++p == d) p = 0;
        }
        r->pos[t] = p;
    }
    return ROOM_OK;
}

int room_sum(const Room *r, const float *tap_in, float *wet, int frames, int channels) {
    if (!r || !tap_in || !wet || frames <= 0) return ROOM_ERR_ARG;
    if (channels <= 0 || channels != r->channels) return ROOM_ERR_CHANNELS;
    memset(wet, 0, (size_t)frames * (size_t)channels * sizeof(float));
    for (int t = 0; t < r->taps; ++t) {
        const float *ti = tap_in + (size_t)t * (size_t)frames * (size_t)channels;
        float g = r->decay[t];
        for (int i = 0; i < frames; ++i)
            for (int c = 0; c < channels; ++c)
                wet[(size_t)i * channels + c] += g * ti[(size_t)i * channels + c];
    }
    return ROOM_OK;
}

int room_mix(const Room *r, const float *in, const float *wet_in, float *out,
             int frames, int channels) {
    if (!r || !in || !out || frames <= 0) return ROOM_ERR_ARG;
    if (channels <= 0 || channels != r->channels) return ROOM_ERR_CHANNELS;
    size_t n = (size_t)frames * (size_t)channels;
    if (r->dry == 1.0f && r->wet == 0.0f) {   /* 不加混响：逐样本一致 */
        memcpy(out, in, n * sizeof(float));
        return ROOM_OK;
    }
    if (!wet_in) return ROOM_ERR_ARG;
    for (size_t i = 0; i < n; ++i)
        out[i] = r->dry * in[i] + r->wet * wet_in[i];
    return ROOM_OK;
}

int room_process(Room *r, const float *in, float *out, int frames, int channels) {
    if (!r || !in || !out || frames <= 0) return ROOM_ERR_ARG;
    if (channels <= 0 || channels != r->channels) return ROOM_ERR_CHANNELS;
    size_t tap_n = (size_t)r->taps * (size_t)frames * (size_t)channels;
    size_t bus_n = (size_t)frames * (size_t)channels;
    float *tap = (float*)malloc(tap_n * sizeof(float));
    float *wet = (float*)malloc(bus_n * sizeof(float));
    if (!tap || !wet) { free(tap); free(wet); return ROOM_ERR_NOMEM; }
    int rc = room_reflect(r, in, tap, frames, channels);
    if (rc == ROOM_OK) rc = room_sum(r, tap, wet, frames, channels);
    if (rc == ROOM_OK) rc = room_mix(r, in, wet, out, frames, channels);
    free(tap); free(wet);
    return rc;
}
