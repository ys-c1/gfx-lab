#include "mixer.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

static int peak_pos(const float *x, int frames, int channels, int c) {
    int best = -1;
    float bestv = 0.0f;
    for (int i = 0; i < frames; ++i) {
        float v = fabsf(x[(size_t)i * channels + c]);
        if (v > bestv) { bestv = v; best = i; }
    }
    return best;
}

int main(void) {
    /* ---- existing bus behaviour is untouched ---- */
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

    /* ---- 2 channels: an event at frame 2 arrives at 2 and 5 ---- */
    CK(mx_init(&b, 64, 2) == 0);
    float src2[16];
    memset(src2, 0, sizeof(src2));
    src2[2 * 2 + 0] = 1.0f;   /* ch0: true frame 2 + delay 0 */
    src2[5 * 2 + 1] = 1.0f;   /* ch1: true frame 2 + delay 3 */
    int delays2[2] = {0, 3};
    float dst2[22];
    MxAlignTable t2;
    memset(&t2, 0, sizeof(t2));
    CK(mx_align(&b, src2, 8, delays2, dst2, &t2) == 11);
    CK(t2.channels == 2 && t2.max_delay == 3 && t2.compensated == 3);
    CK(t2.delay[0] == 0 && t2.delay[1] == 3);
    CK(t2.comp[0] == 3 && t2.comp[1] == 0);
    CK(peak_pos(dst2, 11, 2, 0) == 5);
    CK(peak_pos(dst2, 11, 2, 1) == 5);   /* both events now on frame 5 */
    CK(fabsf(dst2[0 * 2 + 0]) < 1e-6f);  /* the padding is real silence */
    CK(fabsf(dst2[10 * 2 + 1]) < 1e-6f);
    mx_free(&b);

    /* ---- zero delays: output is a byte-for-byte copy, not a shifted one ---- */
    CK(mx_init(&b, 64, 2) == 0);
    int zero2[2] = {0, 0};
    float same2[16];
    MxAlignTable tz;
    memset(&tz, 0, sizeof(tz));
    CK(mx_align(&b, src2, 8, zero2, same2, &tz) == 8);
    CK(memcmp(same2, src2, sizeof(src2)) == 0);
    CK(tz.max_delay == 0 && tz.compensated == 0 && tz.comp[0] == 0 && tz.comp[1] == 0);
    mx_free(&b);

    /* the zero-delay path must match plain mx_add exactly */
    MixBus plain, delayed;
    CK(mx_init(&plain, 64, 2) == 0);
    CK(mx_init(&delayed, 64, 2) == 0);
    CK(mx_add(&plain, src2, 8, 1.0f) == 8);
    CK(mx_add_delayed(&delayed, src2, 8, zero2, 1.0f, &tz) == 8);
    CK(plain.frames == delayed.frames);
    CK(memcmp(plain.buf, delayed.buf, sizeof(float) * 16) == 0);
    mx_free(&plain);
    mx_free(&delayed);

    /* ---- 6 channels: max delay 5, compensations 2 5 0 4 1 3 ---- */
    MixBus b6;
    CK(mx_init(&b6, 128, 6) == 0);
    int delays6[6] = {3, 0, 5, 1, 4, 2};
    int hand6[6] = {2, 5, 0, 4, 1, 3};
    float src6[6 * 10], dst6[6 * 15];
    memset(src6, 0, sizeof(src6));
    for (int ch = 0; ch < 6; ++ch) src6[(size_t)(4 + delays6[ch]) * 6 + ch] = 1.0f;
    MxAlignTable t6;
    memset(&t6, 0, sizeof(t6));
    CK(mx_align(&b6, src6, 10, delays6, dst6, &t6) == 15);
    CK(t6.max_delay == 5 && t6.compensated == 15);
    for (int ch = 0; ch < 6; ++ch) {
        CK(t6.comp[ch] == hand6[ch]);
        CK(peak_pos(dst6, 15, 6, ch) == 9);   /* true frame 4 + max delay 5 */
    }
    mx_free(&b6);

    /* ---- two-stage mix: the whole block lands in the bus aligned ---- */
    MixBus bus;
    CK(mx_init(&bus, 64, 2) == 0);
    MxAlignTable tb;
    memset(&tb, 0, sizeof(tb));
    CK(mx_add_delayed(&bus, src2, 8, delays2, 1.0f, &tb) == 11);
    CK(bus.frames == 11);
    CK(tb.channels == 2 && tb.compensated == 3 && tb.max_delay == 3);
    CK(peak_pos(bus.buf, 11, 2, 0) == 5);
    CK(peak_pos(bus.buf, 11, 2, 1) == 5);
    mx_free(&bus);

    /* ---- errors carry the offending field name ---- */
    MixBus bad;
    CK(mx_init(&bad, 8, 0) == MX_ERR_CHANNELS);
    CK(strcmp(mx_error_field(MX_ERR_CHANNELS), "channels") == 0);
    CK(mx_init(&bad, 8, -2) == MX_ERR_CHANNELS);
    CK(mx_init(&bad, 8, 2) == 0);

    int neg2[2] = {-1, 0};
    MxAlignTable te;
    memset(&te, 0, sizeof(te));
    float scratch[32];
    CK(mx_align(&bad, src2, 8, neg2, scratch, &te) == MX_ERR_DELAY);
    CK(strcmp(mx_error_field(MX_ERR_DELAY), "delay") == 0);
    CK(mx_add_delayed(&bad, src2, 8, neg2, 1.0f, &te) == MX_ERR_DELAY);
    CK(te.channels == 0 && te.max_delay == 0);   /* no half-filled table */

    int toolate2[2] = {MX_MAX_DELAY + 1, 0};
    CK(mx_align(&bad, src2, 8, toolate2, scratch, &te) == MX_ERR_DELAY);

    /* delay fits MX_MAX_DELAY but the delayed block no longer fits the bus:
     * rc names the delay field, because the delay is what overflowed it */
    int overrun2[2] = {0, 1};
    CK(mx_add_delayed(&bad, src2, 8, overrun2, 1.0f, &te) == MX_ERR_DELAY);
    CK(strcmp(mx_error_field(MX_ERR_DELAY), "delay") == 0);

    /* capacity is its own field */
    float scratch9[40];
    memset(scratch9, 0, sizeof(scratch9));
    CK(mx_add_aligned(&bad, scratch9, 20, 1.0f) == MX_ERR_CAPACITY);
    CK(strcmp(mx_error_field(MX_ERR_CAPACITY), "capacity") == 0);
    mx_free(&bad);
    CK(mx_init(&bad, 0, 2) == MX_ERR_CAPACITY);
    CK(mx_init(&bad, -4, 2) == MX_ERR_CAPACITY);

    /* zero channels on an otherwise valid-looking bus */
    float dummy = 0.0f;
    MixBus noch;
    memset(&noch, 0, sizeof(noch));
    noch.buf = &dummy;
    noch.channels = 0;
    noch.cap = 8;
    CK(mx_align(&noch, src2, 8, zero2, scratch, &te) == MX_ERR_CHANNELS);
    CK(mx_add_delayed(&noch, src2, 8, zero2, 1.0f, &te) == MX_ERR_CHANNELS);
    CK(mx_add(&noch, src2, 8, 1.0f) == MX_ERR_CHANNELS);

    /* a table is capped at MX_MAX_CHANNELS; without one, wider buses align */
    MixBus wide;
    CK(mx_init(&wide, 8, MX_MAX_CHANNELS + 1) == 0);
    float wide_dst[2 * (MX_MAX_CHANNELS + 1)];
    int wd[MX_MAX_CHANNELS + 1];
    for (int i = 0; i <= MX_MAX_CHANNELS; ++i) wd[i] = i % 2;
    CK(mx_align(&wide, wide.buf, 1, wd, wide_dst, &te) == MX_ERR_CHANNELS);
    CK(mx_align(&wide, wide.buf, 1, wd, wide_dst, NULL) == 2);
    mx_free(&wide);

    /* the top end of the delay range is accepted */
    int top2[2] = {MX_MAX_DELAY, 0};
    CK(mx_init(&bad, 8 + MX_MAX_DELAY, 2) == 0);
    CK(mx_add_delayed(&bad, src2, 8, top2, 1.0f, &te) == 8 + MX_MAX_DELAY);
    CK(te.comp[0] == 0 && te.comp[1] == MX_MAX_DELAY && te.compensated == MX_MAX_DELAY);
    mx_free(&bad);

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
