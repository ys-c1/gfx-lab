#include "meter.h"
#include "mixer.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

static void test_mixer_bus(void) {
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

/* 回落时间测量：从 start 采样（持峰点）起，找读数比起点低 drop_db dB 的首穿
 * 采样。返回首穿采样相对 start 的间隔数；找不到返回 -1。 */
static int first_crossing(const float *sig, int n, int fs, float fall,
                          float dr, int start, float start_peak, float drop_db) {
    LevelMeter m;
    CK(meter_init(&m, fs, fall, dr) == 0);
    float threshold = start_peak * powf(10.0f, -drop_db / 20.0f);
    for (int i = 0; i < n; ++i) {
        float p = meter_update(&m, sig[i]);
        if (i >= start && p <= threshold) return i - start;
    }
    return -1;
}

static void test_meter_errors(void) {
    LevelMeter m;
    /* 空输入要报错，并能给出原因 */
    CK(meter_init(NULL, 10, 20.0f, 60.0f) == METER_E_NULL);
    CK(meter_init(&m, 0, 20.0f, 60.0f) == METER_E_PARAM);
    CK(meter_init(&m, 10, 20.0f, 0.0f) == METER_E_PARAM);
    float alpha;
    CK(meter_fall_alpha(10, 20.0f, NULL) == METER_E_NULL);
    CK(meter_fall_alpha(0, 20.0f, &alpha) == METER_E_PARAM);
    CK(meter_init(&m, 10, 20.0f, 60.0f) == 0);
    float sig[1] = {0.5f}, pk;
    CK(meter_process(NULL, sig, 1, &pk) == METER_E_NULL);
    CK(meter_process(&m, NULL, 1, &pk) == METER_E_NULL);
    CK(meter_process(&m, sig, 0, &pk) == METER_E_EMPTY);
    CK(meter_process(&m, sig, -3, &pk) == METER_E_EMPTY);
    CK(meter_export_curve(&m, sig, 0, 1, "build/x.csv") == METER_E_EMPTY);
    CK(meter_export_curve(&m, sig, 1, 0, "build/x.csv") == METER_E_PARAM);
    CK(meter_export_curve(&m, sig, 1, 1, NULL) == METER_E_NULL);
    int codes[] = {METER_E_NULL, METER_E_EMPTY, METER_E_PARAM};
    for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); ++i) {
        const char *why = meter_strerror(codes[i]);
        CK(why != NULL && why[0] != '\0');
        printf("错误码 %d -> %s\n", codes[i], why);
    }
}

static void test_meter_legacy(void) {
    /* 退化：fall<=0 -> alpha=0，读数逐点立即跟随 |x|，块峰值 == 朴素 max|x| */
    float alpha;
    CK(meter_fall_alpha(48000, 0.0f, &alpha) == 0 && alpha == 0.0f);
    CK(meter_fall_alpha(48000, -5.0f, &alpha) == 0 && alpha == 0.0f);
    LevelMeter m;
    CK(meter_init(&m, 48000, 0.0f, 80.0f) == 0);
    float sig[5] = {0.5f, -0.2f, 0.8f, 0.0f, 0.1f};
    float naive = 0.0f, block_peak;
    for (int i = 0; i < 5; ++i) {
        float p = meter_update(&m, sig[i]);
        if (fabsf(sig[i]) > naive) naive = fabsf(sig[i]);
        CK(fabsf(p - fabsf(sig[i])) < 1e-7f);  /* 立即跟随，无阻尼残留 */
    }
    CK(meter_process(&m, sig, 5, &block_peak) == 0);
    CK(fabsf(block_peak - naive) < 1e-7f);
}

