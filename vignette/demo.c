#define _POSIX_C_SOURCE 199309L
#include "vignette.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

static int run(const VignetteCfg *cfg, int n, int iters, float *fast_out) {
    size_t npx = (size_t)n * (size_t)n;
    float *ref = malloc(npx * sizeof(float));
    float *fast = fast_out ? fast_out : malloc(npx * sizeof(float));
    float *lut = malloc((size_t)cfg->lut_size * sizeof(float));
    if (!ref || !fast || !lut) { free(ref); free(lut); return -1; }
    vg_lut_build(cfg, lut, cfg->lut_size);

    double t0 = now_ms();
    for (int i = 0; i < iters; ++i) vg_weights_ref(cfg, n, n, ref);
    double t1 = now_ms();
    for (int i = 0; i < iters; ++i) vg_weights_fast(cfg, n, n, lut, cfg->lut_size, fast);
    double t2 = now_ms();

    double maxe = 0.0, sume = 0.0;
    for (size_t i = 0; i < npx; ++i) {
        double e = fabs((double)fast[i] - (double)ref[i]);
        if (e > maxe) maxe = e;
        sume += e;
    }
    VignetteStats st;
    vg_stats(cfg, n, n, fast, &st);

    printf("[vignette] %dx%d  迭代 %d 次\n", n, n, iters);
    printf("  暗角强度 %.2f  内半径 %.2f  外半径 %.2f  指数 %.2f  LUT %d\n",
           cfg->strength, cfg->inner, cfg->outer, cfg->gamma, cfg->lut_size);
    printf("  边缘像素数 %d / %d  最小权重 %.4f\n", st.edge_pixels, (int)npx, st.min_weight);
    printf("  误差(快速 vs 暴力): max %.6f  mean %.6f\n", maxe, sume / (double)npx);
    printf("  耗时: 暴力 %.3f ms/次  快速 %.3f ms/次\n",
           (t1 - t0) / iters, (t2 - t1) / iters);

    free(ref);
    free(lut);
    if (!fast_out) free(fast);
    return 0;
}

int main(void) {
    VignetteCfg cfg = {0.65f, 0.35f, 0.95f, 1.5f, 256, 1};
    const char *where = NULL;
    int rc = vg_validate(&cfg, &where);
    if (rc != 0) {
        printf("配置非法: 字段 %s (位置 %d)\n", where ? where : "?", rc);
        return 1;
    }

    if (run(&cfg, 64, 2000, NULL) != 0) return 1;

    int n = 512;
    size_t npx = (size_t)n * (size_t)n;
    float *fast = malloc(npx * sizeof(float));
    if (!fast) return 1;
    if (run(&cfg, n, 50, fast) != 0) { free(fast); return 1; }

    /* 第二段演示：平坦灰底压暗后写 PGM */
    float *in = malloc(npx * sizeof(float));
    float *out = malloc(npx * sizeof(float));
    if (!in || !out) { free(fast); free(in); free(out); return 1; }
    for (size_t i = 0; i < npx; ++i) in[i] = 0.8f;
    vg_apply(&cfg, n, n, fast, in, out);
    FILE *f = fopen("demo.pgm", "wb");
    if (!f) { free(fast); free(in); free(out); return 1; }
    fprintf(f, "P5\n%d %d\n255\n", n, n);
    for (size_t i = 0; i < npx; ++i) {
        float v = out[i];
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        unsigned char b = (unsigned char)(v * 255.0f + 0.5f);
        fwrite(&b, 1, 1, f);
    }
    fclose(f);

    if (vg_export_curve(&cfg, "demo_curve.csv", 65) != 0) {
        free(fast); free(in); free(out);
        return 1;
    }
    printf("曲线表已导出 demo_curve.csv (65 行采样)，图像已写出 demo.pgm\n");

    free(fast); free(in); free(out);
    return 0;
}
