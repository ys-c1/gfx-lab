#include "mixer.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

static void test_disabled_bit_identical(void) {
    MixBus b;
    CK(mx_init(&b, 16, 2) == 0);
    float src[32], before[32];
    for (int i = 0; i < 32; ++i) src[i] = 0.001f * (float)(i * 7 - 50);
    CK(mx_add(&b, src, 16, 1.0f) == 16);
    memcpy(before, b.buf, sizeof before);
    MxCrossfeedCfg off = { 0, 0.0f, 0.0f, 0 };      /* 未开启 */
    MxCrossfeedCfg off_junk = { 0, 0.9f, 12.0f, 0 }; /* 未开启时其余字段一律忽略 */
    CK(mx_crossfeed_apply(&b, &off, NULL, NULL, 0) == 0);
    CK(mx_crossfeed_apply(&b, &off_junk, NULL, NULL, 0) == 0);
    CK(memcmp(before, b.buf, sizeof before) == 0);   /* 图和现在一模一样 */
    mx_free(&b);
}

static void test_invalid_combos(void) {
    MixBus b;
    char err[512];
    float src[8] = {0};
    CK(mx_init(&b, 4, 2) == 0);
    mx_add(&b, src, 4, 1.0f);
    MxCrossfeedCfg bad = { 1, 0.0f, -1.0f, 44100 }; /* feed=0 且 delay<0：两个冲突 */
    CK(mx_crossfeed_apply(&b, &bad, NULL, err, sizeof err) == -1);
    CK(strstr(err, "feed=0") != NULL);
    CK(strstr(err, "delay_ms=-1") != NULL);          /* 冲突项逐项列出 */
    MxCrossfeedCfg bad2 = { 1, 1.5f, 40.0f, 1000 }; /* feed>1、delay>30、rate 非法 */
    CK(mx_crossfeed_apply(&b, &bad2, NULL, err, sizeof err) == -1);
    CK(strstr(err, "feed=1.5") && strstr(err, "delay_ms=40") && strstr(err, "sample_rate=1000"));
    mx_free(&b);
    CK(mx_init(&b, 4, 3) == 0);                      /* 奇数声道：无 L/R 对 */
    mx_add(&b, src, 1, 1.0f);
    MxCrossfeedCfg odd = { 1, 0.3f, 0.5f, 48000 };
    CK(mx_crossfeed_apply(&b, &odd, NULL, err, sizeof err) == -1);
    CK(strstr(err, "channels=3") != NULL);
    mx_free(&b);
}

static void test_impulse_analytic(void) {
    /* 解析解：L 冲激 => out_L[0]=1，out_R[d]=feed，其余为 0 */
    const int rate = 48000, frames = 128;
    const float feed = 0.3f;
    MixBus b;
    CK(mx_init(&b, frames, 2) == 0);
    float src[256] = {0};
    src[0] = 1.0f;
    mx_add(&b, src, frames, 1.0f);
    MxCrossfeedCfg cfg = { 1, feed, 1.0f, rate };   /* 1 ms => 48 采样点 */
    MxCrossfeedStats st;
    CK(mx_crossfeed_apply(&b, &cfg, &st, NULL, 0) == 0);
    CK(st.delay_samples == 48);
    CK(fabsf(b.buf[0] - 1.0f) < 1e-6f);
    CK(fabsf(b.buf[2 * 48 + 1] - feed) < 1e-6f);    /* 对侧在 d 处出现馈送 */
    CK(fabsf(st.peak_before - 1.0f) < 1e-6f);
    CK(fabsf(st.peak_after - 1.0f) < 1e-6f);
    CK(!st.clipped);
    for (int i = 0; i < frames; ++i) {               /* 逐项对照解析解 */
        float eL = (i == 0) ? 1.0f : 0.0f;
        float eR = (i == 48) ? feed : 0.0f;
        CK(fabsf(b.buf[2 * i] - eL) < 1e-6f);
        CK(fabsf(b.buf[2 * i + 1] - eR) < 1e-6f);
    }
    mx_free(&b);
}

static void test_sine_crosstalk_analytic(void) {
    /* 解析解：L=正弦、R=0 => out_R[i]=feed*x[i-d]，串音比=feed */
    const int rate = 48000, frames = 4800, d = 24;
    const float feed = 0.25f, amp = 0.8f;
    MixBus b;
    CK(mx_init(&b, frames, 2) == 0);
    float src[9600];
    for (int i = 0; i < frames; ++i) {
        src[2 * i] = amp * sinf(2.0f * 3.14159265f * 1000.0f * (float)i / (float)rate);
        src[2 * i + 1] = 0.0f;
    }
    mx_add(&b, src, frames, 1.0f);
    MxCrossfeedCfg cfg = { 1, feed, 0.5f, rate };   /* 0.5 ms => 24 采样点 */
    MxCrossfeedStats st;
    CK(mx_crossfeed_apply(&b, &cfg, &st, NULL, 0) == 0);
    CK(st.delay_samples == d);
    double sumL = 0, sumR = 0;
    for (int i = 2 * d; i < frames; ++i) {           /* 稳态区 */
        float x = amp * sinf(2.0f * 3.14159265f * 1000.0f * (float)i / (float)rate);
        float xd = amp * sinf(2.0f * 3.14159265f * 1000.0f * (float)(i - d) / (float)rate);
        CK(fabsf(b.buf[2 * i] - x) < 1e-5f);         /* 逐项对照解析解 */
        CK(fabsf(b.buf[2 * i + 1] - feed * xd) < 1e-5f);
        sumL += (double)b.buf[2 * i] * b.buf[2 * i];
        sumR += (double)b.buf[2 * i + 1] * b.buf[2 * i + 1];
    }
    float xtalk = (float)sqrt(sumR / sumL);          /* 串音指标 */
    CK(fabsf(xtalk - feed) < 1e-3f);
    CK(fabsf(st.peak_after - 0.8f) < 1e-4f);         /* 1000Hz/48k 正弦采样必过峰值 */
    mx_free(&b);
}

int main(void) {
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
    test_disabled_bit_identical();
    test_invalid_combos();
    test_impulse_analytic();
    test_sine_crosstalk_analytic();
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
