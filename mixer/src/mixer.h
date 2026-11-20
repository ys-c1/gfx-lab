#ifndef MIXER_H
#define MIXER_H

typedef struct { float *buf; int frames; int channels; int cap; } MixBus;

int mx_init(MixBus *b, int cap_frames, int channels);
void mx_free(MixBus *b);
int mx_add(MixBus *b, const float *src, int frames, float gain);
int mx_write_wav(const MixBus *b, const char *path);

/* ---- peak limiter ---------------------------------------------------- */

typedef struct {
    float threshold;      /* linear full-scale ceiling, (0.0, 1.0] */
    int   sample_rate;    /* Hz, > 0 */
    float attack_ms;      /* look-ahead window, > 0 */
    float release_ms;     /* recovery time constant, > 0 */
    int   enabled;        /* 0 = bypass, output is bit-identical to input */
} MxLimitCfg;

typedef struct {
    float peak_before;    /* max |x| over all frames/channels, pre-limiter */
    float peak_after;     /* max |y| post-limiter (always <= threshold) */
    int   plateau_runs;   /* number of runs of >=2 consecutive samples on the ceiling */
    int   plateau_samples;/* total samples inside those runs */
    int   limited_samples;/* samples whose gain was below 1.0 */
} MxLimitReport;

/* error codes shared by the limiter APIs; legacy mx_* still use -1 */
enum {
    MX_OK = 0,
    MX_ERR_NULL = -10,
    MX_ERR_PARAM = -11,
    MX_ERR_CHANNELS = -12
};

/* per-channel gain-additive add; returns frames added, or a negative MX_ERR_* */
int mx_add_ex(MixBus *b, const float *src, int frames, int channels, float gain);

/* in-place look-ahead brick-wall limiter; *rep filled only when enabled */
int mx_limit(MixBus *b, const MxLimitCfg *cfg, MxLimitReport *rep);

/* human-readable name of the offending parameter for an MX_ERR_* code */
const char *mx_err_str(int code);

#endif
