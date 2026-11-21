#include "mixer.h"
#include "pan.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

#define PI 3.14159265358979323846

static int closef(float a, float b, float tol) { return fabsf(a - b) <= tol; }

static void fill_pattern(float *buf, int n, unsigned seed) {
    unsigned s = seed;
    for (int i = 0; i < n; ++i) {
        s = s * 1664525u + 1013904223u;
        buf[i] = (float)((s >> 8) & 0xffff) / 32768.0f - 1.0f;
    }
}

/* ---- 关闭：逐位不变，且先于任何校验（含非法其余字段/非法总线） ---- */
static void test_disabled_bit_identical(void) {
    MixBus b;
    CK(mx_init(&b, 64, 2) == 0);
    float src[128], before[128];
    fill_pattern(src, 128, 7u);
    CK(mx_add(&b, src, 64, 1.0f) == 64);
    memcpy(before, b.buf, sizeof before);
    MxPanStats st;
    MxPanErr err;
    MxPanCfg off = { 0, MX_PAN_EQUAL_POWER, 0.42f };
    CK(mx_pan_apply(&b, &off, &st, &err) == MX_PAN_OK);
    CK(memcmp(before, b.buf, sizeof before) == 0);
    CK(err.count == 0 && err.msg[0] == '\0');
    CK(st.law == 0 && st.gL == 0.0f && st.frames == 0);

    /* 关闭时其余字段一律忽略：未知律、NaN 位置都不算冲突 */
    MxPanCfg off_junk = { 0, 99, NAN };
    CK(mx_pan_validate(&off_junk, &b, &err) == 0);
    CK(mx_pan_apply(&b, &off_junk, NULL, &err) == MX_PAN_OK);
    CK(memcmp(before, b.buf, sizeof before) == 0);

    /* 关闭时非法总线也旁路：3 声道、frames > cap 都不检查 */
    MixBus odd;
    CK(mx_init(&odd, 8, 3) == 0);
    CK(mx_add(&odd, src, 2, 1.0f) == 2);
    float odd_before[24];
    memcpy(odd_before, odd.buf, sizeof odd_before);
    MxPanCfg off2 = { 0, MX_PAN_LINEAR, 1.0f };
    CK(mx_pan_apply(&odd, &off2, NULL, NULL) == MX_PAN_OK);
    CK(memcmp(odd_before, odd.buf, sizeof odd_before) == 0);

    /* NULL 配置仍然报 NULL（不静默成功），但不写缓冲 */
    CK(mx_pan_apply(&b, NULL, NULL, &err) == MX_PAN_ERR_NULL);
    CK(err.count == 1 && strstr(err.msg, "cfg is NULL") != NULL);
    CK(memcmp(before, b.buf, sizeof before) == 0);

    mx_free(&odd);
    mx_free(&b);
}

