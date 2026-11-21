#include "mixer.h"
#include "softclip.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

static void test_mixer(void) {
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

static void test_invalid(void) {
    SoftClip sc;
    unsigned m = 0;

    /* 非法组合必须拒绝, 并列出冲突项 */
    CK(sc_build_curve(&sc, 0.0f, 1.0f, 1024, &m) == -2);
    CK(m & SC_CONF_KNEE_NONPOS);

    CK(sc_build_curve(&sc, -1.0f, 0.5f, 1024, &m) == -2);
    CK(m & SC_CONF_KNEE_NONPOS);             /* 负 knee; 0.5 < -1 不成立故无 CEIL_LT_KNEE */
    CK(!(m & SC_CONF_CEIL_LT_KNEE));

    CK(sc_build_curve(&sc, 0.8f, 0.5f, 1024, &m) == -2);
    CK(m & SC_CONF_CEIL_LT_KNEE);

    CK(sc_build_curve(&sc, 0.8f, 1.0f, 3, &m) == -2);
    CK(m & SC_CONF_TABS_SMALL);

    CK(sc_build_curve(&sc, NAN, 1.0f, 1024, &m) == -2);
    CK(m & SC_CONF_KNEE_NANINF);

    CK(sc_build_curve(&sc, INFINITY, 1.0f, 1024, &m) == -2);
    CK(m & SC_CONF_KNEE_NANINF);

    CK(sc_build_curve(&sc, 0.8f, NAN, 1024, &m) == -2);
    CK(m & SC_CONF_CEIL_NANINF);

    /* 多重冲突同时上报 */
    CK(sc_build_curve(&sc, 0.0f, -1.0f, 2, &m) == -2);
    CK((m & SC_CONF_KNEE_NONPOS) && (m & SC_CONF_CEIL_NONPOS) &&
       (m & SC_CONF_TABS_SMALL));

    /* 失败后不可用, apply 退化为直通 */
    CK(sc.valid == 0);
    float passthru[4] = {2.0f, -2.0f, 0.3f, -0.3f};
    CK(sc_apply(&sc, passthru, 2, 2) == 4);
    CK(passthru[0] == 2.0f && passthru[1] == -2.0f &&
       passthru[2] == 0.3f && passthru[3] == -0.3f);

    CK(sc_build_curve(NULL, 0.8f, 1.0f, 1024, &m) == -1);
}

static void test_analytic(void) {
    const float T = 0.8f, C = 1.0f;

    /* 拐点以下恒等 */
    CK(sc_analytic(T, C, 0.0f) == 0.0f);
    CK(fabsf(sc_analytic(T, C, 0.5f) - 0.5f) < 1e-6f);

    /* 拐点处连续且值为 T */
    CK(fabsf(sc_analytic(T, C, T) - T) < 1e-6f);

    /* C1 连续: 拐点两侧导数都为 1 (中心差分) */
    const float h = 1e-4f;
    float dl = (sc_analytic(T, C, T) - sc_analytic(T, C, T - h)) / h;
    float dr = (sc_analytic(T, C, T + h) - sc_analytic(T, C, T)) / h;
    CK(fabsf(dl - 1.0f) < 1e-3f);
    CK(fabsf(dr - 1.0f) < 1e-3f);

    /* 奇对称 */
    CK(fabsf(sc_analytic(T, C, 1.5f) + sc_analytic(T, C, -1.5f)) < 1e-6f);

    /* 压缩: 拐点以上输出小于输入, 且永不超过 ceiling (float 容差) */
    for (float x = T; x < 10.0f; x += 0.1f) {
        float y = sc_analytic(T, C, x);
        CK(y <= x + 1e-6f);
        CK(y <= C + 1e-7f);
    }
    /* 中等输入处严格压缩且严格小于 ceiling (此时未到 float 平台段) */
    CK(sc_analytic(T, C, 1.5f) < C);
    CK(sc_analytic(T, C, T + 0.01f) < C);
    /* 渐近: 大输入在 float 精度内逼近 ceiling, 数学上仍不硬切 */
    CK(fabsf(sc_analytic(T, C, 20.0f) - C) < 1e-2f);

    /* C == T: 合法的退化组合, 等价于在 T 处硬切 */
    CK(fabsf(sc_analytic(0.8f, 0.8f, 0.5f) - 0.5f) < 1e-6f);
    CK(sc_analytic(0.8f, 0.8f, 5.0f) == 0.8f);
    CK(sc_analytic(0.8f, 0.8f, -5.0f) == -0.8f);

    /* 单调性 */
    float prev = sc_analytic(T, C, -5.0f);
    for (float x = -4.99f; x <= 5.0f; x += 0.01f) {
        float y = sc_analytic(T, C, x);
        CK(y >= prev);
        prev = y;
    }
}

static void test_apply_and_lut(void) {
    SoftClip sc;
    unsigned m = 0;
    CK(sc_build_curve(&sc, 0.8f, 1.0f, 1024, &m) == 0);
    CK(m == 0 && sc.valid == 1);

    /* 拐点以下逐样本位级不变 */
    float sig[8] = {0.0f, 0.79f, -0.79f, 0.5f, -0.5f, 0.1f, -0.1f, 0.8f};
    float ref[8];
    memcpy(ref, sig, sizeof sig);
    CK(sc_apply(&sc, sig, 4, 2) == 8);
    for (int i = 0; i < 7; ++i) CK(sig[i] == ref[i]);

    /* 拐点处也应等于原值 (表起点精确) */
    CK(fabsf(sig[7] - 0.8f) < 1e-6f);

    /* 逐样本对照解析解: 含表内、表尾外、负值 */
    float probe[12] = {0.8f, 1.0f, 1.5f, 2.0f, 3.0f, 6.399f,
                      -0.8f, -1.0f, -1.5f, -2.0f, -3.0f, -9.0f};
    float probe_ref[12];
    memcpy(probe_ref, probe, sizeof probe);
    CK(sc_apply(&sc, probe, 6, 2) == 12);
    for (int i = 0; i < 12; ++i) {
        float ya = sc_analytic(0.8f, 1.0f, probe_ref[i]);
        CK(fabsf(probe[i] - ya) < 2e-3f);     /* LUT 插值容差 */
        CK(fabsf(probe[i]) <= 1.0f);          /* 不越 ceiling */
    }

    /* 未启用的 SoftClip: 直通 */
    SoftClip off = {0};
    float raw[4] = {5.0f, -5.0f, 0.2f, -0.2f};
    CK(sc_apply(&off, raw, 2, 2) == 4);
    CK(raw[0] == 5.0f && raw[1] == -5.0f);
    CK(sc_apply(NULL, raw, 2, 2) == 4);       /* 空指针同样直通 */

    /* 非法缓冲参数 */
    CK(sc_apply(&sc, NULL, 4, 2) == -1);
    CK(sc_apply(&sc, raw, 0, 2) == -1);
    CK(sc_apply(&sc, raw, 4, 0) == -1);

    /* C == T 时 apply 退化为硬切 */
    SoftClip hard;
    CK(sc_build_curve(&hard, 0.8f, 0.8f, 64, &m) == 0);
    float hc[6] = {0.5f, -0.5f, 2.0f, -2.0f, 0.8f, -0.8f};
    sc_apply(&hard, hc, 3, 2);
    CK(hc[0] == 0.5f && hc[1] == -0.5f);
    CK(hc[2] == 0.8f && hc[3] == -0.8f);
    CK(hc[4] == 0.8f && hc[5] == -0.8f);
    sc_free(&hard);

    /* 曲线表导出 */
    CK(sc_export_csv(&sc, "build/curve_unit.csv", 256, 4.0f) == 0);
    FILE *f = fopen("build/curve_unit.csv", "r");
    CK(f != NULL);
    if (f) {
        char line[128];
        CK(fgets(line, sizeof line, f) != NULL);
        CK(strncmp(line, "input,analytic,lut", 18) == 0);
        int rows = 0;
        while (fgets(line, sizeof line, f)) ++rows;   /* 不含表头 */
        CK(rows == 256);
        fclose(f);
    }
    CK(sc_export_csv(&sc, "build/no/such/dir.csv", 16, 4.0f) == -2);

    sc_free(&sc);
    CK(sc.valid == 0);
}

int main(void) {
    test_mixer();
    test_invalid();
    test_analytic();
    test_apply_and_lut();
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
