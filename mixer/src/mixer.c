#include "mixer.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ *
 * Legacy bus / WAV code.  Kept exactly as it was, including its -1 / -2
 * return codes, so existing callers see no change.  The MX_ERR_* codes
 * below belong to the compression API only.
 * ------------------------------------------------------------------ */

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

const char *mx_err_str(int err) {
    switch (err) {
        case MX_OK:              return "ok";
        case MX_ERR_ARG:         return "null pointer argument";
        case MX_ERR_EMPTY:       return "empty input: frames <= 0, or bus count <= 0 (nothing to compress)";
        case MX_ERR_CHANNELS:    return "channels <= 0";
        case MX_ERR_PARAM:       return "threshold must be in (0,1], ratio must be >= 1 and finite, detector must be PEAK or RMS";
        case MX_ERR_BUSES:       return "bus count exceeds MX_MAX_BUSES";
        case MX_ERR_IO:          return "file could not be opened for writing";
        case MX_ERR_MEM:         return "scratch allocation failed (reserved; not used by the compression path)";
        case MX_ERR_NONFINITE:   return "bus contains NaN or Inf";
        default:                 return "unknown error";
    }
}

/* ------------------------------------------------------------------ *
 * Shared validation and measurement
 * ------------------------------------------------------------------ */

static int cfg_valid(const MxCompCfg *cfg) {
    if (!cfg) return MX_ERR_ARG;
    if (cfg->detector != MX_DETECT_PEAK && cfg->detector != MX_DETECT_RMS)
        return MX_ERR_PARAM;
    if (!isfinite(cfg->threshold) || !(cfg->threshold > 0.0f) || !(cfg->threshold <= 1.0f))
        return MX_ERR_PARAM;
    if (!isfinite(cfg->ratio) || !(cfg->ratio >= 1.0f))
        return MX_ERR_PARAM;
    return MX_OK;
}

/* Structural checks only; does not touch samples. */
static int view_valid(const MxBusView *bus) {
    if (!bus) return MX_ERR_ARG;
    if (bus->frames <= 0) return MX_ERR_EMPTY;
    if (bus->channels <= 0) return MX_ERR_CHANNELS;
    if (!bus->buf) return MX_ERR_ARG;
    if ((unsigned long long)bus->frames >
        (unsigned long long)SIZE_MAX / (unsigned long long)bus->channels)
        return MX_ERR_PARAM;
    return MX_OK;
}

/* Stage-1 measurement: peak, RMS and the detector-selected level.
 * Sums are accumulated in double so a large bus does not lose precision;
 * the final estimates are rounded to float once. */
static int measure(const MxBusView *bus, MxDetector detector, MxLevel *out) {
    if (!out) return MX_ERR_ARG;
    long long n = (long long)bus->frames * (long long)bus->channels;
    double ss = 0.0;
    float peak = 0.0f;
    const float *p = bus->buf;
    for (long long i = 0; i < n; ++i) {
        float v = p[i];
        if (!isfinite(v)) return MX_ERR_NONFINITE;
        float a = fabsf(v);
        if (a > peak) peak = a;
        ss += (double)v * (double)v;
    }
    float rms = (float)sqrt(ss / (double)n);
    out->peak = peak;
    out->rms = rms;
    out->level = detector == MX_DETECT_RMS ? rms : peak;
    out->frames = bus->frames;
    out->channels = bus->channels;
    out->samples = n;
    return MX_OK;
}

static float gain_for(float level, const MxCompCfg *cfg) {
    if (!cfg->enabled || !(cfg->ratio > 1.0f) || !(level > cfg->threshold))
        return 1.0f;
    /* Double precision here keeps the static curve exact to the hand table;
     * the applied gain is rounded to float once. */
    double g = pow((double)level / (double)cfg->threshold,
                   1.0 / (double)cfg->ratio - 1.0);
    float gf = (float)g;
    if (!(gf < 1.0f)) return 1.0f;
    if (gf < 0.0f) return 0.0f;
    return gf;
}

static int level_matches(const MxLevel *lvl, const MxBusView *bus) {
    if (!lvl) return 1; /* NULL means "estimate it" */
    if (lvl->frames != bus->frames || lvl->channels != bus->channels) return 0;
    if (lvl->samples != (long long)bus->frames * (long long)bus->channels) return 0;
    if (!isfinite(lvl->level) || !isfinite(lvl->peak) || !isfinite(lvl->rms)) return 0;
    return 1;
}

/* ------------------------------------------------------------------ *
 * Stage 1: per-bus level estimation
 * ------------------------------------------------------------------ */