/* ---- 第一段：两种律的增益与解析解逐点对照 ---- */
static void test_gains_analytic(void) {
    const float pos[] = { -1.0f, -0.75f, -0.5f, -0.25f, 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };
    const int n = (int)(sizeof(pos) / sizeof(pos[0]));
    float prevL = 2.0f, prevR = -1.0f;
    for (int i = 0; i < n; ++i) {
        float p = pos[i], gl, gr;

        CK(mx_pan_gains(MX_PAN_EQUAL_POWER, p, &gl, &gr) == MX_PAN_OK);
        double th = (double)(p + 1.0f) * (PI / 4.0);
        CK(closef(gl, (float)cos(th), 1e-6f));
        CK(closef(gr, (float)sin(th), 1e-6f));
        CK(closef(gl * gl + gr * gr, 1.0f, 1e-6f));           /* 恒功率 */
        CK(gl >= 0.0f && gr >= 0.0f);
        CK(gl <= prevL + 1e-6f && gr >= prevR - 1e-6f);        /* 单调 */
        prevL = gl; prevR = gr;

        CK(mx_pan_gains(MX_PAN_LINEAR, p, &gl, &gr) == MX_PAN_OK);
        CK(closef(gl, 0.5f * (1.0f - p), 1e-6f));
        CK(closef(gr, 0.5f * (1.0f + p), 1e-6f));
        CK(closef(gl + gr, 1.0f, 1e-6f));                      /* 恒和 */
    }
    /* 端点与中点：两律在端点一致，在中点等功率高 3.01 dB */
    float el, er, ll, lr;
    CK(mx_pan_gains(MX_PAN_EQUAL_POWER, -1.0f, &el, &er) == MX_PAN_OK);
    CK(mx_pan_gains(MX_PAN_LINEAR,      -1.0f, &ll, &lr) == MX_PAN_OK);
    CK(closef(el, 1.0f, 1e-7f) && closef(er, 0.0f, 1e-7f));
    CK(closef(el, ll, 1e-7f) && closef(er, lr, 1e-7f));
    CK(mx_pan_gains(MX_PAN_EQUAL_POWER, 1.0f, &el, &er) == MX_PAN_OK);
    CK(mx_pan_gains(MX_PAN_LINEAR,      1.0f, &ll, &lr) == MX_PAN_OK);
    CK(closef(el, 0.0f, 1e-7f) && closef(er, 1.0f, 1e-7f));
    CK(closef(el, ll, 1e-7f) && closef(er, lr, 1e-7f));
    CK(mx_pan_gains(MX_PAN_EQUAL_POWER, 0.0f, &el, &er) == MX_PAN_OK);
    CK(mx_pan_gains(MX_PAN_LINEAR,      0.0f, &ll, &lr) == MX_PAN_OK);
    CK(closef(el, 0.70710677f, 1e-6f) && closef(er, el, 1e-6f));
    CK(closef(ll, 0.5f, 1e-7f) && closef(lr, ll, 1e-7f));
    CK(closef(el * el + er * er, 1.0f, 1e-6f));
    CK(closef(ll * ll + lr * lr, 0.5f, 1e-7f));                /* 中间发空的根因 */
    CK(closef(20.0f * log10f(el / ll), 3.0103f, 1e-3f));

    /* 非法输入 */
    float g1 = 9.0f, g2 = 9.0f;
    CK(mx_pan_gains(MX_PAN_EQUAL_POWER, 1.0001f, &g1, &g2) == MX_PAN_ERR_INVALID);
    CK(mx_pan_gains(MX_PAN_EQUAL_POWER, -1.0001f, &g1, &g2) == MX_PAN_ERR_INVALID);
    CK(mx_pan_gains(MX_PAN_EQUAL_POWER, NAN, &g1, &g2) == MX_PAN_ERR_INVALID);
    CK(mx_pan_gains(MX_PAN_EQUAL_POWER, INFINITY, &g1, &g2) == MX_PAN_ERR_INVALID);
    CK(mx_pan_gains(MX_PAN_LINEAR, 0.0f, NULL, &g2) == MX_PAN_ERR_NULL);
    CK(mx_pan_gains((MxPanLaw)7, 0.0f, &g1, &g2) == MX_PAN_ERR_INVALID);
    CK(g1 == 9.0f && g2 == 9.0f);                              /* 失败时不写输出 */
}

/* ---- 第一段的逆运算：位置可反解，逐点对照 ---- */
static void test_position_inverse(void) {
    for (int i = -10; i <= 10; ++i) {
        float p = (float)i / 10.0f, gl, gr, back, back_lin;
        CK(mx_pan_gains(MX_PAN_EQUAL_POWER, p, &gl, &gr) == MX_PAN_OK);
        CK(mx_pan_position(MX_PAN_EQUAL_POWER, gl, gr, &back) == MX_PAN_OK);
        CK(closef(back, p, 1e-6f));
        CK(mx_pan_gains(MX_PAN_LINEAR, p, &gl, &gr) == MX_PAN_OK);
        CK(mx_pan_position(MX_PAN_LINEAR, gl, gr, &back_lin) == MX_PAN_OK);
        CK(closef(back_lin, p, 1e-6f));
        /* 两律反解的是同一组位置；把等功率增益按线性反解才出现偏差 */
        float epl, epr, mixed = 0.0f;
        CK(mx_pan_gains(MX_PAN_EQUAL_POWER, p, &epl, &epr) == MX_PAN_OK);
        CK(mx_pan_position(MX_PAN_LINEAR, epl, epr, &mixed) == MX_PAN_OK);
        if (p == 0.0f) CK(closef(mixed, 0.0f, 1e-6f));
        /* 端点两律一致；中间区域反解偏差可测，即“两律差异” */
        if (fabsf(p) > 1e-6f && fabsf(p) < 0.999f) CK(fabsf(mixed - p) > 1e-4f);
    }
    float p = 0.0f;
    CK(mx_pan_position(MX_PAN_EQUAL_POWER, 0.0f, 0.0f, &p) == MX_PAN_ERR_INVALID);
    CK(mx_pan_position(MX_PAN_EQUAL_POWER, -0.1f, 0.5f, &p) == MX_PAN_ERR_INVALID);
    CK(mx_pan_position(MX_PAN_EQUAL_POWER, NAN, 0.5f, &p) == MX_PAN_ERR_INVALID);
    CK(mx_pan_position((MxPanLaw)7, 0.5f, 0.5f, &p) == MX_PAN_ERR_INVALID);
    CK(mx_pan_position(MX_PAN_LINEAR, 0.5f, 0.5f, NULL) == MX_PAN_ERR_NULL);
}

