#define _POSIX_C_SOURCE 199309L
#include "raster.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static double edgef(double ax, double ay, double bx, double by, double px, double py) {
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

/* 归一化坐标场景，按分辨率缩放 */
#define NTRI 5
static void make_scene(RTri *tris, int n) {
    static const double pts[NTRI][3][3] = { /* x, y, z */
        {{0.05,0.05,0.90}, {0.95,0.10,0.40}, {0.50,0.90,0.60}},
        {{0.10,0.80,0.30}, {0.90,0.70,0.80}, {0.60,0.05,0.20}},
        {{0.20,0.20,0.50}, {0.80,0.30,0.50}, {0.50,0.75,0.70}},
        {{0.00,0.50,0.85}, {0.40,0.95,0.35}, {0.45,0.40,0.45}},
        {{0.55,0.15,0.25}, {1.00,0.55,0.65}, {0.60,0.60,0.55}},
    };
    static const unsigned char cols[NTRI][3] = {
        {220, 40, 40}, {40, 200, 220}, {240, 200, 40}, {120, 60, 220}, {60, 220, 90},
    };
    for (int i = 0; i < NTRI; ++i) {
        for (int v = 0; v < 3; ++v) {
            tris[i].v[v].x = pts[i][v][0] * (double)n;
            tris[i].v[v].y = pts[i][v][1] * (double)n;
            tris[i].v[v].z = pts[i][v][2];
        }
        memcpy(tris[i].rgb, cols[i], 3);
    }
}

/* 逐像素参考：对每个像素逐一判覆盖并取最近深度（双精度） */
static int ref_depth(const RTri *tris, int ntri, int n, double *ref) {
    int covered = 0;
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            double px = x + 0.5, py = y + 0.5, best = 1e30;
            for (int i = 0; i < ntri; ++i) {
                const RVert *v = tris[i].v;
                double w0 = edgef(v[1].x, v[1].y, v[2].x, v[2].y, px, py);
                double w1 = edgef(v[2].x, v[2].y, v[0].x, v[0].y, px, py);
                double w2 = edgef(v[0].x, v[0].y, v[1].x, v[1].y, px, py);
                if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                double sum = w0 + w1 + w2;
                double z = (w0 * v[0].z + w1 * v[1].z + w2 * v[2].z) / sum;
                if (z < best) best = z;
            }
            ref[y * n + x] = best;
            if (best < 1e30) ++covered;
        }
    }
    return covered;
}

static void reset_target(RTarget *t) {
    int np = t->w * t->h;
    for (int i = 0; i < np; ++i) t->z[i] = 1e30f;
    memset(t->rgb, 0, (size_t)np * 3);
}

static void render(RTarget *t, const RTri *tris, const RDepthMask *m, int use_ex) {
    if (use_ex) {
        for (int i = 0; i < NTRI; ++i) rt_triangle_ex(t, &tris[i], m, i);
    } else {
        for (int i = 0; i < NTRI; ++i) rt_triangle(t, &tris[i]);
    }
}

int main(void) {
    const int sizes[2] = {64, 256};
    const int reps[2] = {2000, 200};
    RTri tris[NTRI];

    printf("== 掩码打开：覆盖像素 / 耗时 / 与逐像素参考的深度误差 ==\n");
    printf("%-6s %-12s %10s %12s %12s %12s\n", "res", "mode", "covered", "time_us", "max_err", "mean_err");
    for (int s = 0; s < 2; ++s) {
        int n = sizes[s];
        make_scene(tris, n);
        double *ref = malloc(sizeof(double) * (size_t)n * n);
        int ref_cov = ref_depth(tris, NTRI, n, ref);
        RTarget t;
        if (rt_init(&t, n, n) != 0) return 1;

        const struct { const char *name; RDepthMask m; } modes[2] = {
            {"write=1", {1, 1}},
            {"write=0", {1, 0}},
        };
        for (int mi = 0; mi < 2; ++mi) {
            /* 计时 */
            double t0 = now_s();
            for (int r = 0; r < reps[s]; ++r) { reset_target(&t); render(&t, tris, &modes[mi].m, 1); }
            double us = (now_s() - t0) * 1e6 / reps[s];
            /* 误差（write=0 时深度缓冲不被写，误差无意义，核对缓冲保持初始值） */
            int cov = 0;
            double maxe = 0.0, sume = 0.0;
            for (int i = 0; i < n * n; ++i) {
                if (t.z[i] < 1e30f) {
                    ++cov;
                    double e = fabs((double)t.z[i] - ref[i]);
                    if (e > maxe) maxe = e;
                    sume += e;
                }
            }
            if (mi == 1) {
                /* write=0：深度应完全未写，覆盖数以参考为准 */
                printf("%-6d %-12s %10d %12.1f %12s %12s\n", n, modes[mi].name,
                       ref_cov, us, "(depth intact)", cov == 0 ? "yes" : "no");
            } else {
                printf("%-6d %-12s %10d %12.1f %12.3g %12.3g\n", n, modes[mi].name,
                       cov, us, maxe, cov ? sume / cov : 0.0);
                if (cov != ref_cov) printf("  !! covered %d != reference %d\n", cov, ref_cov);
            }
        }
        rt_free(&t);
        free(ref);
    }

    printf("\n== 掩码关闭：旧接口 vs 新接口（覆盖像素 / 平均深度 / 耗时，应完全一致）==\n");
    printf("%-6s %-14s %10s %14s %12s\n", "res", "path", "covered", "mean_depth", "time_us");
    for (int s = 0; s < 2; ++s) {
        int n = sizes[s];
        make_scene(tris, n);
        RTarget t;
        if (rt_init(&t, n, n) != 0) return 1;
        RDepthMask off = {0, 0};
        for (int path = 0; path < 2; ++path) {
            double t0 = now_s();
            for (int r = 0; r < reps[s]; ++r) { reset_target(&t); render(&t, tris, &off, path); }
            double us = (now_s() - t0) * 1e6 / reps[s];
            int cov = 0;
            double sumz = 0.0;
            for (int i = 0; i < n * n; ++i) {
                if (t.z[i] < 1e30f) { ++cov; sumz += t.z[i]; }
            }
            printf("%-6d %-14s %10d %14.8f %12.1f\n", n,
                   path ? "ex(mask off)" : "rt_triangle", cov, cov ? sumz / cov : 0.0, us);
        }
        rt_free(&t);
    }
    return 0;
}