int mx_estimate(const MxBusView *bus, MxDetector detector, MxLevel *out) {
    int rc = view_valid(bus);
    if (rc != MX_OK) return rc;
    if (!out) return MX_ERR_ARG;
    if (detector != MX_DETECT_PEAK && detector != MX_DETECT_RMS) return MX_ERR_PARAM;
    return measure(bus, detector, out);
}

int mx_estimate_buses(const MxBusView *buses, const MxCompCfg *cfgs,
                      int count, MxLevel *levels) {
    if (!buses || !cfgs || !levels) return MX_ERR_ARG;
    if (count <= 0) return MX_ERR_EMPTY;
    if (count > MX_MAX_BUSES) return MX_ERR_BUSES;
    for (int i = 0; i < count; ++i) {
        int rc = view_valid(&buses[i]);
        if (rc != MX_OK) return rc;
        rc = cfg_valid(&cfgs[i]);
        if (rc != MX_OK) return rc;
    }
    for (int i = 0; i < count; ++i) {
        int rc = measure(&buses[i], cfgs[i].detector, &levels[i]);
        if (rc != MX_OK) return rc;
    }
    return MX_OK;
}

/* ------------------------------------------------------------------ *
 * Stage 2: per-bus compression
 * ------------------------------------------------------------------ */

/* Single pass over a bus to reject NaN/Inf without measuring anything. */
static int scan_finite(const MxBusView *bus) {
    long long n = (long long)bus->frames * (long long)bus->channels;
    const float *p = bus->buf;
    for (long long i = 0; i < n; ++i)
        if (!isfinite(p[i])) return MX_ERR_NONFINITE;
    return MX_OK;
}

/* Apply one validated gain to a bus and fill the report.  No validation
 * happens here; callers do it first so a batch is all-or-nothing. */
static void apply_gain(MxBusView *bus, const MxCompCfg *cfg, const MxLevel *level,
                       float gain, MxBusReport *report) {
    long long n = level->samples;
    float peak_before = 0.0f, peak_after = 0.0f;
    double ss_before = 0.0, ss_after = 0.0;
    long long over = 0;
    for (long long i = 0; i < n; ++i) {
        float x = bus->buf[i];
        float ax = fabsf(x);
        if (ax > peak_before) peak_before = ax;
        if (ax > cfg->threshold) ++over;
        ss_before += (double)x * (double)x;
        float y = x * gain;
        bus->buf[i] = y;
        float ay = fabsf(y);
        if (ay > peak_after) peak_after = ay;
        ss_after += (double)y * (double)y;
    }
    if (!report) return;

    report->bus = 0;
    report->enabled = cfg->enabled ? 1 : 0;
    report->detector = cfg->detector;
    report->threshold = cfg->threshold;
    report->ratio = cfg->ratio;
    report->level = *level;
    report->gain = gain;
    report->gain_db = gain < 1.0f ? (float)(20.0 * log10((double)gain)) : 0.0f;
    report->reduction_db = gain < 1.0f ? -report->gain_db : 0.0f;
    report->peak_before = peak_before;
    report->peak_after = peak_after;
    report->rms_before = (float)sqrt(ss_before / (double)n);
    report->rms_after = (float)sqrt(ss_after / (double)n);
    report->loudness_before_dbfs = report->rms_before > 0.0f
        ? (float)(20.0 * log10((double)report->rms_before)) : -INFINITY;
    report->loudness_after_dbfs = report->rms_after > 0.0f
        ? (float)(20.0 * log10((double)report->rms_after)) : -INFINITY;
    report->loudness_delta_db =
        (report->rms_before > 0.0f && report->rms_after > 0.0f)
            ? report->loudness_after_dbfs - report->loudness_before_dbfs : 0.0f;
    report->compressed_samples = gain < 1.0f ? n : 0;
    report->samples_over_threshold = over;
    report->samples = n;
}

int mx_compress(MxBusView *bus, const MxCompCfg *cfg,
                const MxLevel *level, MxBusReport *report) {
    int rc = view_valid(bus);
    if (rc != MX_OK) return rc;
    rc = cfg_valid(cfg);
    if (rc != MX_OK) return rc;
    if (!level_matches(level, bus)) return MX_ERR_PARAM;

    MxLevel est;
    if (!level) {
        rc = measure(bus, cfg->detector, &est);
        if (rc != MX_OK) return rc;
        level = &est;
    } else {
        /* A supplied stage-1 result does not excuse bad sample data. */
        rc = scan_finite(bus);
        if (rc != MX_OK) return rc;
    }

    apply_gain(bus, cfg, level, gain_for(level->level, cfg), report);
    return MX_OK;
}

