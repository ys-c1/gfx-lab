#define _POSIX_C_SOURCE 200809L
#include "atlasmip.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RUNS 10

static long now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000000000L + ts.tv_nsec;
}

static unsigned char rnd(unsigned s) {           /* 确定性 8bit 颜色分量 */
    s ^= (unsigned)(s << 13); s ^= (s >> 17); s ^= (unsigned)(s << 5);
    return (unsigned char)(s & 0xff);
}

/* 压力图集：W*H，边长 BS 的纯色块网格。 */
static void grid_atlas(int W, int H, int BS, unsigned char *src, AmRect *rects, int *nb) {
    int k = 0;
    for (int by = 0; by < H / BS; ++by)
        for (int bx = 0; bx < W / BS; ++bx, ++k) {
            unsigned seed = (unsigned)(k * 2654435761u + 1u);
            unsigned char c[3] = {rnd(seed), rnd(seed ^ 0x55aa), rnd(seed ^ 0xaa55)};
            rects[k] = (AmRect){bx * BS, by * BS, BS, BS};
            for (int y = 0; y < BS; ++y)
                for (int x = 0; x < BS; ++x)
                    memcpy(src + ((by * BS + y) * W + (bx * BS + x)) * 3, c, 3);
        }
    *nb = k;
}

static void report(const AtlasMip *am, int level, const char *tag) {
    AmReport rep;
    long t0 = now_ns();
    if (am_measure(am, level, &rep) != 0) { printf("measure failed at %s\n", tag); return; }
    long dt = now_ns() - t0;
    printf("  %-4s L%d: compared=%d bleed_px=%d err_sum=%ld err_max=%d measure=%.3f ms\n",
           tag, level, rep.pixels, rep.bleed_px, rep.err_sum, rep.err_max, dt / 1e6);
}

int main(void) {
    /* ===== 手推算例：4x1 竖条 R G B W，L1 朴素像素两两混合、块链保持纯色 ===== */
    unsigned char toy[4 * 3];
    memcpy(toy + 0, (unsigned char[]){255, 0, 0}, 3);
    memcpy(toy + 3, (unsigned char[]){0, 255, 0}, 3);
    memcpy(toy + 6, (unsigned char[]){0, 0, 255}, 3);
    memcpy(toy + 9, (unsigned char[]){255, 255, 255}, 3);
    AmRect tr[2] = {{0, 0, 2, 1}, {2, 0, 2, 1}};
    AtlasMip dm;
    if (am_build(&dm, toy, tr, 2, 4, 1, 3, 1, NULL) != 0) return 1;
    AmReport trp;
    am_measure(&dm, 1, &trp);
    printf("toy 4x1 at L1 (2x): compared=%d bleed=%d err_sum=%ld max=%d (centers land inside blocks)\n",
           trp.pixels, trp.bleed_px, trp.err_sum, trp.err_max);
    am_measure(&dm, 2, &trp);
    printf("toy 4x1 at L2 (4x): compared=%d bleed=%d err_sum=%ld max=%d (naive mixes R/G/B/W, block chain stays pure)\n",
           trp.pixels, trp.bleed_px, trp.err_sum, trp.err_max);
    am_free(&dm);

    /* ===== 压力图集 1：256x256，16x16 块网格，对齐边界，用于计时 ===== */
    enum { W = 256, H = 256, BS = 16, NB = (W / BS) * (H / BS) };
    unsigned char *src = (unsigned char*)malloc((size_t)W * H * 3);
    AmRect *rects = (AmRect*)malloc((size_t)NB * sizeof(AmRect));
    int nb;
    grid_atlas(W, H, BS, src, rects, &nb);
    printf("\nstress atlas %dx%d, %d blocks of %dx%d, runs %d\n", W, H, nb, BS, BS, RUNS);

    double tot = 0.0;
    AtlasMip am;
    for (int i = 0; i < RUNS; ++i) {
        long t0 = now_ns();
        if (am_build(&am, src, rects, nb, W, H, 9, 1, NULL) != 0) return 1;
        tot += (double)(now_ns() - t0);
        if (i + 1 < RUNS) am_free(&am);
    }
    printf("build avg %.3f ms  [stage1 edge-extend %.3f ms | stage2 %d block chains %.3f ms | naive chain %.3f ms]\n",
           tot / RUNS / 1e6, am.ns_extend / 1e6, nb, am.ns_chains / 1e6, am.ns_naive / 1e6);

    AtlasMip am0;
    if (am_build(&am0, src, rects, nb, W, H, 9, 0, NULL) != 0) return 1;
    printf("extend=0 base identical to source: %s\n",
           memcmp(am0.extended, src, (size_t)W * H * 3) == 0 ? "yes" : "NO");
    am_free(&am0);

    /* 块宽 16 是 4 和 16 的倍数：4x/16x 边界对齐，无渗色；32x 才跨块 */
    printf("aligned 16px blocks (footprint hits block edge exactly):\n");
    report(&am, 2, "4x");
    report(&am, 4, "16x");
    report(&am, 5, "32x");

    /* ===== 压力图集 2：块宽 6（非 4 倍数），4x 降采样必然跨块渗色 ===== */
    enum { W2 = 252, H2 = 16, BW2 = 6, NB2 = 42 };   /* 42*6=252，块铺满 */
    unsigned char *src2 = (unsigned char*)malloc((size_t)W2 * H2 * 3);
    AmRect rects2[NB2];
    for (int i = 0; i < NB2; ++i) {
        unsigned seed = (unsigned)(i * 40503u + 7u);
        unsigned char c[3] = {rnd(seed), rnd(seed ^ 0x33cc), rnd(seed ^ 0xcc33)};
        rects2[i] = (AmRect){i * BW2, 0, BW2, H2};
        for (int y = 0; y < H2; ++y)
            for (int x = 0; x < BW2; ++x)
                memcpy(src2 + (y * W2 + i * BW2 + x) * 3, c, 3);
    }
    AtlasMip am2;
    if (am_build(&am2, src2, rects2, NB2, W2, H2, 8, 1, NULL) != 0) return 1;
    printf("\nmisaligned atlas %dx%d, %d vertical blocks of width %d:\n", W2, H2, NB2, BW2);
    report(&am2, 2, "4x");    /* 6 不是 4 的倍数，footprint 跨块，出现非零渗色 */
    report(&am2, 4, "16x");   /* 16x 下跨得更远，渗色更多 */

    /* ===== 按块裁剪取样：越界 UV 只取块边，绝不读邻居 ===== */
    unsigned char inb[3], clip_lo[3], clip_hi[3];
    am_sample(&am, 20, 0.0, 0.5, 3, inb);
    am_sample(&am, 20, -10.0, 0.5, 3, clip_lo);
    am_sample(&am, 20, 10.0, 0.5, 3, clip_hi);
    printf("\nclipped samples match block edge: %s\n",
           memcmp(inb, clip_lo, 3) == 0 && memcmp(inb, clip_hi, 3) == 0 ? "yes" : "NO");

    /* ===== 块边界写进索引并落盘、读回 ===== */
    if (am_index_write("demo.idx", &am.idx) != 0) return 1;
    AmIndex in;
    if (am_index_read("demo.idx", &in) != 0) return 1;
    printf("index written: %d rects on %dx%d, rect[17]=(%d,%d,%d,%d)\n",
           in.n, in.w, in.h, in.rects[17].x, in.rects[17].y, in.rects[17].w, in.rects[17].h);
    am_index_free(&in);

    am_free(&am); am_free(&am2);
    free(src); free(src2); free(rects);
    return 0;
}
