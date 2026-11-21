#include "reverb.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Buffers may not overlap: the stages read the input while writing output.
 * Sizes are in floats and may differ (padded output is longer than input). */
static int overlaps(const float *a, size_t na, const float *b, size_t nb) {
    uintptr_t pa = (uintptr_t)a, pb = (uintptr_t)b;
    uintptr_t ba = (uintptr_t)(na * sizeof(float)), bb = (uintptr_t)(nb * sizeof(float));
    return pa < pb + bb && pb < pa + ba;
}

static int size_ok(int frames, int channels) {
    if (frames < 0 || channels <= 0) return 0;
    if ((size_t)frames > SIZE_MAX / (size_t)channels) return 0;
    if ((size_t)frames * (size_t)channels > SIZE_MAX / sizeof(float)) return 0;
    return 1;
}

const char *rv_strerror(int code) {
    switch (code) {
    case RV_OK:            return "no error";
    case RV_ERR_ARG:       return "invalid argument 'buffer': null pointer";
    case RV_ERR_EMPTY:     return "invalid parameter 'frames': input is empty (frames <= 0)";
    case RV_ERR_RATE:      return "invalid parameter 'rate': must be > 0 samples/s";
    case RV_ERR_DECAY:     return "invalid parameter 'decay_sec': must be finite and > 0 s";
    case RV_ERR_CHANNELS:  return "invalid parameter 'channels': must be 1..8";
    case RV_ERR_DELAY:     return "invalid parameter 'delay': every tap must be > 0 samples";
    case RV_ERR_CAPACITY:  return "invalid parameter 'frames': padded length does not fit in int";
    case RV_ERR_IO:        return "invalid parameter 'path': decay curve could not be written";
    case RV_ERR_NOMEM:     return "out of memory";
    case RV_ERR_TAPS:      return "invalid parameter 'taps': must be 1..8";
    case RV_ERR_GAIN:      return "invalid parameter 'gain': every tap must be finite and >= 0";
    case RV_ERR_TAIL:      return "invalid parameter 'tail': must be >= 0 frames";
    case RV_ERR_ALIAS:     return "invalid argument 'buffer': input and output overlap";
    default:               return "unknown error";
    }
}

/* fb = 10^(-3 * delay / (decay_sec * rate)): one pass through the delay line
 * loses exactly the 60 dB spread over decay_sec. */
float rv_tap_feedback(int delay, int rate, float decay_sec) {
    if (delay <= 0 || rate <= 0 || !isfinite(decay_sec) || decay_sec <= 0.0f) return 0.0f;
    return (float)pow(10.0, -3.0 * (double)delay / ((double)decay_sec * (double)rate));
}

int rv_init(Reverb *r, int channels, int rate, int taps,
            const int *delay, const float *gain, float decay_sec) {
    if (!r) return RV_ERR_ARG;
    if (channels <= 0 || channels > RV_MAX_CHANNELS) return RV_ERR_CHANNELS;
    if (rate <= 0) return RV_ERR_RATE;
    if (!isfinite(decay_sec) || decay_sec <= 0.0f) return RV_ERR_DECAY;
    if (!delay || !gain) return RV_ERR_ARG;
    if (taps <= 0 || taps > RV_MAX_TAPS) return RV_ERR_TAPS;

    for (int t = 0; t < taps; ++t) {
        if (delay[t] <= 0) return RV_ERR_DELAY;
        if (!isfinite(gain[t]) || gain[t] < 0.0f) return RV_ERR_GAIN;
    }

    memset(r, 0, sizeof(*r));
    r->channels = channels;
    r->rate = rate;
    r->taps = taps;
    r->decay_sec = decay_sec;
    r->dry = 1.0f;
    r->wet = 0.0f;
    for (int t = 0; t < taps; ++t) {
        r->delay[t] = delay[t];
        r->gain[t] = gain[t];
        r->fb[t] = rv_tap_feedback(delay[t], rate, decay_sec);
        if (delay[t] > INT_MAX / channels) { rv_free(r); return RV_ERR_CAPACITY; }
        size_t n = (size_t)delay[t] * (size_t)channels;
        r->line[t] = (float*)calloc(n, sizeof(float));
        if (!r->line[t]) { rv_free(r); return RV_ERR_NOMEM; }
    }
    return RV_OK;
}

void rv_free(Reverb *r) {
    if (!r) return;
    for (int t = 0; t < r->taps; ++t) free(r->line[t]);
    memset(r, 0, sizeof(*r));
}

void rv_reset(Reverb *r) {
    if (!r) return;
    for (int t = 0; t < r->taps; ++t) {
        memset(r->line[t], 0, (size_t)r->delay[t] * (size_t)r->channels * sizeof(float));
        r->pos[t] = 0;
    }
}

