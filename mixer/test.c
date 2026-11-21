#include "mixer.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

/* ------------------------------------------------------------------ *
 * Legacy bus tests (unchanged behaviour)
 * ------------------------------------------------------------------ */

static void test_basic_mix(void) {
    MixBus b;
    CK(mx_init(&b, 8, 2) == 0);
    float a[8], c[8];
    for (int i = 0; i < 8; ++i) { a[i] = 0.25f; c[i] = 0.5f; }
    CK(mx_add(&b, a, 4, 1.0f) == 4);
    CK(mx_add(&b, c, 4, 0.5f) == 4);
    CK(fabsf(b.buf[0] - 0.25f) < 1e-6f);
    CK(fabsf(b.buf[8] - 0.25f) < 1e-6f);
    CK(mx_add(&b, a, 4, 1.0f) == 0);
    mx_free(&b);
}

/* ------------------------------------------------------------------ *
 * Hand-computed table: one row per bus, checked field by field.
 *
 * gain = (level/threshold)^(1/ratio - 1) when enabled && ratio > 1 &&
 * level > threshold, else 1.  Values are the ones in mixer/README.md,
 * computed independently in double precision and rounded to float.
 * ------------------------------------------------------------------ */

typedef struct {
    const char *name;
    int frames, channels, enabled;
    MxDetector detector;
    float threshold, ratio;
    float level, gain, gain_db, reduction_db;
    float peak_before, peak_after, rms_before, rms_after;
    float loudness_before, loudness_after, loudness_delta;
    long long over, compressed, samples;
} Row;

static const Row rows[] = {
    { "peak_over", 8, 1, 1, MX_DETECT_PEAK, 0.5f, 2.0f,
      1.0f, 0.707106769f, -3.01030016f, 3.01030016f,
      1.0f, 0.707106769f, 1.0f, 0.707106769f,
      0.0f, -3.01030016f, -3.01030016f, 8, 8, 8 },
    { "peak_at", 8, 1, 1, MX_DETECT_PEAK, 0.5f, 2.0f,
      0.5f, 1.0f, 0.0f, 0.0f,
      0.5f, 0.5f, 0.5f, 0.5f,
      -6.02059984f, -6.02059984f, 0.0f, 0, 0, 8 },
    { "rms_ratio4", 8, 1, 1, MX_DETECT_RMS, 0.5f, 4.0f,
      0.75f, 0.737787962f, -2.64136863f, 2.64136863f,
      0.75f, 0.553340971f, 0.75f, 0.553340971f,
      -2.49877477f, -5.14014339f, -2.64136863f, 8, 8, 8 },
    { "rms_sparse", 8, 1, 1, MX_DETECT_RMS, 0.5f, 2.0f,
      0.707106769f, 0.840896428f, -1.50514984f, 1.50514984f,
      1.0f, 0.840896428f, 0.707106769f, 0.594603539f,
      -3.01030016f, -4.51545f, -1.50514984f, 4, 8, 8 },
    { "low_thr", 8, 1, 1, MX_DETECT_PEAK, 0.0625f, 4.0f,
      0.125f, 0.594603539f, -4.51545f, 4.51545f,
      0.125f, 0.0743254423f, 0.125f, 0.0743254423f,
      -18.0618f, -22.5772495f, -4.51544952f, 8, 8, 8 },
    { "ratio_one", 8, 1, 1, MX_DETECT_PEAK, 0.5f, 1.0f,
      0.75f, 1.0f, 0.0f, 0.0f,
      0.75f, 0.75f, 0.75f, 0.75f,
      -2.49877477f, -2.49877477f, 0.0f, 8, 0, 8 },
    { "bypass", 8, 1, 0, MX_DETECT_PEAK, 0.5f, 2.0f,
      1.0f, 1.0f, 0.0f, 0.0f,
      1.0f, 1.0f, 1.0f, 1.0f,
      0.0f, 0.0f, 0.0f, 8, 0, 8 },
    { "stereo_link", 2, 2, 1, MX_DETECT_PEAK, 0.5f, 2.0f,
      1.0f, 0.707106769f, -3.01030016f, 3.01030016f,
      1.0f, 0.707106769f, 0.710633516f, 0.502493799f,
      -2.96708632f, -5.977386f, -3.01029968f, 2, 4, 4 },
};

