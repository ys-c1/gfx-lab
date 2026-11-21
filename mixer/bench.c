/* Benchmark / conformance harness for two-stage per-bus compression.
 *
 * Runs a small (1 s, 4 buses) and a large (10 s, 16 buses) batch through
 * mx_process_buses(), timing stage 1 (estimation) and stage 2
 * (compression) separately, then:
 *
 *   - compares every bus's gain with an independently computed
 *     double-precision reference (the "error" column),
 *   - exports the per-bus comparison table (mx_write_bus_csv),
 *   - dumps raw float32 input/output per bus for ref_bus.py, which
 *     recomputes the whole table from scratch and checks it row by row,
 *   - checks that a one-bus batch is bit-identical to the legacy
 *     whole-segment call mx_compress_legacy(),
 *   - contrasts per-bus settings with the old single shared setting.
 *
 * The hand-table fixture (hand_fixture) is the same eight cases as
 * mixer/test.c and mixer/hand_table.csv; its exported CSV is
 * build/buses_hand.csv and ref_bus.py checks it one-to-one against
 * mixer/hand_table.csv.
 */
#define _POSIX_C_SOURCE 200809L
#include "mixer.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SR 48000
#define CH 2
#define MAX_BUSES_PER_CASE 16

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++failures; \
    printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double*)a, y = *(const double*)b;
    return (x > y) - (x < y);
}

/* Deterministic material so runs are reproducible. */
static unsigned long rng_state;
static float rngf(void) {
    rng_state = rng_state * 1103515245UL + 12345UL;
    return (float)((rng_state >> 8) & 0xffffff) / (float)0x1000000 * 2.0f - 1.0f;
}

static void fill_material(float *buf, int frames, int channels, int bus) {
    rng_state = 0x9e3779b9UL + (unsigned long)bus * 2654435761UL;
    float amp = 0.15f + 0.14f * (float)((bus * 7) % 10); /* 0.15 .. 1.41 */
    float f0 = 90.0f * (float)(1 + bus % 7);
    float phase = 0.37f * (float)bus;
    for (int i = 0; i < frames; ++i) {
        float t = (float)i / (float)SR;
        float env = 0.75f + 0.25f * sinf(0.4f * (float)bus + t * 1.7f);
        float common = sinf(2.0f * (float)M_PI * f0 * t + phase);
        float harm = 0.30f * sinf(2.0f * (float)M_PI * 3.0f * f0 * t + 0.5f * phase);
        float noise = 0.15f * rngf() * (float)(1 + bus % 3);
        float l = amp * env * (common + harm + noise);
        float r = amp * env * (common * 0.9f - harm + noise * 0.7f);
        buf[(size_t)i * channels + 0] = l;
        buf[(size_t)i * channels + 1] = r;
    }
}

/* Independent reference: level and gain computed in double from the raw
 * samples, no use of the library's estimate. */
static void reference_gain(const float *buf, int frames, int channels,
                           MxDetector det, float threshold, float ratio,
                           int enabled, double *level_out, double *gain_out) {
    size_t n = (size_t)frames * (size_t)channels;
    double ss = 0.0, peak = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double a = fabs((double)buf[i]);
        if (a > peak) peak = a;
        ss += (double)buf[i] * (double)buf[i];
    }
    double level = det == MX_DETECT_RMS ? sqrt(ss / (double)n) : peak;
    double gain = 1.0;
    if (enabled && ratio > 1.0 && level > (double)threshold)
        gain = pow(level / (double)threshold, 1.0 / ratio - 1.0);
    *level_out = level;
    *gain_out = gain;
}

static int dump_f32(const char *path, const float *p, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t w = fwrite(p, sizeof(float), n, f);
    fclose(f);
    return w == n ? 0 : -1;
}

