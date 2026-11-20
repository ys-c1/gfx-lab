#include "mixer.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

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

/* disabled limiter must leave every sample bit-identical */
static void test_bypass_identical(void) {
    MixBus b;
    CK(mx_init(&b, 64, 2) == 0);
    for (int i = 0; i < 64 * 2; ++i) b.buf[i] = sinf(i * 0.7f) * 3.0f;
    b.frames = 64;
    float snap[128];
    memcpy(snap, b.buf, sizeof(snap));
    MxLimitCfg cfg = { 1.0f, 48000, 1.0f, 20.0f, 0 };
    MxLimitReport rep = { 123, 456, 7, 8, 9 };
    CK(mx_limit(&b, &cfg, &rep) == MX_OK);
    CK(memcmp(snap, b.buf, sizeof(snap)) == 0);
    CK(rep.peak_before == 123 && rep.plateau_runs == 7); /* report untouched */
    mx_free(&b);
}

static void test_guard_and_no_plateau(void) {
    /* 24 stereo frames with peaks up to 4x full scale */
    MixBus b;
    CK(mx_init(&b, 24, 2) == 0);
    b.frames = 24;
    for (int i = 0; i < 24; ++i) {
        float s = sinf(i * 0.9f) * 4.0f;
        b.buf[i * 2] = s;
        b.buf[i * 2 + 1] = -s * 0.5f;
    }
    MxLimitCfg cfg = { 0.9f, 48000, 1.0f, 20.0f, 1 };
    MxLimitReport rep = {0};
    CK(mx_limit(&b, &cfg, &rep) == MX_OK);
    CK(rep.peak_before > 3.9f);
    CK(rep.peak_after <= cfg.threshold);
    CK(rep.plateau_runs == 0 && rep.plateau_samples == 0);
    CK(rep.limited_samples > 0);
    /* no two consecutive samples may share the boundary value on either channel */
    for (int c = 0; c < 2; ++c)
        for (int i = 1; i < 24; ++i)
            CK(b.buf[i * 2 + c] != b.buf[(i - 1) * 2 + c] ||
               fabsf(b.buf[i * 2 + c]) < cfg.threshold - 1e-6f);
    /* every sample strictly inside the ceiling */
    for (int i = 0; i < 48; ++i) CK(fabsf(b.buf[i]) <= cfg.threshold);
    mx_free(&b);
}

static void test_quiet_signal_passthrough(void) {
    MixBus b;
    CK(mx_init(&b, 16, 1) == 0);
    b.frames = 16;
    for (int i = 0; i < 16; ++i) b.buf[i] = 0.1f * sinf(i);
    MxLimitCfg cfg = { 1.0f, 48000, 1.0f, 20.0f, 1 };
    MxLimitReport rep = {0};
    CK(mx_limit(&b, &cfg, &rep) == MX_OK);
    CK(rep.limited_samples == 0);
    CK(fabsf(rep.peak_after - rep.peak_before) < 1e-6f);
    for (int i = 0; i < 16; ++i) CK(fabsf(b.buf[i] - 0.1f * sinf(i)) < 1e-6f);
    mx_free(&b);
}

static void test_param_rejection(void) {
    MixBus b;
    CK(mx_init(&b, 16, 2) == 0);
    b.frames = 4;
    MxLimitReport rep = {0};
    MxLimitCfg bad;
    bad = (MxLimitCfg){ 0.0f, 48000, 1.0f, 20.0f, 1 };
    CK(mx_limit(&b, &bad, &rep) == MX_ERR_PARAM);          /* threshold 0 */
    bad = (MxLimitCfg){ 1.01f, 48000, 1.0f, 20.0f, 1 };
    CK(mx_limit(&b, &bad, &rep) == MX_ERR_PARAM);          /* threshold > 1 */
    bad = (MxLimitCfg){ -0.5f, 48000, 1.0f, 20.0f, 1 };
    CK(mx_limit(&b, &bad, &rep) == MX_ERR_PARAM);          /* negative threshold */
    bad = (MxLimitCfg){ 1.0f, 0, 1.0f, 20.0f, 1 };
    CK(mx_limit(&b, &bad, &rep) == MX_ERR_PARAM);          /* sample rate */
    bad = (MxLimitCfg){ 1.0f, 48000, 0.0f, 20.0f, 1 };
    CK(mx_limit(&b, &bad, &rep) == MX_ERR_PARAM);          /* attack */
    bad = (MxLimitCfg){ 1.0f, 48000, 1.0f, -1.0f, 1 };
    CK(mx_limit(&b, &bad, &rep) == MX_ERR_PARAM);          /* release */
    CK(mx_limit(NULL, &bad, &rep) == MX_ERR_NULL);
    CK(mx_limit(&b, NULL, &rep) == MX_ERR_NULL);
    printf("  param hint: %s\n", mx_err_str(MX_ERR_PARAM));
    mx_free(&b);
}

static void test_channel_mismatch(void) {
    MixBus b;
    CK(mx_init(&b, 16, 2) == 0);
    float mono[4] = { 0.1f, 0.2f, 0.3f, 0.4f };
    int rc = mx_add_ex(&b, mono, 4, 1, 1.0f);
    CK(rc == MX_ERR_CHANNELS);
    printf("  channels hint: %s\n", mx_err_str(rc));
    CK(mx_add_ex(&b, mono, 0, 1, 1.0f) == MX_ERR_PARAM);
    CK(mx_add_ex(NULL, mono, 4, 2, 1.0f) == MX_ERR_NULL);
    /* matching channels works */
    float st[8] = { 0.1f, 0.1f, 0.2f, 0.2f, 0.3f, 0.3f, 0.4f, 0.4f };
    CK(mx_add_ex(&b, st, 4, 2, 1.0f) == 4);
    CK(fabsf(b.buf[2] - 0.2f) < 1e-6f);
    mx_free(&b);
}

int main(void) {
    test_basic_mix();
    test_bypass_identical();
    test_guard_and_no_plateau();
    test_quiet_signal_passthrough();
    test_param_rejection();
    test_channel_mismatch();
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