#define NROW ((int)(sizeof(rows) / sizeof(rows[0])))

/* Fixture data matching the table. */
static void fill_bus(int i, float *buf) {
    static const float peak_over[8]   = { 1,-1,1,-1,1,-1,1,-1 };
    static const float peak_at[8]     = { .5f,-.5f,.5f,-.5f,.5f,-.5f,.5f,-.5f };
    static const float rms_ratio4[8]  = { .75f,-.75f,.75f,-.75f,.75f,-.75f,.75f,-.75f };
    static const float rms_sparse[8]  = { 1,0,1,0,1,0,1,0 };
    static const float low_thr[8]     = { .125f,-.125f,.125f,-.125f,.125f,-.125f,.125f,-.125f };
    static const float ratio_one[8]   = { .75f,-.75f,.75f,-.75f,.75f,-.75f,.75f,-.75f };
    static const float bypass[8]      = { 1,-1,1,-1,1,-1,1,-1 };
    static const float stereo_link[4] = { 1, .1f, 1, .1f };
    const float *src[NROW] = { peak_over, peak_at, rms_ratio4, rms_sparse,
                               low_thr, ratio_one, bypass, stereo_link };
    memcpy(buf, src[i], (size_t)rows[i].samples * sizeof(float));
}

static int nearf(float got, float want, float tol) {
    return fabsf(got - want) <= tol * (1.0f + fabsf(want));
}

static void test_hand_table(void) {
    float buf[NROW][8];
    MxBusView buses[NROW];
    MxCompCfg cfgs[NROW];
    MxLevel levels[NROW];
    MxBusReport reps[NROW];

    for (int i = 0; i < NROW; ++i) {
        fill_bus(i, buf[i]);
        buses[i].buf = buf[i];
        buses[i].frames = rows[i].frames;
        buses[i].channels = rows[i].channels;
        cfgs[i].detector = rows[i].detector;
        cfgs[i].threshold = rows[i].threshold;
        cfgs[i].ratio = rows[i].ratio;
        cfgs[i].enabled = rows[i].enabled;
    }

    /* Stage 1 and stage 2 separately, as the pipeline is meant to run. */
    CK(mx_estimate_buses(buses, cfgs, NROW, levels) == MX_OK);
    for (int i = 0; i < NROW; ++i)
        CK(nearf(levels[i].level, rows[i].level, 1e-6f));
    CK(mx_compress_buses(buses, cfgs, levels, NROW, reps) == MX_OK);

    for (int i = 0; i < NROW; ++i) {
        const Row *r = &rows[i];
        const MxBusReport *g = &reps[i];
        int tag = i;
        CK(g->bus == i);
        CK(g->enabled == r->enabled);
        CK(g->detector == r->detector);
        CK(g->threshold == r->threshold);
        CK(g->ratio == r->ratio);
        if (!nearf(g->level.level, r->level, 1e-6f)) printf("  row %d level\n", tag);
        if (!nearf(g->gain, r->gain, 1e-6f)) printf("  row %d gain %.9g vs %.9g\n", tag, g->gain, r->gain);
        if (!nearf(g->gain_db, r->gain_db, 1e-5f)) printf("  row %d gain_db\n", tag);
        if (!nearf(g->reduction_db, r->reduction_db, 1e-5f)) printf("  row %d reduction\n", tag);
        if (!nearf(g->peak_before, r->peak_before, 1e-6f)) printf("  row %d peak_before\n", tag);
        if (!nearf(g->peak_after, r->peak_after, 1e-6f)) printf("  row %d peak_after\n", tag);
        if (!nearf(g->rms_before, r->rms_before, 1e-6f)) printf("  row %d rms_before\n", tag);
        if (!nearf(g->rms_after, r->rms_after, 1e-6f)) printf("  row %d rms_after\n", tag);
        if (!nearf(g->loudness_before_dbfs, r->loudness_before, 1e-5f)) printf("  row %d loud_before\n", tag);
        if (!nearf(g->loudness_after_dbfs, r->loudness_after, 1e-5f)) printf("  row %d loud_after\n", tag);
        if (!nearf(g->loudness_delta_db, r->loudness_delta, 1e-5f)) printf("  row %d loud_delta\n", tag);
        CK(g->samples_over_threshold == r->over);
        CK(g->compressed_samples == r->compressed);
        CK(g->samples == r->samples);
        CK(nearf(g->level.level, r->level, 1e-6f));
        CK(nearf(g->gain, r->gain, 1e-6f));
        CK(nearf(g->gain_db, r->gain_db, 1e-5f));
        CK(nearf(g->reduction_db, r->reduction_db, 1e-5f));
        CK(nearf(g->peak_before, r->peak_before, 1e-6f));
        CK(nearf(g->peak_after, r->peak_after, 1e-6f));
        CK(nearf(g->rms_before, r->rms_before, 1e-6f));
        CK(nearf(g->rms_after, r->rms_after, 1e-6f));
        CK(nearf(g->loudness_before_dbfs, r->loudness_before, 1e-5f));
        CK(nearf(g->loudness_after_dbfs, r->loudness_after, 1e-5f));
        CK(nearf(g->loudness_delta_db, r->loudness_delta, 1e-5f));
    }
}