/* ---- 第二段：按增益分配到每个 L/R 对（2/4/8 声道） ---- */
static void test_spread_layouts(void) {
    const int chans[] = { 2, 4, 8 };
    const float acL = 0.3f, acR = 0.9f;
    const int frames = 16;
    for (int ci = 0; ci < 3; ++ci) {
        int ch = chans[ci];
        MixBus b;
        CK(mx_init(&b, frames, ch) == 0);
        float *src = (float*)malloc(sizeof(float) * (size_t)frames * (size_t)ch);
        fill_pattern(src, frames * ch, 11u + (unsigned)ch);
        CK(mx_add(&b, src, frames, 1.0f) == frames);
        float *orig = (float*)malloc(sizeof(float) * (size_t)frames * (size_t)ch);
        memcpy(orig, b.buf, sizeof(float) * (size_t)frames * (size_t)ch);
        MxPanStats st;
        CK(mx_pan_spread(&b, acL, acR, &st) == MX_PAN_OK);
        CK(st.channels == ch && st.pairs == ch / 2);
        CK(st.frames == frames && st.samples == (long long)frames * ch);
        CK(closef(st.gL, acL, 0.0f) && closef(st.gR, acR, 0.0f));
        CK(closef(st.energy, acL * acL + acR * acR, 1e-7f));
        CK(st.law == -1 && isnan(st.position) && isnan(st.pos_est));
        for (int i = 0; i < frames; ++i) {
            for (int k = 0; k < ch / 2; ++k) {
                size_t il = (size_t)i * ch + 2 * k;
                CK(closef(b.buf[il],     orig[il]     * acL, 1e-7f));
                CK(closef(b.buf[il + 1], orig[il + 1] * acR, 1e-7f));
            }
        }
        CK(b.frames == frames);
        free(orig);
        free(src);
        mx_free(&b);
    }

    /* 非法布局：1/3/10 声道、frames>cap、空缓冲；一律不写 */
    const int badch[] = { 1, 3, 10 };
    for (int i = 0; i < 3; ++i) {
        MixBus b;
        CK(mx_init(&b, 4, badch[i]) == 0);
        float z[40] = {0};
        CK(mx_add(&b, z, 2, 1.0f) == 2);
        float before[40];
        size_t bytes = (size_t)b.cap * (size_t)b.channels * sizeof(float);
        memcpy(before, b.buf, bytes);
        CK(mx_pan_spread(&b, 0.5f, 0.5f, NULL) == MX_PAN_ERR_INVALID);
        CK(memcmp(before, b.buf, bytes) == 0);
        mx_free(&b);
    }
    MixBus o;
    CK(mx_init(&o, 4, 2) == 0);
    float *o_buf = o.buf;
    o.frames = 5;                                              /* > cap */
    CK(mx_pan_spread(&o, 0.5f, 0.5f, NULL) == MX_PAN_ERR_INVALID);
    o.frames = 0;
    o.buf = NULL;
    CK(mx_pan_spread(&o, 0.5f, 0.5f, NULL) == MX_PAN_ERR_INVALID);
    free(o_buf);
    mx_free(&o);
    CK(mx_pan_spread(NULL, 0.5f, 0.5f, NULL) == MX_PAN_ERR_NULL);

    /* 非法增益 */
    MixBus g;
    CK(mx_init(&g, 4, 2) == 0);
    float z2[8] = {0};
    CK(mx_add(&g, z2, 4, 1.0f) == 4);
    CK(mx_pan_spread(&g, 1.01f, 0.5f, NULL) == MX_PAN_ERR_INVALID);
    CK(mx_pan_spread(&g, 0.5f, -0.01f, NULL) == MX_PAN_ERR_INVALID);
    CK(mx_pan_spread(&g, NAN, 0.5f, NULL) == MX_PAN_ERR_INVALID);
    mx_free(&g);
}

/* ---- 两段合并：逐样本对照解析解，2 与 8 声道各一遍 ---- */
static void test_apply_analytic(void) {
    const int chans[] = { 2, 8 };
    const float pos[] = { -1.0f, -0.5f, 0.0f, 0.5f, 1.0f };
    const int frames = 64;
    for (int ci = 0; ci < 2; ++ci) {
        int ch = chans[ci];
        for (int pi = 0; pi < 5; ++pi) {
            MixBus b;
            CK(mx_init(&b, frames, ch) == 0);
            float *src = (float*)malloc(sizeof(float) * (size_t)frames * (size_t)ch);
            fill_pattern(src, frames * ch, 23u + (unsigned)pi * 7u + (unsigned)ch);
            CK(mx_add(&b, src, frames, 1.0f) == frames);

            MxPanCfg cfg = { 1, MX_PAN_EQUAL_POWER, pos[pi] };
            MxPanStats st;
            MxPanErr err;
            CK(mx_pan_apply(&b, &cfg, &st, &err) == MX_PAN_OK);
            CK(err.count == 0);
            double th = (double)(pos[pi] + 1.0f) * (PI / 4.0);
            float gL = (float)cos(th), gR = (float)sin(th);
            CK(closef(st.gL, gL, 1e-6f) && closef(st.gR, gR, 1e-6f));
            CK(closef(st.energy, 1.0f, 1e-6f));
            CK(closef(st.pos_est, pos[pi], 1e-5f));
            CK(st.law == MX_PAN_EQUAL_POWER && st.channels == ch && st.pairs == ch / 2);
            for (int i = 0; i < frames; ++i) {
                for (int c = 0; c < ch; ++c) {
                    size_t idx = (size_t)i * ch + c;
                    float g = (c % 2 == 0) ? gL : gR;
                    CK(closef(b.buf[idx], src[idx] * g, 2e-7f));
                }
            }
            free(src);
            mx_free(&b);
        }
    }
}