void rv_set_mix(Reverb *r, float dry, float wet) {
    if (!r) return;
    r->dry = dry;
    r->wet = wet;
}

int rv_tail_len(int rate, float decay_sec, int *tail_frames) {
    if (rate <= 0) return RV_ERR_RATE;
    if (!isfinite(decay_sec) || decay_sec <= 0.0f) return RV_ERR_DECAY;
    if (!tail_frames) return RV_ERR_ARG;
    /* Multiply in double, then snap a product that is within a thousandth of
     * a frame of an integer.  This keeps short decimal decays exact despite
     * their float representation: 0.1f * 48000 is 4800.00007, which should
     * still yield a 4800-frame tail, not 4801. */
    double d = (double)decay_sec * (double)rate;
    const double eps = 1e-3;
    double fl = floor(d);
    if (d - fl < eps)
        d = fl;
    else
        d = ceil(d);
    if (d > (double)INT_MAX) return RV_ERR_CAPACITY;
    int n = (int)d;
    if (n < 1) n = 1;
    *tail_frames = n;
    return RV_OK;
}

int rv_pad(const float *in, int frames, int channels, int tail, float *padded) {
    if (!in || !padded) return RV_ERR_ARG;
    if (frames <= 0) return RV_ERR_EMPTY;
    if (channels <= 0 || channels > RV_MAX_CHANNELS) return RV_ERR_CHANNELS;
    if (tail < 0) return RV_ERR_TAIL;
    if (frames > INT_MAX - tail) return RV_ERR_CAPACITY;
    int total = frames + tail;
    if (!size_ok(total, channels)) return RV_ERR_CAPACITY;
    size_t n = (size_t)frames * (size_t)channels;
    if (overlaps(in, (size_t)frames * (size_t)channels,
                 padded, (size_t)total * (size_t)channels)) return RV_ERR_ALIAS;

    memcpy(padded, in, n * sizeof(float));
    if (tail > 0)
        memset(padded + n, 0, (size_t)tail * (size_t)channels * sizeof(float));
    return total;
}

int rv_sum(Reverb *r, const float *padded, int padded_frames, float *out) {
    if (!r || !padded || !out) return RV_ERR_ARG;
    if (padded_frames <= 0) return RV_ERR_EMPTY;
    int ch = r->channels;
    if (ch <= 0 || ch > RV_MAX_CHANNELS) return RV_ERR_CHANNELS;
    if (r->taps <= 0 || r->taps > RV_MAX_TAPS) return RV_ERR_TAPS;
    if (!size_ok(padded_frames, ch)) return RV_ERR_CAPACITY;
    size_t n = (size_t)padded_frames * (size_t)ch;
    if (overlaps(padded, n, out, n)) return RV_ERR_ALIAS;

    /* Dry path first, then every tap adds its wet contribution sample by
     * sample.  Each tap's ring position advances once per frame, after all
     * channels of that frame were read and written. */
    if (r->dry == 1.0f) memcpy(out, padded, n * sizeof(float));
    else for (size_t k = 0; k < n; ++k) out[k] = r->dry * padded[k];

    for (int t = 0; t < r->taps; ++t) {
        int d = r->delay[t], p = r->pos[t];
        float *ln = r->line[t];
        float fb = r->fb[t], g = r->gain[t];
        /* With wet == 0 the output must stay bit-identical to dry * padded, but
         * the lines still advance: a later call may raise the wet mix. */
        float wg = r->wet * g;
        for (int i = 0; i < padded_frames; ++i) {
            const float *src = padded + (size_t)i * (size_t)ch;
            float *dst = out + (size_t)i * (size_t)ch;
            float *slot = ln + (size_t)p * (size_t)ch;
            for (int c = 0; c < ch; ++c) {
                float rd = slot[c];
                if (r->wet != 0.0f) dst[c] += wg * rd;
                slot[c] = src[c] + fb * rd;
            }
            if (p + 1 == d) p = 0; else ++p;
        }
        r->pos[t] = p;
    }
    return RV_OK;
}

int rv_flush(Reverb *r, int tail_frames, float *out) {
    if (!r || !out) return RV_ERR_ARG;
    if (tail_frames <= 0) return tail_frames == 0 ? RV_OK : RV_ERR_TAIL;
    int ch = r->channels;
    if (ch <= 0 || ch > RV_MAX_CHANNELS) return RV_ERR_CHANNELS;
    if (!size_ok(tail_frames, ch)) return RV_ERR_CAPACITY;
    size_t n = (size_t)tail_frames * (size_t)ch;
    float *zeros = (float*)calloc(n, sizeof(float));
    if (!zeros) return RV_ERR_NOMEM;
    int rc = rv_sum(r, zeros, tail_frames, out);
    free(zeros);
    return rc;
}

