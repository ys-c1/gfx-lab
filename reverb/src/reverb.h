#ifndef REVERB_H
#define REVERB_H

#include <stdio.h>

/* Feedback-comb reverb with an explicit tail stage.
 *
 * The renderer is split in three stages so the tail can be handled on its own:
 *   1) rv_tail_len()  - tail length in frames from the RT60 decay time;
 *   2) rv_pad()       - copy the input and append that many zero frames;
 *   3) rv_sum()       - per-sample dry/wet sum while running the comb lines.
 *
 * rv_process()/rv_process_tail() chain the three stages.  The old behaviour
 * (render the input only, drop the tail) is rv_process_legacy(), which is
 * bit-identical to rv_process_tail(..., tail = 0, ...) and to the prefix of
 * the full three-stage result (the network is causal).
 *
 * Each tap is a feedback comb of `delay` samples whose feedback coefficient is
 * chosen so the tap has fallen by 60 dB after `decay_sec` seconds:
 *     fb = 10 ^ (-3 * delay / (decay_sec * rate))
 * so a tail of ceil(decay_sec * rate) frames lets the network ring out. */

#define RV_MAX_TAPS 8
#define RV_MAX_CHANNELS 8

/* Error codes.  rv_strerror() explains what is wrong and which field. */
enum {
    RV_OK = 0,
    RV_ERR_ARG      = -1,   /* null pointer                                  */
    RV_ERR_EMPTY    = -2,   /* input is empty: frames <= 0                   */
    RV_ERR_RATE     = -3,   /* 'rate' must be > 0                            */
    RV_ERR_DECAY    = -4,   /* 'decay_sec' must be finite and > 0            */
    RV_ERR_CHANNELS = -5,   /* 'channels' must be 1..RV_MAX_CHANNELS         */
    RV_ERR_DELAY    = -6,   /* every tap 'delay' must be > 0                 */
    RV_ERR_CAPACITY = -7,   /* frame count does not fit in int               */
    RV_ERR_IO       = -8,   /* decay curve file could not be written         */
    RV_ERR_NOMEM    = -9,   /* allocation failed                             */
    RV_ERR_TAPS     = -10,  /* 'taps' must be 1..RV_MAX_TAPS                 */
    RV_ERR_GAIN     = -11,  /* every tap 'gain' must be finite and >= 0      */
    RV_ERR_TAIL     = -12,  /* 'tail' must be >= 0 frames                    */
    RV_ERR_ALIAS    = -13   /* input and output overlap                      */
};

typedef struct {
    int channels;
    int rate;
    int taps;
    float decay_sec;                 /* RT60, seconds                     */
    int delay[RV_MAX_TAPS];          /* samples, > 0                      */
    float gain[RV_MAX_TAPS];         /* wet gain of each tap              */
    float fb[RV_MAX_TAPS];           /* feedback from decay_sec/delay     */
    float *line[RV_MAX_TAPS];        /* delay * channels floats           */
    int pos[RV_MAX_TAPS];
    float dry, wet;
} Reverb;

typedef struct {
    int rows;                        /* arrival rows written              */
    int mismatched;                  /* rows with |measured - hand| > tol */
    double max_abs_err;
    double max_rel_err;
} RvCurveStats;

const char *rv_strerror(int code);

/* Feedback coefficient of one comb; 0 if the arguments are invalid. */
float rv_tap_feedback(int delay, int rate, float decay_sec);

/* Configure the network.  Lines are allocated and zeroed; dry=1, wet=0,
 * which is the old behaviour. */
int rv_init(Reverb *r, int channels, int rate, int taps,
            const int *delay, const float *gain, float decay_sec);
void rv_free(Reverb *r);
void rv_reset(Reverb *r);
void rv_set_mix(Reverb *r, float dry, float wet);

/* Stage 1: tail length for an RT60 decay time.
 * *tail_frames = ceil(decay_sec * rate), at least 1, only written on RV_OK. */
int rv_tail_len(int rate, float decay_sec, int *tail_frames);

/* Stage 2: copy `frames` interleaved frames and append `tail` zero frames.
 * `padded` holds (frames + tail) * channels floats; it must not alias `in`.
 * Returns the padded frame count.  tail == 0 is an exact copy of the input. */
int rv_pad(const float *in, int frames, int channels, int tail, float *padded);

/* Stage 3: run the comb network over an already padded buffer and write
 * out = dry * padded + wet * network for every sample.  The network state is
 * advanced by padded_frames. */
int rv_sum(Reverb *r, const float *padded, int padded_frames, float *out);

/* Run `tail_frames` zero-input samples through the network (drain the tail).
 * Used to end a streamed block sequence without inserting silence between
 * blocks.  Returns RV_OK for tail_frames == 0 (no work). */
int rv_flush(Reverb *r, int tail_frames, float *out);

/* Old behaviour: render the input only, no padding, tail is dropped. */
int rv_process_legacy(Reverb *r, const float *in, int frames, float *out);

/* Stages 2+3 with an explicit tail length.  tail == 0 is the old behaviour;
 * otherwise the output is (frames + tail) frames and out must hold that many.
 * Returns the number of frames written. */
int rv_process_tail(Reverb *r, const float *in, int frames, int tail, float *out);

/* All three stages: derive the tail from the configured decay time, pad,
 * render.  Returns the number of frames written; when tail_frames is non-NULL
 * it receives the derived tail length. */
int rv_process(Reverb *r, const float *in, int frames, float *out,
               int *tail_frames);

/* Export the decay curve of the wet path, one row per arrival frame:
 *     frame, hand, measured, abs_err, level_db, taps
 * `ir` is the impulse response rendered with the same Reverb (dry = 0 so the
 * impulse itself is not in the wave, wet may be anything), `ir_frames` frames.
 * `hand` is the closed-form value computed in double precision from
 * gain[t] * fb[t]^(n/delay[t] - 1) summed over every tap arriving at n.
 * Every row is compared against the hand table; stats->mismatched counts rows
 * with |measured - hand| > tol.  Returns RV_OK, or RV_ERR_IO if path fails. */
int rv_export_curve(const Reverb *r, const float *ir, int ir_frames,
                    const char *path, double tol, RvCurveStats *stats);

#endif