/* Fixture for the checked-in hand table, identical to mixer/test.c. */
static const struct {
    const char *name;
    int frames, channels, enabled;
    MxDetector detector;
    float threshold, ratio;
    float samples[8];
} hand_rows[] = {
    { "peak_over",   8, 1, 1, MX_DETECT_PEAK, 0.5f,    2.0f, { 1,-1,1,-1,1,-1,1,-1 } },
    { "peak_at",     8, 1, 1, MX_DETECT_PEAK, 0.5f,    2.0f, { .5f,-.5f,.5f,-.5f,.5f,-.5f,.5f,-.5f } },
    { "rms_ratio4",  8, 1, 1, MX_DETECT_RMS,  0.5f,    4.0f, { .75f,-.75f,.75f,-.75f,.75f,-.75f,.75f,-.75f } },
    { "rms_sparse",  8, 1, 1, MX_DETECT_RMS,  0.5f,    2.0f, { 1,0,1,0,1,0,1,0 } },
    { "low_thr",     8, 1, 1, MX_DETECT_PEAK, 0.0625f, 4.0f, { .125f,-.125f,.125f,-.125f,.125f,-.125f,.125f,-.125f } },
    { "ratio_one",   8, 1, 1, MX_DETECT_PEAK, 0.5f,    1.0f, { .75f,-.75f,.75f,-.75f,.75f,-.75f,.75f,-.75f } },
    { "bypass",      8, 1, 0, MX_DETECT_PEAK, 0.5f,    2.0f, { 1,-1,1,-1,1,-1,1,-1 } },
    { "stereo_link", 2, 2, 1, MX_DETECT_PEAK, 0.5f,    2.0f, { 1,.1f,1,.1f } },
};
#define NHAND ((int)(sizeof(hand_rows) / sizeof(hand_rows[0])))

static void run_hand_table(void) {
    float bufs[NHAND][8];
    MxBusView buses[NHAND];
    MxCompCfg cfgs[NHAND];
    MxBusReport reps[NHAND];
    int count = 0;
    for (int i = 0; i < NHAND; ++i) {
        int n = hand_rows[i].frames * hand_rows[i].channels;
        memcpy(bufs[count], hand_rows[i].samples, (size_t)n * sizeof(float));
        buses[count] = (MxBusView){ bufs[count], hand_rows[i].frames, hand_rows[i].channels };
        cfgs[count] = (MxCompCfg){ hand_rows[i].detector, hand_rows[i].threshold,
                                   hand_rows[i].ratio, hand_rows[i].enabled };
        ++count;
    }
    int rc = mx_process_buses(buses, cfgs, count, reps);
    CHECK(rc == MX_OK, "hand fixture rc=%d (%s)", rc, mx_err_str(rc));
    if (rc != MX_OK) return;
    rc = mx_write_bus_csv(reps, count, "build/buses_hand.csv");
    CHECK(rc == MX_OK, "hand csv rc=%d", rc);
    printf("hand table: %d buses exported to build/buses_hand.csv "
           "(checked against mixer/hand_table.csv by ref_bus.py)\n", count);
}

