#define _POSIX_C_SOURCE 199309L
#include "raster.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static double edgef(const RVert *a, const RVert *b, double px, double py) {
    return (b->x - a->x) * (py - a->y) - (b->y - a->y) * (px - a->x);
}

int main(void) {
    static const double table[] = {1.00, 0.55, 0.80, 0.35};
    const int sizes[2] = {64, 512};
    const int reps[2] = {2000, 40};

    /* 色调表、顶点号：可直接取到 */
    printf("tint table:");
    for (size_t i = 0; i < 4; ++i) printf(" [%zu]=%.2f", i, table[i]);
    printf("\nvertices: tri0 vi={0,1,2}  tri1 vi={1,3,0}\n");

    printf("%-4s %-3s %-56s %9s %9s %11s %12s %11s %8s\n",
           "size", "tri", "coef (a0,b0,c0 | a1,b1,c1 | a2,b2,c2)",
           "fast_ms", "bf_ms", "img_max_err", "img_mean_err", "interp_err", "cov_diff");

    for (int s = 0; s < 2; ++s) {
        int S = sizes[s];
        double k = S / 128.0;
        RTri tris[2] = {
            {{ {  8*k,   8*k, 0.5}, {118*k,  20*k, 0.5}, {64*k, 118*k, 0.5} }, {255, 200,  40}},
            {{ { 20*k, 100*k, 0.5}, {110*k,  90*k, 0.5}, {70*k,  16*k, 0.5} }, { 40, 180, 220}},
        };
        RTint tints[2] = {
            { table, 4, {0, 1, 2} },
            { table, 4, {1, 3, 0} },
        };
        for (int ti = 0; ti < 2; ++ti) {
            RTintPlan plan;
            char err[128];
            if (rt_tint_solve(&tris[ti], &tints[ti], &plan, err, sizeof err) != 0) {
                printf("solve failed: %s\n", err);
                return 1;
            }
            if (S == 64) {
                /* 插值权重：可直接取到 */
                double cx = (tris[ti].v[0].x + tris[ti].v[1].x + tris[ti].v[2].x) / 3.0;
                double cy = (tris[ti].v[0].y + tris[ti].v[1].y + tris[ti].v[2].y) / 3.0;
                double w[3];
                rt_tint_weights(&plan, cx, cy, w);
                printf("tri%d weights @centroid: {%.4f, %.4f, %.4f}  tint=%.4f\n",
                       ti, w[0], w[1], w[2], rt_tint_interp(&plan, w));
            }

            RTarget a, b;
            if (rt_init(&a, S, S) != 0 || rt_init(&b, S, S) != 0) return 1;

            double t0 = now_ms();
            for (int r = 0; r < reps[s]; ++r) rt_triangle_tint(&a, &tris[ti], &tints[ti]);
            double fast = (now_ms() - t0) / reps[s];

            t0 = now_ms();
            for (int r = 0; r < reps[s]; ++r) rt_triangle_tint_bf(&b, &tris[ti], &tints[ti]);
            double bf = (now_ms() - t0) / reps[s];

            /* 误差一：最终图像。只在两条路径都覆盖的像素上比颜色；
               覆盖不一致（边界舍入翻转）的像素单独计数 */
            int maxe = 0, cov = 0;
            double sume = 0;
            for (int i = 0; i < S * S; ++i) {
                int ca = a.z[i] < 1e29f, cb = b.z[i] < 1e29f;
                if (ca != cb) { ++cov; continue; }
                if (!ca) continue;
                for (int ch = 0; ch < 3; ++ch) {
                    int d = abs(a.rgb[i*3+ch] - b.rgb[i*3+ch]);
                    if (d > maxe) maxe = d;
                    sume += d;
                }
            }
            /* 误差二：插值本身，双精度对照暴力遍历 */
            double maxd = 0;
            for (int y = 0; y < S; ++y) {
                for (int x = 0; x < S; ++x) {
                    double px = x + 0.5, py = y + 0.5, w[3];
                    rt_tint_weights(&plan, px, py, w);
                    double kf = rt_tint_interp(&plan, w);
                    double e0 = edgef(&tris[ti].v[1], &tris[ti].v[2], px, py);
                    double e1 = edgef(&tris[ti].v[2], &tris[ti].v[0], px, py);
                    double e2 = edgef(&tris[ti].v[0], &tris[ti].v[1], px, py);
                    double kb = (e0 * plan.tint[0] + e1 * plan.tint[1] + e2 * plan.tint[2]) / (e0 + e1 + e2);
                    double d = fabs(kf - kb);
                    if (d > maxd) maxd = d;
                }
            }

            char coef[96];
            snprintf(coef, sizeof coef, "%.2e,%.2e,%.2e | %.2e,%.2e,%.2e | %.2e,%.2e,%.2e",
                     plan.a[0], plan.b[0], plan.c[0],
                     plan.a[1], plan.b[1], plan.c[1],
                     plan.a[2], plan.b[2], plan.c[2]);
            printf("%-4d %-3d %-56s %9.3f %9.3f %11d %12.4g %11.2e %8d\n",
                   S, ti, coef, fast, bf, maxe, sume / (S * S * 3), maxd, cov);

            rt_free(&a);
            rt_free(&b);
        }
    }
    return 0;
}
