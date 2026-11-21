/* 画线基准：32 与 256 两档比误差和耗时。
 * 表列：线型、线宽档位、分档 levels、覆盖像素数、断点数、MAE 误差、耗时。
 * 误差基准：同一几何的精确覆盖率（未量化的手算公式）逐像素对比。
 * 断点：沿主轴相邻两列的已画像素集合没有同行像素（4-连通断裂）的次数。 */
#define _POSIX_C_SOURCE 199309L
#include "raster.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define W 1024
#define H 768
#define ITERS 100

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* 手算参考：与 rt_line 无关的独立覆盖率计算，返回精确亮度图（0..255 浮点） */
static float *exact_intensity(double x0, double y0, double x1, double y1, double width) {
    float *ref = (float *)calloc((size_t)W * H, sizeof(float));
    double dx = x1 - x0, dy = y1 - y0;
    double len = hypot(dx, dy);
    double amaj = fmax(fabs(dx), fabs(dy));
    double wcov = width * len / amaj;
    int xmajor = fabs(dx) >= fabs(dy);
    double a0 = xmajor ? x0 : y0, a1 = xmajor ? x1 : y1;
    double b0 = xmajor ? y0 : x0, b1 = xmajor ? y1 : x1;
    if (a0 > a1) { double t = a0; a0 = a1; a1 = t; t = b0; b0 = b1; b1 = t; }
    double half = wcov * 0.5, da = a1 - a0;
    for (int m = (int)floor(a0); m <= (int)floor(a1); ++m) {
        double s = m + 0.5;
        if (s < a0 || s > a1) continue;
        double c = b0 + (b1 - b0) * (s - a0) / da;
        double lo = c - half, hi = c + half;
        for (int j = (int)floor(lo); j <= (int)ceil(hi) - 1; ++j) {
            double cov = fmin(hi, j + 1.0) - fmax(lo, (double)j);
            if (cov <= 0.0) continue;
            if (cov > 1.0) cov = 1.0;
            int x = xmajor ? m : j, y = xmajor ? j : m;
            if (x < 0 || y < 0 || x >= W || y >= H) continue;
            float v = (float)(cov * 255.0);
            size_t i = (size_t)y * W + x;
            if (v > ref[i]) ref[i] = v;
        }
    }
    return ref;
}

/* 断点数：沿主轴逐列扫描，相邻列已画像素（红通道>0）无共同行则记一个断点 */
static int count_gaps(const unsigned char *img, double x0, double y0, double x1, double y1) {
    double dx = x1 - x0, dy = y1 - y0;
    int xmajor = fabs(dx) >= fabs(dy);
    double a0 = xmajor ? x0 : y0, a1 = xmajor ? x1 : y1;
    if (a0 > a1) { double t = a0; a0 = a1; a1 = t; }
    int gaps = 0;
    unsigned char prev[ H > W ? H : W ], cur[H > W ? H : W];
    int first = 1, nminor = xmajor ? H : W;
    for (int m = (int)floor(a0); m <= (int)floor(a1); ++m) {
        memset(cur, 0, (size_t)nminor);
        for (int j = 0; j < nminor; ++j) {
            int x = xmajor ? m : j, y = xmajor ? j : m;
            if (x < 0 || y < 0 || x >= W || y >= H) continue;
            if (img[((size_t)y * W + x) * 3] > 0) cur[j] = 1;
        }
        int any = 0;
        for (int j = 0; j < nminor; ++j) if (cur[j]) { any = 1; break; }
        if (!any) continue; /* 空列（端点取整所致）不计断点 */
        if (!first) {
            int connected = 0;
            for (int j = 0; j < nminor; ++j)
                if (cur[j] && prev[j]) { connected = 1; break; }
            if (!connected) ++gaps;
        }
        memcpy(prev, cur, (size_t)nminor);
        first = 0;
    }
    return gaps;
}

/* MAE：渲染结果（红通道）与精确覆盖率参考，在任一非零像素上取平均绝对误差 */
static double mae(const unsigned char *img, const float *ref) {
    double sum = 0.0;
    int n = 0;
    for (size_t i = 0; i < (size_t)W * H; ++i) {
        double r = img[i * 3], e = ref[i];
        if (r > 0.0 || e > 0.0) { sum += fabs(r - e); ++n; }
    }
    return n ? sum / n : 0.0;
}

static void run(const char *name, double x0, double y0, double x1, double y1,
                double width, int levels, int aa, float *ref) {
    RTarget t;
    rt_init(&t, W, H);
    RLine l = { x0, y0, x1, y1, width, levels, aa, {255, 255, 255}, {0} };
    int covered = rt_line(&t, &l);
    int gaps = count_gaps(t.rgb, x0, y0, x1, y1);
    double err = mae(t.rgb, ref);
    double t0 = now_s();
    for (int it = 0; it < ITERS; ++it) {
        RTarget tmp;
        rt_init(&tmp, W, H);
        RLine l2 = l;
        rt_line(&tmp, &l2);
        rt_free(&tmp);
    }
    double us = (now_s() - t0) / ITERS * 1e6;
    printf("| %s | %.1f | %s | %d | %d | %.4f | %.1f |\n",
           name, width, aa ? (levels == 32 ? "32" : "256") : "关(基线)",
           covered, gaps, err, us);
    rt_free(&t);
}

int main(void) {
    struct { const char *name; double x0, y0, x1, y1; } lines[2] = {
        { "横线", 0.5, 384.5, 1023.5, 384.5 },
        { "斜线", 0.5, 0.5, 1023.5, 767.5 },
    };
    double widths[3] = { 1.0, 2.0, 4.0 };
    printf("| 线型 | 线宽档位 | 分档 | 覆盖像素数 | 断点数 | MAE误差 | 耗时us |\n");
    printf("|---|---|---|---|---|---|---|\n");
    for (int k = 0; k < 2; ++k) {
        for (int wi = 0; wi < 3; ++wi) {
            double wdt = widths[wi];
            float *ref = exact_intensity(lines[k].x0, lines[k].y0,
                                         lines[k].x1, lines[k].y1, wdt);
            run(lines[k].name, lines[k].x0, lines[k].y0, lines[k].x1, lines[k].y1,
                wdt, 256, 0, ref);
            run(lines[k].name, lines[k].x0, lines[k].y0, lines[k].x1, lines[k].y1,
                wdt, 32, 1, ref);
            run(lines[k].name, lines[k].x0, lines[k].y0, lines[k].x1, lines[k].y1,
                wdt, 256, 1, ref);
            free(ref);
        }
    }
    return 0;
}