/* ---- 非法组合一次列出全部冲突项，且被拒绝时不写缓冲 ---- */
static void test_reject_lists_all(void) {
    MixBus b;
    CK(mx_init(&b, 8, 4) == 0);
    float src[32], before[32];
    fill_pattern(src, 32, 5u);
    CK(mx_add(&b, src, 8, 1.0f) == 8);
    memcpy(before, b.buf, sizeof before);

    MxPanCfg bad = { 2, (MxPanLaw)9, 2.5f };                   /* enabled/law/position 三项 */
    MxPanErr err;
    CK(mx_pan_apply(&b, &bad, NULL, &err) == MX_PAN_ERR_INVALID);
    CK(err.count == 3);
    CK(strstr(err.item[0], "enabled=2") != NULL);
    CK(strstr(err.item[1], "law=9") != NULL);
    CK(strstr(err.item[2], "position=2.5") != NULL);
    CK(strstr(err.msg, "enabled=2") && strstr(err.msg, "law=9") &&
       strstr(err.msg, "position=2.5") && strstr(err.msg, "; ") != NULL);
    CK(memcmp(before, b.buf, sizeof before) == 0);

    /* 总线问题与配置问题同时命中：共 6 项，逐项可读 */
    MixBus odd;
    CK(mx_init(&odd, 8, 3) == 0);
    CK(mx_add(&odd, src, 2, 1.0f) == 2);
    odd.frames = odd.cap + 1;                                  /* frames > cap */
    float odd_buf[24];
    memcpy(odd_buf, odd.buf, sizeof odd_buf);
    MxPanCfg bad2 = { 3, (MxPanLaw)4, NAN };
    CK(mx_pan_apply(&odd, &bad2, NULL, &err) == MX_PAN_ERR_INVALID);
    CK(err.count == 5);                                        /* enabled/law/position/channels/frames */
    CK(strstr(err.msg, "enabled=3") && strstr(err.msg, "law=4") &&
       strstr(err.msg, "not finite") && strstr(err.msg, "channels=3") &&
       strstr(err.msg, "frames=9 > cap=8"));
    CK(memcmp(odd_buf, odd.buf, sizeof odd_buf) == 0);         /* 被拒绝时零写入 */

    /* 空缓冲单独命中 */
    MixBus nobuf = { NULL, 0, 2, 8 };
    CK(mx_pan_apply(&nobuf, &(MxPanCfg){1, MX_PAN_LINEAR, 0.0f}, NULL, &err) ==
       MX_PAN_ERR_INVALID);
    CK(err.count == 1 && strstr(err.msg, "buffer is NULL") != NULL);

    mx_free(&odd);
    mx_free(&b);
}

/* ---- 关闭状态与“未接入声像”的旧路径逐位一致（含 8 声道与流式两次 add） ---- */
static void test_legacy_flow_identical(void) {
    MixBus b, ref;
    CK(mx_init(&b, 32, 8) == 0);
    CK(mx_init(&ref, 32, 8) == 0);
    float a[128], c[128];
    fill_pattern(a, 128, 31u);
    fill_pattern(c, 128, 32u);
    CK(mx_add(&b, a, 16, 1.0f) == 16);
    CK(mx_add(&b, c, 16, 0.5f) == 16);
    memcpy(ref.buf, b.buf, sizeof(float) * 32 * 8);
    ref.frames = b.frames;
    MxPanCfg off = { 0, MX_PAN_EQUAL_POWER, 0.0f };
    CK(mx_pan_apply(&b, &off, NULL, NULL) == MX_PAN_OK);
    CK(memcmp(ref.buf, b.buf, sizeof(float) * 32 * 8) == 0);
    CK(b.frames == ref.frames);
    mx_free(&ref);
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
    test_gains_analytic();
    test_position_inverse();
    test_spread_layouts();
    test_apply_analytic();
    test_reject_lists_all();
    test_legacy_flow_identical();

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