static void run_scale(const char *scale, int frames, int bus_count) {
    size_t per_bus = (size_t)frames * CH;
    size_t total = per_bus * (size_t)bus_count;

    float *storage = (float*)malloc(total * sizeof(float));
    float *scratch = (float*)malloc(total * sizeof(float));
    float *work = (float*)malloc(total * sizeof(float));
    if (!storage || !scratch || !work) { printf("oom\n"); exit(1); }

    MxBusView buses[MAX_BUSES_PER_CASE];
    MxCompCfg cfgs[MAX_BUSES_PER_CASE];
    MxBusReport reps[MAX_BUSES_PER_CASE];

    for (int b = 0; b < bus_count; ++b) {
        float *p = storage + (size_t)b * per_bus;
        fill_material(p, frames, CH, b);
        buses[b] = (MxBusView){ p, frames, CH };
        cfgs[b].detector = (b % 2) ? MX_DETECT_RMS : MX_DETECT_PEAK;
        cfgs[b].threshold = (b % 5 == 4) ? 0.10f : (b % 3 == 1 ? 0.25f : 0.5f);
        cfgs[b].ratio = (float)(2 + (b % 3));        /* 2, 3, 4 */
        cfgs[b].enabled = (b % 7 == 6) ? 0 : 1;      /* one bypassed bus */
    }

    char meta_path[128];
    snprintf(meta_path, sizeof(meta_path), "build/bench_%s.meta", scale);
    FILE *meta = fopen(meta_path, "w");
    if (!meta) { printf("meta open failed\n"); exit(1); }
    fprintf(meta, "scale=%s\nframes=%d\nchannels=%d\nbuses=%d\n", scale, frames, CH, bus_count);
    for (int b = 0; b < bus_count; ++b)
        fprintf(meta, "bus=%d,%s,%.9g,%.9g,%d\n", b,
                cfgs[b].detector == MX_DETECT_RMS ? "rms" : "peak",
                cfgs[b].threshold, cfgs[b].ratio, cfgs[b].enabled);
    fclose(meta);

    /* Stage 1 timing (does not mutate). */
    const int REPS = 7;
    double t1[REPS], t2[REPS];
    MxLevel levels[MAX_BUSES_PER_CASE];
    for (int r = 0; r < REPS; ++r) {
        double t0 = now_s();
        int rc = mx_estimate_buses(buses, cfgs, bus_count, levels);
        t1[r] = now_s() - t0;
        CHECK(rc == MX_OK, "estimate rc=%d", rc);
    }
    /* Stage 2 timing on fresh copies; the copy is outside the timer. */
    for (int r = 0; r < REPS; ++r) {
        memcpy(work, storage, total * sizeof(float));
        for (int b = 0; b < bus_count; ++b)
            buses[b].buf = work + (size_t)b * per_bus;
        double t0 = now_s();
        int rc = mx_compress_buses(buses, cfgs, levels, bus_count, reps);
        t2[r] = now_s() - t0;
        CHECK(rc == MX_OK, "compress rc=%d", rc);
    }
    qsort(t1, REPS, sizeof(double), cmp_double);
    qsort(t2, REPS, sizeof(double), cmp_double);

    /* Final run on the pristine copy, with reports and dumps. */
    memcpy(work, storage, total * sizeof(float));
    for (int b = 0; b < bus_count; ++b)
        buses[b].buf = work + (size_t)b * per_bus;
    int rc = mx_process_buses(buses, cfgs, bus_count, reps);
    CHECK(rc == MX_OK, "process rc=%d (%s)", rc, mx_err_str(rc));
    if (rc != MX_OK) return;

    /* Per-bus error against an independent double-precision computation. */
    double max_gain_err = 0.0, max_level_err = 0.0;
    double overall_peak_before = 0.0, overall_peak_after = 0.0;
    long long total_samples = 0, total_compressed = 0;
    for (int b = 0; b < bus_count; ++b) {
        const float *in = storage + (size_t)b * per_bus;
        const float *out = work + (size_t)b * per_bus;
        double level, gain;
        reference_gain(in, frames, CH, cfgs[b].detector, cfgs[b].threshold,
                       cfgs[b].ratio, cfgs[b].enabled, &level, &gain);
        double lerr = fabs(level - (double)reps[b].level.level);
        double gerr = fabs(gain - (double)reps[b].gain);
        if (lerr > max_level_err) max_level_err = lerr;
        if (gerr > max_gain_err) max_gain_err = gerr;
        if (reps[b].peak_before > overall_peak_before) overall_peak_before = reps[b].peak_before;
        if (reps[b].peak_after > overall_peak_after) overall_peak_after = reps[b].peak_after;
        total_samples += reps[b].samples;
        total_compressed += reps[b].compressed_samples;

        /* Application check: out[i] == in[i] * reported gain, bit exact. */
        long long bad = 0;
        for (size_t i = 0; i < per_bus; ++i)
            if (out[i] != in[i] * reps[b].gain) ++bad;
        CHECK(bad == 0, "%s bus %d: %lld samples != in*gain", scale, b, bad);

        char path[160];
        snprintf(path, sizeof(path), "build/bench_%s_bus%d_in.f32", scale, b);
        CHECK(dump_f32(path, in, per_bus) == 0, "dump %s", path);
        snprintf(path, sizeof(path), "build/bench_%s_bus%d_out.f32", scale, b);
        CHECK(dump_f32(path, out, per_bus) == 0, "dump %s", path);
    }

    char csv_path[128];
    snprintf(csv_path, sizeof(csv_path), "build/buses_%s.csv", scale);
    rc = mx_write_bus_csv(reps, bus_count, csv_path);
    CHECK(rc == MX_OK, "csv rc=%d", rc);

    double ms_samples = (double)total_samples / 1e6;
    printf("\n[%s] %d frames, %d channels, %d buses, %.2f M samples\n",
           scale, frames, CH, bus_count, ms_samples);
    printf("  timing (median of %d): estimate %.3f ms, compress %.3f ms, "
           "total %.3f ms (%.0f Msample/s)\n",
           REPS, t1[REPS / 2] * 1e3, t2[REPS / 2] * 1e3,
           (t1[REPS / 2] + t2[REPS / 2]) * 1e3,
           (t1[REPS / 2] + t2[REPS / 2]) > 0.0
               ? ms_samples / ((t1[REPS / 2] + t2[REPS / 2]) * 1e-3) : 0.0);
    printf("  error vs independent double reference: max |level err| = %.3g, "
           "max |gain err| = %.3g\n", max_level_err, max_gain_err);
    printf("  overall peak before = %.6f, after = %.6f (%.3f dB)\n",
           overall_peak_before, overall_peak_after,
           20.0 * log10((double)overall_peak_after / (double)overall_peak_before));
    printf("  compressed samples: %lld / %lld (%.1f%%)\n",
           total_compressed, total_samples,
           100.0 * (double)total_compressed / (double)total_samples);

    printf("  %-3s %-5s %-5s %-6s %-9s %-9s %-8s %-9s %-9s %-9s\n",
           "bus", "det", "thr", "ratio", "level", "gain", "red dB",
           "peak pre", "peak post", "loud dB");
    for (int b = 0; b < bus_count; ++b) {
        const MxBusReport *r = &reps[b];
        printf("  %-3d %-5s %-5.3g %-6.3g %-9.5f %-9.5f %-8.3f %-9.5f %-9.5f %+8.3f\n",
               b, r->detector == MX_DETECT_RMS ? "rms" : "peak",
               r->threshold, r->ratio, r->level.level, r->gain,
               r->reduction_db, r->peak_before, r->peak_after,
               r->loudness_delta_db);
    }
    printf("  table exported: %s\n", csv_path);

    /* Legacy equivalence on bus 0: one-bus batch vs whole-segment call. */
    float *legacy = (float*)malloc(per_bus * sizeof(float));
    float *one = (float*)malloc(per_bus * sizeof(float));
    if (!legacy || !one) { printf("oom\n"); exit(1); }
    memcpy(legacy, storage, per_bus * sizeof(float));
    memcpy(one, storage, per_bus * sizeof(float));
    MxBusReport leg_rep, one_rep;
    rc = mx_compress_legacy(legacy, frames, CH, &cfgs[0], &leg_rep);
    CHECK(rc == MX_OK, "legacy rc=%d", rc);
    MxBusView one_bus = { one, frames, CH };
    rc = mx_process_buses(&one_bus, &cfgs[0], 1, &one_rep);
    CHECK(rc == MX_OK, "one-bus rc=%d", rc);
    int same = memcmp(legacy, one, per_bus * sizeof(float)) == 0 &&
               leg_rep.gain == one_rep.gain &&
               leg_rep.reduction_db == one_rep.reduction_db;
    CHECK(same, "legacy path differs from one-bus batch (%s)", scale);
    printf("  legacy whole-segment vs one-bus batch: %s\n",
           same ? "bit-identical" : "MISMATCH");

    /* Contrast with the old whole-segment behaviour: sum the stems into one
     * mix, take one level over that segment, apply one gain to everything.
     * With per-bus settings only the buses over their own threshold move. */
    double *sum = (double*)calloc(per_bus, sizeof(double));
    if (!sum) { printf("oom\n"); exit(1); }
    for (int b = 0; b < bus_count; ++b) {
        const float *p = storage + (size_t)b * per_bus;
        for (size_t i = 0; i < per_bus; ++i) sum[i] += (double)p[i];
    }
    double seg_ss = 0.0, seg_peak = 0.0;
    for (size_t i = 0; i < per_bus; ++i) {
        double a = fabs(sum[i]);
        if (a > seg_peak) seg_peak = a;
        seg_ss += sum[i] * sum[i];
    }
    double old_level = cfgs[0].detector == MX_DETECT_RMS
        ? sqrt(seg_ss / (double)per_bus) : seg_peak;
    double old_gain = 1.0;
    if (cfgs[0].enabled && cfgs[0].ratio > 1.0 && old_level > (double)cfgs[0].threshold)
        old_gain = pow(old_level / (double)cfgs[0].threshold,
                       1.0 / (double)cfgs[0].ratio - 1.0);
    double old_db = 20.0 * log10(old_gain);
    int quiet_untouched = 0, changed_by_old = 0;
    for (int b = 0; b < bus_count; ++b) {
        const MxBusReport *r = &reps[b];
        if (r->reduction_db == 0.0f) ++quiet_untouched;
        if (fabs((double)r->gain_db - old_db) > 0.1) ++changed_by_old;
    }
    printf("  per-bus (%s, thr %.3g, ratio %.3g): %d/%d buses left untouched\n",
           cfgs[0].detector == MX_DETECT_RMS ? "rms" : "peak",
           cfgs[0].threshold, cfgs[0].ratio, quiet_untouched, bus_count);
    printf("  old one-setting on the summed segment: level %.5f -> one gain "
           "%.4f (%.3f dB) for every bus; %d/%d buses would get a different "
           "reduction than their own threshold asks for\n",
           old_level, old_gain, old_db, changed_by_old, bus_count);
    free(sum);

    free(legacy); free(one);
    free(storage); free(scratch); free(work);
}

int main(void) {
    run_hand_table();
    run_scale("small", 1 * SR, 4);
    run_scale("large", 10 * SR, 16);
    if (failures) {
        printf("\n%d benchmark check(s) FAILED\n", failures);
        return 1;
    }
    printf("\nall benchmark checks passed\n");
    return 0;
}
