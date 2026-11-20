#define _POSIX_C_SOURCE 199309L
#include "mip.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

/* 一轮：以 lod_center 为中心扫过层边界，比较 混合关(最近层) 与 混合开(三线性) */
static void run_round(const MipChain *c, double lod_center, const char *tag) {
    enum { GRID = 64, STEPS = 64, REPS = 200 };
    static unsigned char prev[2][GRID][GRID][3];
    long long reads[2] = {0, 0};
    int max_jump[2] = {0, 0};
    double ms[2];
    const double per = (double)REPS * STEPS * GRID * GRID; /* 每种模式的采样次数 */

    for (int mode = 0; mode < 2; ++mode) { /* 0 = 混合关, 1 = 混合开 */
        double t0 = now_ms();
        for (int rep = 0; rep < REPS; ++rep) {
            int jump = 0;
            for (int s = 0; s < STEPS; ++s) {
                double lod = lod_center - 0.5 + (double)s / (STEPS - 1);
                int lo; double w;
                if (mip_lod_split(c, lod, &lo, &w, NULL) != 0) exit(2);
                for (int i = 0; i < GRID; ++i)
                    for (int j = 0; j < GRID; ++j) {
                        double u = (i + 0.5) / GRID, v = (j + 0.5) / GRID;
                        unsigned char px[3];
                        MipTrace tr;
                        if (!mode) {
                            /* 旧行为：最近整数层 */
                            int lvl = (int)(lod + 0.5);
                            if (lvl > c->levels - 1) lvl = c->levels - 1;
                            mip_sample_trilinear(c, lvl, 0.0, u, v, 0, px, &tr, NULL);
                        } else {
                            mip_sample_trilinear(c, lo, w, u, v, 1, px, &tr, NULL);
                        }
                        if (rep == REPS - 1) {
                            reads[mode] += tr.texel_reads;
                            if (s > 0)
                                for (int ch = 0; ch < 3; ++ch) {
                                    int d = abs(px[ch] - (int)prev[mode][i][j][ch]);
                                    if (d > jump) jump = d;
                                }
                            memcpy(prev[mode][i][j], px, 3);
                        }
                    }
            }
            if (rep == REPS - 1 && jump > max_jump[mode]) max_jump[mode] = jump;
        }
        ms[mode] = now_ms() - t0;
    }
    printf("%s (lod ~%.1f, %dx%d 像素 x %d 步):\n", tag, lod_center, GRID, GRID, STEPS);
    printf("  混合关(最近层): 跳变 %3d 级 | 采样点 %lld | %.1f ms (%.0f ns/次)\n",
           max_jump[0], reads[0], ms[0], ms[0] * 1e6 / per);
    printf("  混合开(三线性): 跳变 %3d 级 | 采样点 %lld | %.1f ms (%.0f ns/次)\n",
           max_jump[1], reads[1], ms[1], ms[1] * 1e6 / per);
}

int main(void) {
    enum { W = 256, H = 256 };
    unsigned char *src = (unsigned char*)malloc((size_t)W * H * 3);
    /* 确定性白噪声：各层滤波后仍有差异，最近层切换时才会暴露跳变 */
    unsigned int rng = 0x9e3779b9u;
    for (int i = 0; i < W * H * 3; ++i) {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        src[i] = (unsigned char)(rng >> 24);
    }
    MipChain c;
    if (mip_build(&c, src, W, H) != 0) return 1;

    run_round(&c, 1.5, "2 倍缩小"); /* 扫过 1 层与 2 层的边界 */
    run_round(&c, 3.5, "8 倍缩小"); /* 扫过 3 层与 4 层的边界 */

    mip_free(&c);
    free(src);
    return 0;
}