/* Each bus must be levelled against its own threshold: two buses that
 * differ only in level get different gains from the same cfg. */
static void test_per_bus_levels(void) {
    float a[8], b[8];
    for (int i = 0; i < 8; ++i) { a[i] = (i & 1) ? -1.0f : 1.0f;
                                  b[i] = (i & 1) ? -0.25f : 0.25f; }
    MxBusView buses[2] = { { a, 8, 1 }, { b, 8, 1 } };
    MxCompCfg cfgs[2] = {
        { MX_DETECT_PEAK, 0.5f, 2.0f, 1 },
        { MX_DETECT_PEAK, 0.5f, 2.0f, 1 },
    };
    MxBusReport reps[2];
    CK(mx_process_buses(buses, cfgs, 2, reps) == MX_OK);
    /* loud bus compressed, quiet one untouched */
    CK(reps[0].gain < 1.0f);
    CK(reps[1].gain == 1.0f);
    CK(nearf(reps[1].peak_after, 0.25f, 1e-6f));
}

/* A bus's channels share one gain so the stereo image does not move. */
static void test_channel_link(void) {
    float buf[4] = { 1.0f, 0.1f, 1.0f, 0.1f };
    MxBusView bus = { buf, 2, 2 };
    MxCompCfg cfg = { MX_DETECT_PEAK, 0.5f, 2.0f, 1 };
    MxBusReport rep;
    CK(mx_compress(&bus, &cfg, NULL, &rep) == MX_OK);
    CK(rep.gain < 1.0f);
    /* both channels scaled by exactly the reported gain */
    CK(buf[0] == 1.0f * rep.gain);
    CK(buf[2] == 1.0f * rep.gain);
    CK(buf[1] == 0.1f * rep.gain);
    CK(buf[3] == 0.1f * rep.gain);
}

/* Degenerate paths must leave the buffer bit-identical. */
static void noop_case(int enabled, float ratio, float threshold, float amp) {
    float buf[64], snap[64];
    for (int i = 0; i < 64; ++i) buf[i] = snap[i] = sinf(0.7f * (float)i) * amp;
    MxBusView bus = { buf, 32, 2 };
    MxCompCfg cfg = { MX_DETECT_PEAK, threshold, ratio, enabled };
    MxBusReport rep;
    CK(mx_compress(&bus, &cfg, NULL, &rep) == MX_OK);
    CK(memcmp(buf, snap, sizeof(buf)) == 0);
    CK(rep.gain == 1.0f);
}

