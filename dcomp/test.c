#include "dcomp.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

static float db_gain_expected(float x, const DcParams *p) {
    /* 独立的 dB 域手算公式（与库内线性域实现无关），仅用于对照。
       线性静态特性：y=thr+(a-thr)/ratio，增益 dB = 20log10(y/a) */
    float a = fabsf(x);
    if (a <= p->threshold) return 1.0f;
    if (a < p->ceiling) {
        float y = p->threshold + (a - p->threshold) / p->ratio;
        float gain_db = 20.0f * log10f(y / a);    /* 全程 dB 量纲 */
        return powf(10.0f, gain_db / 20.0f);
    }
    return p->ceiling / a;
}

int main(void) {
    DcParams base = { 0.5f, 4.0f, 0.9f, 5.0f, 100.0f, 48000, 0 };

    /* ---- 1. 参数校验：门限为零、比率为负要报错并指出字段 ---- */
    DcParams bad = base;
    bad.threshold = 0.0f;
    CK(dc_validate(&bad) == -1);                 /* stderr 应打印字段 'threshold' */
    bad = base; bad.ratio = -2.0f;
    CK(dc_validate(&bad) == -1);                 /* stderr 应打印字段 'ratio' */
    bad = base; bad.ratio = 0.0f;
    CK(dc_validate(&bad) == -1);
    bad = base; bad.ceiling = 0.25f;             /* ceiling < threshold */
    CK(dc_validate(&bad) == -1);
    bad = base; bad.sample_rate = 0;
    CK(dc_validate(&bad) == -1);
    bad = base; bad.attack_ms = -1.0f;
    CK(dc_validate(&bad) == -1);
    CK(dc_validate(&base) == 0);
    CK(dc_validate(NULL) == -1);

    /* ---- 2. 不加处理（bypass）：逐样本完全一致 ---- */
    {
        DcParams bp = base; bp.bypass = 1;
        float in[8], out[8], gain[8];
        unsigned char tier[8];
        for (int i = 0; i < 8; ++i) in[i] = 0.7f * sinf((float)i);
        CK(dc_process(&bp, in, out, 8, gain, tier, NULL) == 0);
        CK(memcmp(in, out, sizeof(in)) == 0);
        for (int i = 0; i < 8; ++i) CK(gain[i] == 1.0f);
    }

    /* ---- 3. 自然静默（全程低于门限）：也必须逐样本一致 ---- */
    {
        float in[64], out[64];
        for (int i = 0; i < 64; ++i) in[i] = 0.1f * sinf((float)i * 0.3f);
        DcStats st;
        CK(dc_process(&base, in, out, 64, NULL, NULL, &st) == 0);
        CK(memcmp(in, out, sizeof(in)) == 0);
        CK(st.over_count == 0);
        CK(st.max_gain_reduction_db == 0.0f);
        CK(st.measured_release_ms == -1.0);
    }

    /* ---- 4. 瞬时包络下手算增益曲线逐样本对照（线性域 + dB 域双公式） ---- */
    {
        DcParams inst = base; inst.attack_ms = 0.0f; inst.release_ms = 0.0f;
        float in[5]  = { 0.25f, 0.5f, 0.8f, 1.0f, -0.8f };
        float out[5], gain[5];
        unsigned char tier[5];
        DcStats st;
        CK(dc_process(&inst, in, out, 5, gain, tier, &st) == 0);

        /* 手算（threshold=.5 ratio=4 ceiling=.9）：
           .25 低档 -> g=1
           .5  边界归低档 -> g=1
           .8  压缩档 -> y=.5+(.8-.5)/4=.575, g=.575/.8=.71875
           1.0 限幅档 -> g=.9, 输出 .9
           -.8 压缩档 -> 同 .8，输出 -.575 */
        CK(fabsf(gain[0] - 1.0f) < 1e-7f);
        CK(fabsf(gain[1] - 1.0f) < 1e-7f);
        CK(fabsf(gain[2] - 0.71875f) < 1e-7f);
        CK(fabsf(gain[3] - 0.9f) < 1e-7f);
        CK(fabsf(gain[4] - 0.71875f) < 1e-7f);
        CK(fabsf(out[2] - 0.575f) < 1e-7f);
        CK(fabsf(out[3] - 0.9f) < 1e-7f);
        CK(fabsf(out[4] + 0.575f) < 1e-7f);
        CK(tier[0] == DC_TIER_LOW && tier[1] == DC_TIER_LOW);
        CK(tier[2] == DC_TIER_COMPRESS && tier[4] == DC_TIER_COMPRESS);
        CK(tier[3] == DC_TIER_LIMIT);

        /* dB 域独立公式逐样本对照 */
        for (int i = 0; i < 5; ++i)
            CK(fabsf(gain[i] - db_gain_expected(in[i], &inst)) < 1e-6f);

        /* 统计量 */
        CK(st.in_peak == 1.0f);
        CK(st.out_peak == 0.9f);
        CK(st.peak_over == 0.0f);                 /* 限幅兜底，峰值零超调 */
        CK(st.over_count == 3);
        CK(st.limit_count == 1);
        CK(st.tier_hist[DC_TIER_LOW] == 2);
        CK(st.tier_hist[DC_TIER_COMPRESS] == 2);
        CK(st.tier_hist[DC_TIER_LIMIT] == 1);
        /* 最大压缩量出现在压缩档 .8 样本：g=.71875，GR=20log10(1/.71875) */
        CK(fabsf(st.max_gain_reduction_db - (-20.0f * log10f(0.71875f))) < 1e-5f);
        CK(fabs(st.over_ms - 3.0 * 1000.0 / 48000.0) < 1e-9);
    }

    /* ---- 5. 启动期包络来不及下压：限幅钳制保证不超天花板 ---- */
    {
        DcParams slow = base; slow.attack_ms = 20.0f; slow.release_ms = 50.0f;
        float in[4096], out[4096], gain[4096];
        for (int i = 0; i < 4096; ++i) in[i] = 1.0f;
        DcStats st;
        CK(dc_process(&slow, in, out, 4096, gain, NULL, &st) == 0);
        CK(st.out_peak <= slow.ceiling + 1e-7f);
        CK(st.peak_over == 0.0f);
        CK(fabsf(out[0] - 0.9f) < 1e-7f);          /* 首样本即被钳到天花板 */
        CK(gain[0] == 0.9f);                       /* 曲线写回的是实际生效增益 */
        for (int i = 0; i < 4096; ++i) CK(fabsf(out[i]) <= 0.9f + 1e-7f);
    }

    /* ---- 6. 实测恢复时间：20 个高样本 + 长静默，release=10ms ---- */
    {
        DcParams p = base; p.attack_ms = 0.0f; p.release_ms = 10.0f;
        int n = 4800;                              /* 100ms 素材，留足恢复窗口 */
        float *in = calloc((size_t)n, sizeof(float));
        float *out = malloc((size_t)n * sizeof(float));
        float *gain = malloc((size_t)n * sizeof(float));
        CK(in && out && gain);
        for (int i = 0; i < 20; ++i) in[i] = 0.8f; /* 其后全 0（静默） */
        DcStats st;
        CK(dc_process(&p, in, out, n, gain, NULL, &st) == 0);
        /* 手算：rel=1-exp(-1000/(10*48000))；从 .71875 首达 >=0.988553 的偏移 */
        double rel = 1.0 - exp(-1000.0 / (10.0 * 48000.0));
        double g = 0.71875;
        int k = 0;
        while (g < 0.988553094652676 && k < n - 20) { g += rel * (1.0 - g); ++k; }
        CK(g >= 0.988553094652676);                /* 手算确认窗口内确实恢复 */
        CK(st.release_truncated == 0);
        CK(fabs(st.measured_release_ms - (double)k * 1000.0 / 48000.0) < 1e-9);
        /* 一阶极点需约 3.2 个时间常数才从 -2.87dB 回到 -0.1dB，实测≈32ms */
        CK(k >= 1400 && k <= 1700);
        free(in); free(out); free(gain);
    }

    /* ---- 7. 素材太短未恢复：截断标志 ---- */
    {
        DcParams p = base; p.attack_ms = 0.0f; p.release_ms = 500.0f;
        float in[40], out[40];
        for (int i = 0; i < 40; ++i) in[i] = 0.8f;
        DcStats st;
        CK(dc_process(&p, in, out, 40, NULL, NULL, &st) == 0);
        CK(st.release_truncated == 1);
    }

    /* ---- 8. 增益曲线导出 ---- */
    {
        DcParams inst = base; inst.attack_ms = 0.0f; inst.release_ms = 0.0f;
        float in[3] = { 0.2f, 0.8f, 1.0f }, out[3], gain[3];
        unsigned char tier[3];
        CK(dc_process(&inst, in, out, 3, gain, tier, NULL) == 0);
        FILE *fp = fopen("build/curve_test.csv", "wb");
        CK(fp != NULL);
        CK(dc_export_curve(&inst, in, 3, gain, tier, fp) == 0);
        fclose(fp);
        fp = fopen("build/curve_test.csv", "rb");
        char hdr[128];
        CK(fgets(hdr, sizeof hdr, fp) != NULL);
        static const char want[] = "sample,time_ms,in,level,gain_db,out,tier";
        CK(strncmp(hdr, want, strlen(want)) == 0);
        int lines = 0;
        while (fgets(hdr, sizeof hdr, fp)) ++lines;
        CK(lines == 3);                            /* 每样本一行 */
        fclose(fp);
    }

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