int mx_compress_buses(MxBusView *buses, const MxCompCfg *cfgs,
                      const MxLevel *levels, int count, MxBusReport *reports) {
    if (!buses || !cfgs) return MX_ERR_ARG;
    if (count <= 0) return MX_ERR_EMPTY;
    if (count > MX_MAX_BUSES) return MX_ERR_BUSES;

    /* Validate every bus (structure, config, supplied level, sample data)
     * before modifying any of them: the batch is all-or-nothing. */
    for (int i = 0; i < count; ++i) {
        int rc = view_valid(&buses[i]);
        if (rc != MX_OK) return rc;
        rc = cfg_valid(&cfgs[i]);
        if (rc != MX_OK) return rc;
        if (!level_matches(levels ? &levels[i] : NULL, &buses[i])) return MX_ERR_PARAM;
        rc = scan_finite(&buses[i]);
        if (rc != MX_OK) return rc;
    }

    for (int i = 0; i < count; ++i) {
        MxLevel est;
        const MxLevel *lvl = levels ? &levels[i] : NULL;
        if (!lvl) {
            int rc = measure(&buses[i], cfgs[i].detector, &est); /* already finite */
            if (rc != MX_OK) return rc;
            lvl = &est;
        }
        MxBusReport rep;
        apply_gain(&buses[i], &cfgs[i], lvl, gain_for(lvl->level, &cfgs[i]), &rep);
        rep.bus = i;
        if (reports) reports[i] = rep;
    }
    return MX_OK;
}

int mx_process_buses(MxBusView *buses, const MxCompCfg *cfgs,
                     int count, MxBusReport *reports) {
    if (!buses || !cfgs) return MX_ERR_ARG;
    if (count <= 0) return MX_ERR_EMPTY;
    if (count > MX_MAX_BUSES) return MX_ERR_BUSES;

    MxLevel levels[MX_MAX_BUSES];
    for (int i = 0; i < count; ++i) {
        int rc = view_valid(&buses[i]);
        if (rc != MX_OK) return rc;
        rc = cfg_valid(&cfgs[i]);
        if (rc != MX_OK) return rc;
    }
    int rc = mx_estimate_buses(buses, cfgs, count, levels);
    if (rc != MX_OK) return rc;
    return mx_compress_buses(buses, cfgs, levels, count, reports);
}

/* The old whole-segment path: a single parameter set and one level taken
 * over the entire segment.  Implemented as the one-bus case so the
 * identity is bit-for-bit by construction. */
int mx_compress_legacy(float *buf, int frames, int channels,
                       const MxCompCfg *cfg, MxBusReport *report) {
    MxBusView bus = { buf, frames, channels };
    int rc = mx_compress(&bus, cfg, NULL, report);
    if (rc == MX_OK && report) report->bus = 0;
    return rc;
}

/* ------------------------------------------------------------------ *
 * Per-bus comparison table (CSV)
 * ------------------------------------------------------------------ */

static const char *detector_name(MxDetector d) {
    return d == MX_DETECT_RMS ? "rms" : "peak";
}

int mx_write_bus_csv(const MxBusReport *reports, int count, const char *path) {
    if (!reports || !path) return MX_ERR_ARG;
    if (count <= 0) return MX_ERR_EMPTY;
    FILE *f = fopen(path, "w");
    if (!f) return MX_ERR_IO;
    fprintf(f, "bus,enabled,detector,threshold,ratio,level,gain,gain_db,"
               "reduction_db,peak_before,peak_after,rms_before,rms_after,"
               "loudness_before_dbfs,loudness_after_dbfs,loudness_delta_db,"
               "samples_over_threshold,compressed_samples,samples,frames,channels\n");
    for (int i = 0; i < count; ++i) {
        const MxBusReport *r = &reports[i];
        fprintf(f, "%d,%d,%s,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,"
                   "%.9g,%.9g,%.9g,%lld,%lld,%lld,%d,%d\n",
                r->bus, r->enabled, detector_name(r->detector),
                r->threshold, r->ratio, r->level.level, r->gain, r->gain_db,
                r->reduction_db, r->peak_before, r->peak_after,
                r->rms_before, r->rms_after, r->loudness_before_dbfs,
                r->loudness_after_dbfs, r->loudness_delta_db,
                r->samples_over_threshold, r->compressed_samples, r->samples,
                r->level.frames, r->level.channels);
    }
    if (fclose(f) != 0) return MX_ERR_IO;
    return MX_OK;
}