static void test_noop_identical(void) {
    noop_case(0, 2.0f, 0.5f, 2.0f);   /* disabled: level would compress */
    noop_case(1, 1.0f, 0.5f, 2.0f);   /* ratio 1: no curve               */
    noop_case(1, 2.0f, 1.0f, 0.5f);   /* below threshold                 */
}

/* A one-bus batch must be bit-identical to the legacy single-segment
 * call.  The legacy function is the old whole-segment behaviour. */
static void test_legacy_identical(void) {
    float a[64], b[64], snap[64];
    for (int i = 0; i < 64; ++i) {
        a[i] = 0.8f * sinf(0.3f * (float)i) + 0.4f * cosf(0.11f * (float)i);
        snap[i] = a[i];
    }
    memcpy(b, a, sizeof(a));
    MxCompCfg cfg = { MX_DETECT_RMS, 0.35f, 3.0f, 1 };

    MxBusReport legacy_rep, batch_rep;
    CK(mx_compress_legacy(a, 32, 2, &cfg, &legacy_rep) == MX_OK);

    MxBusView buses[1] = { { b, 32, 2 } };
    MxCompCfg cfgs[1] = { cfg };
    CK(mx_process_buses(buses, cfgs, 1, &batch_rep) == MX_OK);

    CK(memcmp(a, b, sizeof(a)) == 0);
    CK(legacy_rep.gain == batch_rep.gain);
    CK(legacy_rep.reduction_db == batch_rep.reduction_db);
    CK(memcmp(snap, b, sizeof(snap)) != 0);   /* it did compress */
}

/* Empty input is rejected with MX_ERR_EMPTY and a reason string. */
static void test_empty_input(void) {
    float x[4] = { 1, 0, 1, 0 };
    MxBusView bus = { x, 0, 1 };
    MxCompCfg cfg = { MX_DETECT_PEAK, 0.5f, 2.0f, 1 };
    MxLevel lvl;
    MxBusReport rep;
    CK(mx_estimate(&bus, MX_DETECT_PEAK, &lvl) == MX_ERR_EMPTY);
    CK(mx_compress(&bus, &cfg, NULL, &rep) == MX_ERR_EMPTY);
    CK(mx_estimate_buses(&bus, &cfg, 0, &lvl) == MX_ERR_EMPTY);
    CK(mx_process_buses(&bus, &cfg, 0, &rep) == MX_ERR_EMPTY);
    CK(mx_compress(NULL, &cfg, NULL, &rep) == MX_ERR_ARG);
    CK(strstr(mx_err_str(MX_ERR_EMPTY), "empty") != NULL);
    CK(strlen(mx_err_str(MX_ERR_EMPTY)) > 10);
    printf("  empty hint: %s\n", mx_err_str(MX_ERR_EMPTY));
}