static void test_meter_small(void) {
    /* 小图：fs=10, fall=20 dB/s, DR=60 dB，单位冲激 + 14 个零点。
     * 黄金值取自 Python binary32 仿真模型（与 meter.c 同样逐运算舍入），
     * 故 C 结果应只相差数个 ulp；double 物理真值见 artifacts/hand_small.csv。 */
    LevelMeter m;
    CK(meter_init(&m, 10, 20.0f, 60.0f) == 0);
    CK(fabsf(m.alpha - 0.794328213f) < 1e-9f);   /* f32 舍入后的 alpha */
    CK(fabsf(m.floor_lin - 0.001f) < 1e-12f);
    struct { int k; float peak; } gold[] = {
        {1, 0.794328213f}, {2, 0.630957305f}, {3, 0.501187205f},
        {5, 0.316227734f}, {10, 0.0999999717f}, {14, 0.0398106985f}};
    float sig[15] = {1.0f};
    int gi = 0;
    for (int i = 0; i < 15; ++i) {
        float p = meter_update(&m, sig[i]);
        if (gi < (int)(sizeof(gold) / sizeof(gold[0])) && i == gold[gi].k) {
            float tol = 1e-7f + 4.0f * 5.9604645e-8f * gold[gi].peak; /* 4 ulp */
            CK(fabsf(p - gold[gi].peak) <= tol);
            ++gi;
        }
    }
    /* 攻击即时：0.2 后给 0.9，读数立刻到 0.9 */
    LevelMeter a;
    CK(meter_init(&a, 10, 20.0f, 60.0f) == 0);
    CK(fabsf(meter_update(&a, 0.2f) - 0.2f) < 1e-7f);
    CK(fabsf(meter_update(&a, 0.9f) - 0.9f) < 1e-7f);
    /* 回落时间：降 20 dB 理论 1.000s（10 个采样） */
    int dk = first_crossing(sig, 15, 10, 20.0f, 60.0f, 0, 1.0f, 20.0f);
    CK(dk == 10);
    float tm = (float)dk / 10.0f, tt = 20.0f / 20.0f;
    CK(fabsf(tm - tt) < 1e-6f);
    /* 导出曲线，且导出不改变表读数 */
    LevelMeter ex;
    CK(meter_init(&ex, 10, 20.0f, 60.0f) == 0);
    for (int i = 0; i < 15; ++i) meter_update(&ex, sig[i]);
    float before = ex.peak;
    int rows = meter_export_curve(&ex, sig, 15, 1, "build/meter_test_curve.csv");
    CK(rows == 15);
    CK(fabsf(ex.peak - before) < 1e-12f);
}

static void test_meter_large(void) {
    /* 大图：fs=48000, fall=33 dB/s, DR=80 dB；0.25s 恒幅 0.8 后 1.75s 静音 */
    enum { N = 96000, TONE = 12000 };
    float *sig = (float *)calloc(N, sizeof(float));
    CK(sig != NULL);
    for (int i = 0; i < TONE; ++i) sig[i] = 0.8f;
    LevelMeter m;
    CK(meter_init(&m, 48000, 33.0f, 80.0f) == 0);
    CK(m.alpha > 0.9999f && m.alpha < 1.0f);
    CK(fabsf(m.alpha - 0.999920845f) < 1e-9f);  /* f32 舍入后的 alpha */
    CK(fabsf(m.floor_lin - 0.0001f) < 1e-12f);
    /* 黄金值取自 Python binary32 仿真模型（C 对它只应差数个 ulp）；
     * double 物理真值与 float32 累积误差见 artifacts/verify_report.txt。 */
    struct { int k; float peak; } gold[] = {
        {0, 0.8f},
        {12000, 0.799936712f},
        {41088, 0.0799964666f},
        {95999, 0.0010359633f}};
    int gi = 0;
    for (int i = 0; i < N; ++i) {
        float p = meter_update(&m, sig[i]);
        if (gi < (int)(sizeof(gold) / sizeof(gold[0])) && i == gold[gi].k) {
            float tol = 1e-7f + 4.0f * 5.9604645e-8f * gold[gi].peak; /* 4 ulp */
            CK(fabsf(p - gold[gi].peak) <= tol);
            ++gi;
        }
    }
    /* 回落时间：从最后一个持峰采样 TONE-1 起算，降 20dB。
     * (1) 确定性：C 首穿间隔必须与 binary32 仿真模型完全一致（29089 采样）；
     * (2) 健全性：相对连续理论 20/33 s 的误差不超过解析上界 ——
     *     1 个采样的离散量化 + alpha 量化经 steps 次连乘的漂移。 */
    int dk = first_crossing(sig, N, 48000, 33.0f, 80.0f, TONE - 1, 0.8f, 20.0f);
    CK(dk == 29089);
    double tm = (double)dk / 48000.0, tt = 20.0 / 33.0;
    /* 理想 alpha 用 double 字面量（不可加 f，否则被舍入成 f32 使相对误差为 0） */
    double alpha_ideal = 0.99992085177;
    double alpha_rel = fabs((double)m.alpha - alpha_ideal) / alpha_ideal;
    double rate = 33.0 * log(10.0) / (20.0 * 48000.0); /* 每采样 |ln alpha| */
    double bound = (1.0 + (double)dk * alpha_rel / rate) / 48000.0;
    printf("大图回落时间: 实测 %.6f s 理论 %.6f s 误差 %.3f ms (%.2f 采样, 解析上界 %.2f 采样)\n",
           tm, tt, (tm - tt) * 1000.0, (tm - tt) * 48000.0, bound * 48000.0);
    CK(fabs(tm - tt) <= bound);
    free(sig);
}

int main(void) {
    test_mixer_bus();
    test_meter_errors();
    test_meter_legacy();
    test_meter_small();
    test_meter_large();
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
