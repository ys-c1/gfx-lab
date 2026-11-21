#include "atlasmip.h"
#include <stdio.h>
#include <stdlib.h>

/*
 * 演示：8x4 图集（R = 像素线性编号，G/B=0），3 个块。
 * 分别构建 新路径(逐块层链) 与 老路径(整图层链) 两套对象，
 * 按块坐标取样并统计两档倍率下的采样次数与新老误差。
 *
 * 纸笔推导（R 通道，整数折半，奇数维一跳到 floor(n/2)）：
 *   块0 (3x2) 独立链 L1 为 1x1：像素 {0,1,8,9} -> (0+1+8+9)/4 = 4
 *   块1 (5x2) 独立链 L1 为 2x1：
 *     x0 = (3+4+11+12)/4 = 7,  x1 = (5+6+13+14)/4 = 9
 *   老整图链 L1 在块1 左缘的核覆盖全局 x=2,3，混入块0 像素 2/10 -> 6，
 *     而逐块链同位置为 7，单通道误差 1（降采样核跨块边界的串色量）。
 *   层链内存 = 各层 w*h*3 之和（不含指针/表头）。
 */
#define W 8
#define H 4

static unsigned char *make_src(void) {
    unsigned char *s = (unsigned char*)malloc(W * H * 3);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            unsigned char *p = s + (y * W + x) * 3;
            p[0] = (unsigned char)(x + y * W); p[1] = 0; p[2] = 0;
        }
    return s;
}

static const char *scale_name(AmScale sc) { return sc == AM_SCALE_1X ? "1x" : "2x"; }

static void report(const char *tag, AtlasMip *am, const AmBlock *blk, int n) {
    printf("==== %s (use_block_chains=%d) ====\n", tag, am_uses_block_chains(am));
    long chain_total = am_total_chain_bytes(am);
    long full_bytes = am_full_chain_bytes(am);
    printf("blocks=%d  full-chain bytes=%ld  block-chain total bytes=%ld (单独统计)\n",
           am_block_count(am), full_bytes, chain_total);

    for (int b = 0; b < n; ++b) {
        int lv = am_level_count(am, b);
        printf("block %d @(%d,%d) size %dx%d  levels=%d  chain bytes=%ld\n",
               b, blk[b].x, blk[b].y, blk[b].w, blk[b].h, lv, am_chain_bytes(am, b));
        printf("  layer table:");
        for (int l = 0; l < lv; ++l) {
            int lw, lh;
            am_level_info(am, b, l, &lw, &lh);
            printf(" L%d=%dx%d", l, lw, lh);
        }
        printf("\n");
    }

    /* 两档倍率，每块 4x4=16 个块内坐标采样 */
    for (int scI = 0; scI < AM_SCALE_N; ++scI) {
        AmScale sc = (AmScale)scI;
        for (int b = 0; b < n; ++b)
            for (int iy = 0; iy < 4; ++iy)
                for (int ix = 0; ix < 4; ++ix) {
                    double u = (ix + 0.5) / 4.0, v = (iy + 0.5) / 4.0;
                    unsigned char px[3];
                    am_sample_scaled(am, b, sc, u, v, px);
                }
        const AmScaleStat *st = am_scale_stats(am, sc);
        printf("scale %s: samples=%ld  abs_err_sum=%lu  max_err=%d\n",
               scale_name(sc), st->samples, st->abs_err_sum, st->max_err);
    }
    printf("\n");
}

int main(void) {
    AmBlock blk[3] = {
        {0, 0, 3, 2},   /* 块0：非 2 次幂宽，边界会被整图降采样核跨过 */
        {3, 0, 5, 2},   /* 块1 */
        {0, 2, 8, 2}    /* 块2：整行 */
    };
    unsigned char *src = make_src();

    AtlasMip *modern = NULL, *legacy = NULL;
    if (am_build(&modern, src, W, H, blk, 3, 1) != AM_OK) return 1;
    if (am_build(&legacy, src, W, H, blk, 3, 0) != AM_OK) return 1;

    report("NEW per-block chains", modern, blk, 3);
    report("LEGACY whole-atlas chain", legacy, blk, 3);

    /* 纸笔值对照 */
    unsigned char px[3];
    am_sample(modern, 0, 1, 0.0, 0.0, px);
    printf("paper check block0 L1 (1x1): got R=%d, paper=4\n", px[0]);
    am_sample(modern, 1, 1, 0.0, 0.0, px);
    printf("paper check block1 L1 x0:    got R=%d, paper=7\n", px[0]);
    am_sample(modern, 1, 1, 0.5, 0.0, px);
    printf("paper check block1 L1 x1:    got R=%d, paper=9\n", px[0]);

    /* 越界访问检出与编号 */
    int rc = am_sample(modern, 9, 0, 0.0, 0.0, px);
    const AmOob *o = am_last_oob(modern);
    printf("oob block: rc=%d code=%d block=%d level=%d\n",
           rc, o->code, o->block, o->level);
    rc = am_sample(modern, 0, 5, 0.0, 0.0, px);
    o = am_last_oob(modern);
    printf("oob level: rc=%d code=%d block=%d level=%d\n",
           rc, o->code, o->block, o->level);
    rc = am_sample(modern, 1, 0, 1.5, 0.0, px);
    o = am_last_oob(modern);
    printf("oob uv:    rc=%d code=%d block=%d level=%d\n",
           rc, o->code, o->block, o->level);

    am_free(modern);
    am_free(legacy);
    free(src);
    return 0;
}
