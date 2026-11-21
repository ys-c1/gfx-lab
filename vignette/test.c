#include "vignette.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

static float max_err(const float *a, const float *b, size_t n) {
    float m = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        float e = fabsf(a[i] - b[i]);
        if (e > m) m = e;
    }
    return m;
}

int main(void) {
    VignetteCfg cfg = {0.65f, 0.35f, 0.95f, 1.5f, 256, 1};
    const char *where = NULL;

    /* 合法配置 */
    CK(vg_validate(&cfg, &where) == 0 && where == NULL);

    /* 非法值必须拒绝，并说明位置 */
    VignetteCfg bad = cfg;
    bad.strength = 1.5f;
    CK(vg_validate(&bad, &where) == 1 && strcmp(where, "strength") == 0);
    bad = cfg; bad.strength = NAN;
    CK(vg_validate(&bad, &where) == 1 && strcmp(where, "strength") == 0);
    bad = cfg; bad.inner = -0.1f;
    CK(vg_validate(&bad, &where) == 2 && strcmp(where, "inner") == 0);
    bad = cfg; bad.outer = 0.2f; /* outer <= inner */
    CK(vg_validate(&bad, &where) == 3 && strcmp(where, "outer") == 0);
    bad = cfg; bad.outer = 1.5f;
    CK(vg_validate(&bad, &where) == 3 && strcmp(where, "outer") == 0);
    bad = cfg; bad.gamma = 0.0f;
    CK(vg_validate(&bad, &where) == 4 && strcmp(where, "gamma") == 0);
    bad = cfg; bad.gamma = INFINITY;
    CK(vg_validate(&bad, &where) == 4 && strcmp(where, "gamma") == 0);
    bad = cfg; bad.lut_size = 1;
    CK(vg_validate(&bad, &where) == 5 && strcmp(where, "lut_size") == 0);
    bad = cfg; bad.enabled = 2;
    CK(vg_validate(&bad, &where) == 6 && strcmp(where, "enabled") == 0);
    CK(vg_validate(NULL, &where) == -1 && strcmp(where, "cfg") == 0);

    /* 非法配置必须被各入口拒绝 */
    float px[4] = {0, 0, 0, 0};
    CK(vg_weights_ref(&bad, 2, 2, px) == -1);
    CK(vg_apply(&bad, 2, 2, px, px, px) == -1);
    CK(vg_stats(&bad, 2, 2, px, NULL) == -1);
    CK(vg_export_curve(&bad, "build/x.csv", 4) == -1);

    int n = 64;
    size_t npx = (size_t)n * (size_t)n;
    float *ref = malloc(npx * sizeof(float));
    float *fast = malloc(npx * sizeof(float));
    float *in = malloc(npx * sizeof(float));
    float *out = malloc(npx * sizeof(float));
    float *lut = malloc((size_t)cfg.lut_size * sizeof(float));
    CK(ref && fast && in && out && lut);

    /* 第一段：权重 */
    CK(vg_lut_build(&cfg, lut, cfg.lut_size) == 0);
    CK(vg_weights_ref(&cfg, n, n, ref) == 0);
    CK(vg_weights_fast(&cfg, n, n, lut, cfg.lut_size, fast) == 0);
    /* 中心权重 1，角落权重 0 */
    CK(fabsf(ref[(size_t)32 * n + 32] - 1.0f) < 1e-6f);
    CK(ref[0] < 1e-6f);
    /* LUT 单调不增 */
    int mono = 1;
    for (int i = 1; i < cfg.lut_size; ++i) if (lut[i] > lut[i - 1] + 1e-7f) mono = 0;
    CK(mono);
    /* 快速路径与暴力遍历对照，64 下误差有界 */
    CK(max_err(fast, ref, npx) < 0.01f);

    /* 第二段：按权重压暗 */
    for (size_t i = 0; i < npx; ++i) in[i] = 0.8f;
    CK(vg_apply(&cfg, n, n, ref, in, out) == 0);
    CK(fabsf(out[(size_t)32 * n + 32] - 0.8f) < 1e-5f);       /* 中心不压暗 */
    CK(fabsf(out[0] - 0.8f * (1.0f - 0.65f)) < 1e-5f);        /* 角落压暗到 1-strength */

    /* 不启用时输出保持原样(逐字节一致，weights 可为 NULL) */
    VignetteCfg off = cfg; off.enabled = 0;
    memset(out, 0, npx * sizeof(float));
    CK(vg_apply(&off, n, n, NULL, in, out) == 0);
    CK(memcmp(out, in, npx * sizeof(float)) == 0);

    /* 统计：强度、边缘像素数、最小权重 */
    VignetteStats st;
    CK(vg_stats(&cfg, n, n, fast, &st) == 0);
    CK(st.strength == 0.65f);
    CK(st.edge_pixels > 0 && st.edge_pixels < (int)npx);
    CK(st.min_weight < 1e-6f);
    CK(vg_stats(&off, n, n, fast, &st) == 0 && st.strength == 0.0f);

    /* 曲线表导出 */
    CK(vg_export_curve(&cfg, "build/test_curve.csv", 17) == 0);
    FILE *f = fopen("build/test_curve.csv", "r");
    CK(f != NULL);
    if (f) {
        int lines = 0, ch;
        while ((ch = fgetc(f)) != EOF) if (ch == '\n') ++lines;
        fclose(f);
        CK(lines == 18); /* 表头 + 17 行采样 */
    }

    /* 512 下快速路径与暴力遍历对照 */
    {
        int m = 512;
        size_t mpx = (size_t)m * (size_t)m;
        float *r2 = malloc(mpx * sizeof(float));
        float *f2 = malloc(mpx * sizeof(float));
        CK(r2 && f2);
        CK(vg_weights_ref(&cfg, m, m, r2) == 0);
        CK(vg_weights_fast(&cfg, m, m, lut, cfg.lut_size, f2) == 0);
        CK(max_err(f2, r2, mpx) < 0.01f);
        free(r2); free(f2);
    }

    free(ref); free(fast); free(in); free(out); free(lut);
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