/* Reject bad parameters and bad stage-1 hand-offs without touching data. */
static void test_validation(void) {
    float x[4] = { 1, 0, 1, 0 }, snap[4];
    memcpy(snap, x, sizeof(x));
    MxBusView bus = { x, 4, 1 };
    MxBusView buses[1] = { { x, 4, 1 } };
    MxCompCfg good = { MX_DETECT_PEAK, 0.5f, 2.0f, 1 };
    MxCompCfg bad;
    MxBusReport rep;
    MxLevel lvl;

    bad = good; bad.threshold = 0.0f;
    CK(mx_compress(&bus, &bad, NULL, &rep) == MX_ERR_PARAM);
    bad = good; bad.threshold = 1.5f;
    CK(mx_compress(&bus, &bad, NULL, &rep) == MX_ERR_PARAM);
    bad = good; bad.ratio = 0.5f;
    CK(mx_compress(&bus, &bad, NULL, &rep) == MX_ERR_PARAM);
    bad = good; bad.detector = (MxDetector)7;
    CK(mx_compress(&bus, &bad, NULL, &rep) == MX_ERR_PARAM);
    bad = good; bad.threshold = NAN;
    CK(mx_compress(&bus, &bad, NULL, &rep) == MX_ERR_PARAM);
    CK(mx_compress_buses(buses, &good, NULL, MX_MAX_BUSES + 1, &rep) == MX_ERR_BUSES);
    CK(memcmp(x, snap, sizeof(x)) == 0);

    /* Stage-1 estimate from a different bus shape is rejected. */
    CK(mx_estimate(&bus, MX_DETECT_PEAK, &lvl) == MX_OK);
    lvl.frames = 99;
    CK(mx_compress(&bus, &good, &lvl, &rep) == MX_ERR_PARAM);
    CK(memcmp(x, snap, sizeof(x)) == 0);

    /* NaN in a bus is refused. */
    x[2] = NAN;
    CK(mx_compress(&bus, &good, NULL, &rep) == MX_ERR_NONFINITE);
    CK(strstr(mx_err_str(MX_ERR_NONFINITE), "NaN") != NULL);
    x[2] = 1.0f;
}

/* A batch is all-or-nothing: one bad bus means no bus is rewritten. */
static void test_batch_atomic(void) {
    float a[4] = { 1, 0, 1, 0 }, b[4] = { 1, 0, 1, 0 };
    float sa[4], sb[4];
    memcpy(sa, a, sizeof(a)); memcpy(sb, b, sizeof(b));
    MxBusView buses[2] = { { a, 4, 1 }, { b, 4, 1 } };
    MxCompCfg cfgs[2] = {
        { MX_DETECT_PEAK, 0.5f, 2.0f, 1 },
        { MX_DETECT_PEAK, 0.0f, 2.0f, 1 },   /* invalid threshold */
    };
    CK(mx_process_buses(buses, cfgs, 2, NULL) == MX_ERR_PARAM);
    CK(memcmp(a, sa, sizeof(a)) == 0);
    CK(memcmp(b, sb, sizeof(b)) == 0);
}

static void test_csv_export(void) {
    float a[8], b[8];
    for (int i = 0; i < 8; ++i) { a[i] = (i & 1) ? -1.0f : 1.0f; b[i] = 0.25f; }
    MxBusView buses[2] = { { a, 8, 1 }, { b, 8, 1 } };
    MxCompCfg cfgs[2] = {
        { MX_DETECT_PEAK, 0.5f, 2.0f, 1 },
        { MX_DETECT_PEAK, 0.5f, 2.0f, 1 },
    };
    MxBusReport reps[2];
    CK(mx_process_buses(buses, cfgs, 2, reps) == MX_OK);
    const char *path = "build/test_buses.csv";
    CK(mx_write_bus_csv(reps, 2, path) == MX_OK);
    FILE *f = fopen(path, "r");
    CK(f != NULL);
    if (f) {
        char line[512];
        int lines = 0;
        int saw_header = 0;
        while (fgets(line, sizeof(line), f)) {
            if (lines == 0) saw_header = strstr(line, "loudness_delta_db") != NULL;
            ++lines;
        }
        fclose(f);
        CK(saw_header);
        CK(lines == 3);   /* header + 2 buses */
    }
    CK(mx_write_bus_csv(NULL, 2, path) == MX_ERR_ARG);
    CK(mx_write_bus_csv(reps, 0, path) == MX_ERR_EMPTY);
    remove(path);
}

int main(void) {
    test_basic_mix();
    test_hand_table();
    test_per_bus_levels();
    test_channel_link();
    test_noop_identical();
    test_legacy_identical();
    test_empty_input();
    test_validation();
    test_batch_atomic();
    test_csv_export();
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