int rv_process_tail(Reverb *r, const float *in, int frames, int tail, float *out) {
    if (!r || !in || !out) return RV_ERR_ARG;
    if (frames <= 0) return RV_ERR_EMPTY;
    int ch = r->channels;
    if (ch <= 0 || ch > RV_MAX_CHANNELS) return RV_ERR_CHANNELS;
    if (tail < 0) return RV_ERR_TAIL;
    if (frames > INT_MAX - tail) return RV_ERR_CAPACITY;
    int total = frames + tail;
    if (!size_ok(total, ch)) return RV_ERR_CAPACITY;

    if (tail == 0) {                     /* old behaviour: no scratch copy */
        int rc = rv_sum(r, in, frames, out);
        return rc == RV_OK ? frames : rc;
    }

    float *padded = (float*)malloc((size_t)total * (size_t)ch * sizeof(float));
    if (!padded) return RV_ERR_NOMEM;
    int n = rv_pad(in, frames, ch, tail, padded);
    if (n < 0) { free(padded); return n; }
    int rc = rv_sum(r, padded, n, out);
    free(padded);
    return rc == RV_OK ? n : rc;
}

int rv_process_legacy(Reverb *r, const float *in, int frames, float *out) {
    return rv_process_tail(r, in, frames, 0, out);
}

int rv_process(Reverb *r, const float *in, int frames, float *out, int *tail_frames) {
    if (!r) return RV_ERR_ARG;
    int tail = 0;
    int rc = rv_tail_len(r->rate, r->decay_sec, &tail);
    if (rc != RV_OK) return rc;
    rc = rv_process_tail(r, in, frames, tail, out);
    if (rc >= 0 && tail_frames) *tail_frames = tail;
    return rc;
}

/* Closed-form wet response of the comb network at frame n, in double:
 *     hand(n) = wet * sum_t [ gain_t * fb_t^(n/delay_t - 1) ]   if delay_t | n
 * with fb being the RT60 coefficient.  The impulse sits alone on every channel
 * (dry = 0 in the rendered IR), so one scalar describes all channels. */
static double hand_value(const Reverb *r, int n) {
    double v = 0.0;
    if (n > 0) {
        for (int t = 0; t < r->taps; ++t) {
            int d = r->delay[t];
            if (d <= 0 || n % d != 0) continue;
            int k = n / d - 1;
            v += (double)r->gain[t] * pow((double)r->fb[t], (double)k);
        }
    }
    return (double)r->wet * v;
}

static int tap_count(const Reverb *r, int n) {
    int cnt = 0;
    if (n > 0)
        for (int t = 0; t < r->taps; ++t)
            if (r->delay[t] > 0 && n % r->delay[t] == 0) ++cnt;
    return cnt;
}

static double to_db(double v) {
    double a = fabs(v);
    return a > 0.0 ? 20.0 * log10(a) : -INFINITY;
}

int rv_export_curve(const Reverb *r, const float *ir, int ir_frames,
                    const char *path, double tol, RvCurveStats *stats) {
    if (!r || !ir || !path) return RV_ERR_ARG;
    if (ir_frames <= 0) return RV_ERR_EMPTY;
    int ch = r->channels;
    if (ch <= 0 || ch > RV_MAX_CHANNELS) return RV_ERR_CHANNELS;
    if (tol < 0.0) tol = 0.0;

    FILE *f = fopen(path, "wb");
    if (!f) return RV_ERR_IO;
    fprintf(f, "frame,hand,measured,abs_err,level_db_hand,level_db_measured,taps\n");

    if (stats) memset(stats, 0, sizeof(*stats));
    int mismatched = 0;
    double max_abs = 0.0, max_rel = 0.0;

    for (int n = 0; n < ir_frames; ++n) {
        double hand = hand_value(r, n);
        double measured = (double)ir[(size_t)n * (size_t)ch];   /* channel 0 */
        double abs_err = fabs(measured - hand);
        /* Every channel must carry the same response; fold that into abs_err. */
        for (int c = 1; c < ch; ++c) {
            double e = fabs((double)ir[(size_t)n * (size_t)ch + (size_t)c] - hand);
            if (e > abs_err) abs_err = e;
        }
        if (abs_err > max_abs) max_abs = abs_err;
        if (fabs(hand) > 1e-12) {
            double rel = abs_err / fabs(hand);
            if (rel > max_rel) max_rel = rel;
        }
        if (abs_err > tol) ++mismatched;
        fprintf(f, "%d,%.9g,%.9g,%.3g,%.6f,%.6f,%d\n",
                n, hand, measured, abs_err, to_db(hand), to_db(measured), tap_count(r, n));
    }
    if (fclose(f) != 0) return RV_ERR_IO;
    if (stats) {
        stats->rows = ir_frames;
        stats->mismatched = mismatched;
        stats->max_abs_err = max_abs;
        stats->max_rel_err = max_rel;
    }
    return RV_OK;
}
