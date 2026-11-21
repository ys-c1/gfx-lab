#ifndef MIXER_H
#define MIXER_H

/* Upper bound on buses in one batch call. */
#define MX_MAX_BUSES 64

/* Error codes for the compression API.  The legacy mixer calls
 * (mx_init / mx_add / mx_write_wav) keep their historical -1 / -2 codes. */
#define MX_OK             0
#define MX_ERR_ARG      (-1)   /* NULL pointer argument                      */
#define MX_ERR_EMPTY    (-2)   /* empty input: frames <= 0 or bus count <= 0 */
#define MX_ERR_CHANNELS (-3)   /* channels <= 0                              */
#define MX_ERR_PARAM    (-4)   /* threshold/ratio/detector/level invalid     */
#define MX_ERR_BUSES    (-5)   /* bus count > MX_MAX_BUSES                   */
#define MX_ERR_IO       (-6)   /* file could not be written                  */
#define MX_ERR_MEM      (-7)   /* reserved; the compression path allocates
                                  nothing and never returns this              */
#define MX_ERR_NONFINITE (-8)  /* NaN or Inf found in a bus                  */

typedef struct { float *buf; int frames; int channels; int cap; } MixBus;
int mx_init(MixBus *b, int cap_frames, int channels);
void mx_free(MixBus *b);
int mx_add(MixBus *b, const float *src, int frames, float gain);
int mx_write_wav(const MixBus *b, const char *path);

/* ------------------------------------------------------------------ *
 * Two-stage per-bus compression.
 *
 * Stage 1 (mx_estimate / mx_estimate_buses) measures each bus on its own:
 *     peak = max |x|,  rms = sqrt(mean(x^2))
 * over every frame and channel of that bus.
 *
 * Stage 2 (mx_compress / mx_compress_buses) gives every bus its own
 * threshold and ratio and applies one static gain to the whole bus:
 *
 *     gain = (level / threshold) ^ (1/ratio - 1)     when level > threshold
 *     gain = 1                                       otherwise
 *
 * where level is the bus's stage-1 peak or RMS according to cfg.detector.
 * Because the gain is derived per bus and applied to every channel, the
 * channels of a bus stay locked together (no stereo image shift), while
 * each bus is levelled against its own threshold instead of one setting
 * shared by the whole mix.
 *
 * Exact no-ops: enabled == 0, ratio == 1, or level <= threshold leaves
 * every sample bit-identical.  A single-bus batch is bit-identical to
 * mx_compress_legacy(), which reproduces the old whole-segment behaviour.
 *
 * Loudness in the report is RMS dBFS (20 log10 rms), not LUFS.
 * ------------------------------------------------------------------ */

typedef enum { MX_DETECT_PEAK = 0, MX_DETECT_RMS = 1 } MxDetector;

/* One bus of interleaved float samples (frames * channels).  Stage 2
 * rewrites buf in place. */
typedef struct {
    float *buf;
    int frames;
    int channels;
} MxBusView;

typedef struct {
    MxDetector detector;  /* stage-1 level that feeds the gain computer */
    float threshold;      /* linear, in (0, 1]                          */
    float ratio;          /* >= 1 and finite; 1 disables reduction      */
    int enabled;          /* 0 = bypass this bus                        */
} MxCompCfg;

/* Stage-1 result for one bus.  Carries its own shape so a stage-2 call
 * can reject an estimate taken from a different bus. */
typedef struct {
    float peak;
    float rms;
    float level;          /* detector-selected level                    */
    int frames;
    int channels;
    long long samples;
} MxLevel;

/* Stage-2 result for one bus. */
typedef struct {
    int bus;
    int enabled;
    MxDetector detector;
    float threshold;
    float ratio;
    MxLevel level;                /* stage-1 estimate used              */
    float gain;                   /* applied linear gain                */
    float gain_db;                /* 20 log10(gain)                     */
    float reduction_db;           /* max(0, -gain_db)                   */
    float peak_before, peak_after;
    float rms_before, rms_after;
    float loudness_before_dbfs;
    float loudness_after_dbfs;
    float loudness_delta_db;      /* after - before                     */
    long long samples_over_threshold; /* |x| > threshold before gain    */
    long long compressed_samples;     /* samples with gain < 1 (= all)  */
    long long samples;                /* frames * channels              */
} MxBusReport;

/* Stage 1, single bus. */
int mx_estimate(const MxBusView *bus, MxDetector detector, MxLevel *out);

/* Stage 1, batch.  levels[i] receives buses[i]'s estimate. */
int mx_estimate_buses(const MxBusView *buses, const MxCompCfg *cfgs,
                      int count, MxLevel *levels);

/* Stage 2, single bus.  If level is NULL it is measured here from
 * cfg->detector; if supplied, its shape and finiteness are validated.
 * report may be NULL.  On error the bus may be left untouched or, if the
 * error is found while applying, partially written; batch calls validate
 * everything first so they are all-or-nothing. */
int mx_compress(MxBusView *bus, const MxCompCfg *cfg,
                const MxLevel *level, MxBusReport *report);

/* Stage 2, batch.  Validates every bus before modifying any of them.
 * levels may be NULL (each bus is measured).  reports may be NULL;
 * reports[i].bus is set to i. */
int mx_compress_buses(MxBusView *buses, const MxCompCfg *cfgs,
                      const MxLevel *levels, int count, MxBusReport *reports);

/* Both stages for a batch.  Equivalent to mx_estimate_buses() followed by
 * mx_compress_buses(). */
int mx_process_buses(MxBusView *buses, const MxCompCfg *cfgs,
                     int count, MxBusReport *reports);

/* Legacy whole-segment behaviour: one buffer, one cfg, one level for the
 * entire segment.  Bit-identical to a one-bus mx_process_buses(). */
int mx_compress_legacy(float *buf, int frames, int channels,
                       const MxCompCfg *cfg, MxBusReport *report);

/* Export the per-bus comparison table as CSV: a header row followed by
 * one row per bus, with the columns of MxBusReport. */
int mx_write_bus_csv(const MxBusReport *reports, int count, const char *path);

/* Human-readable reason for an MX_ERR_* code. */
const char *mx_err_str(int err);

#endif
