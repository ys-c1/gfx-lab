#include "atlasmip.h"
#include <stdio.h>
#include <stdlib.h>

static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

static unsigned char *atlas_src(int w, int h) {
    unsigned char *s = (unsigned char*)malloc((size_t)w * h * 3);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            unsigned char *p = s + (y * w + x) * 3;
            p[0] = (unsigned char)(x + y * w);   /* R = 线性编号 */
            p[1] = 0; p[2] = 0;
        }
    return s;
}

/* 纸笔推导对照：4x4 图集，4 个 2x2 块，R 通道为像素线性编号。
 *
 * 块0 像素 R = {0,1,4,5}   -> 独立链 L1 R = (0+1+4+5)/4 = 2
 * 块1 像素 R = {2,3,6,7}   -> 独立链 L1 R = (2+3+6+7)/4 = 4
 * 块2 像素 R = {8,9,12,13} -> 独立链 L1 R = (8+9+12+13)/4 = 10
 * 块3 像素 R = {10,11,14,15}-> 独立链 L1 R = (10+11+14+15)/4 = 12
 *
 * 老整图链 L1 取全局 2x2 核，恰好与块边界对齐，数值相同（无串色）。
 */
static void test_paper_values(void) {
    enum { W = 4, H = 4 };
    unsigned char *src = atlas_src(W, H);
    AmBlock blk[4] = {
        {0, 0, 2, 2}, {2, 0, 2, 2}, {0, 2, 2, 2}, {2, 2, 2, 2}
    };
    AtlasMip *am = NULL;
    CK(am_build(&am, src, W, H, blk, 4, 1) == AM_OK);

    /* 层数：2x2 -> L0(2x2) + L1(1x1)，共 2 层 */
    for (int i = 0; i < 4; ++i) CK(am_level_count(am, i) == 2);
    int lw = 0, lh = 0;
    CK(am_level_info(am, 0, 0, &lw, &lh) == AM_OK && lw == 2 && lh == 2);
    CK(am_level_info(am, 3, 1, &lw, &lh) == AM_OK && lw == 1 && lh == 1);

    static const int paper_l1[4] = {2, 4, 10, 12};
    for (int i = 0; i < 4; ++i) {
        unsigned char px[3];
        CK(am_sample(am, i, 1, 0.0, 0.0, px) == AM_OK);
        CK(px[0] == paper_l1[i]);            /* 纸笔值 */
        CK(px[1] == 0 && px[2] == 0);
    }

    /* L0 取样与源像素一致（块内坐标） */
    unsigned char p0[3];
    CK(am_sample(am, 1, 0, 0.5, 0.0, p0) == AM_OK);
    CK(p0[0] == 3);                          /* 全局 (3,0) R=3 */

    /* 每块层链内存：L0 2*2*3=12 + L1 1*1*3=3 = 15 字节；4 块合计 60 */
    for (int i = 0; i < 4; ++i) CK(am_chain_bytes(am, i) == 15);
    CK(am_total_chain_bytes(am) == 60);
    /* 整图老链：4*4*3 + 2*2*3 + 1*1*3 = 48+12+3 = 63 */
    CK(am_full_chain_bytes(am) == 63);
    CK(am_block_count(am) == 4);
    CK(am_uses_block_chains(am) == 1);

    am_free(am);
    free(src);
}

/* 非 2 次幂块宽（3+5=8）：整图降采样核跨过块边界 -> 老路径串色，
 * 新逐块链不串色；同位置两路径 L1 必须出现差异。 */
static void test_bleed_and_stats(void) {
    enum { W = 8, H = 2 };
    unsigned char *src = atlas_src(W, H);
    AmBlock blk[2] = {{0, 0, 3, 2}, {3, 0, 5, 2}};
    AtlasMip *am = NULL;
    CK(am_build(&am, src, W, H, blk, 2, 1) == AM_OK);

    /* 块0 独立链 L1：宽 3 经整数折半直接变 1（3->1），单纹素取
     * 像素 R {0,1,8,9}（边缘 x1 复制）：(0+1+8+9)/4 = 4。纸笔值。 */
    unsigned char q0[3];
    CK(am_sample(am, 0, 1, 0.0, 0.0, q0) == AM_OK && q0[0] == 4);

    /* 块1 (5x2) 独立链 L1 宽 2，像素 R 行0 {3,4,5,6,7} 行1 {11..15}：
     * x0=(3+4+11+12)/4=7，x1=(5+6+13+14)/4=9。纸笔值。 */
    unsigned char r0[3], r1[3];
    CK(am_sample(am, 1, 1, 0.0, 0.0, r0) == AM_OK && r0[0] == 7);
    CK(am_sample(am, 1, 1, 0.5, 0.0, r1) == AM_OK && r1[0] == 9);

    /* 串色对照：块1 u=0 处新链得 7；同位置映射到老整图链 L1 x=1，
     * 降采样核覆盖全局 x=2,3，混入块0 的像素 2/10 -> (2+3+10+11)/4=6。
     * 逐块链隔离了跨块核，新老误差 = |7-6| = 1。纸笔值。 */
    unsigned char nb[3];
    CK(am_sample_scaled(am, 1, AM_SCALE_2X, 0.0, 0.0, nb) == AM_OK);
    CK(nb[0] == 7);
    const AmScaleStat *s2 = am_scale_stats(am, AM_SCALE_2X);
    CK(s2->samples == 1);
    CK(s2->abs_err_sum == 1);                /* 单通道差 |7-6|，G/B 为 0 */
    CK(s2->max_err == 1);

    /* 1x 档采样若干次 */
    for (int i = 0; i < 5; ++i) {
        unsigned char px[3];
        double u = (i % 3) * 0.25;
        CK(am_sample_scaled(am, i % 2, AM_SCALE_1X, u, 0.5, px) == AM_OK);
    }
    const AmScaleStat *s1 = am_scale_stats(am, AM_SCALE_1X);
    CK(s1->samples == 5);
    CK(s1->max_err >= 0);

    /* 层数（整数折半，奇数维一跳到 floor(n/2)）：
     * 块0 宽3高2 -> 3,1 与 2,1 -> 2 层，L1=1x1；
     * 块1 宽5高2 -> 5,2,1 与 2,1,1 -> 3 层，L1=2x1、L2=1x1。 */
    CK(am_level_count(am, 0) == 2);
    CK(am_level_count(am, 1) == 3);
    int lw = 0, lh = 0;
    CK(am_level_info(am, 0, 1, &lw, &lh) == AM_OK && lw == 1 && lh == 1);
    CK(am_level_info(am, 1, 1, &lw, &lh) == AM_OK && lw == 2 && lh == 1);
    CK(am_level_info(am, 1, 2, &lw, &lh) == AM_OK && lw == 1 && lh == 1);

    am_free(am);
    free(src);
}

/* 越界访问检出并报编号：坏块号、坏层号、坏 UV、构建期坏矩形。 */
static void test_oob(void) {
    enum { W = 4, H = 4 };
    unsigned char *src = atlas_src(W, H);
    AmBlock blk[2] = {{0, 0, 2, 2}, {2, 2, 2, 2}};
    AtlasMip *am = NULL;
    CK(am_build(&am, src, W, H, blk, 2, 1) == AM_OK);
    const AmOob *o;
    unsigned char px[3];

    CK(am_sample(am, 7, 0, 0.0, 0.0, px) == AM_ERR_BLOCK);
    o = am_last_oob(am); CK(o && o->code == AM_ERR_BLOCK && o->block == 7);

    CK(am_sample(am, 0, 9, 0.0, 0.0, px) == AM_ERR_LEVEL);
    o = am_last_oob(am); CK(o && o->code == AM_ERR_LEVEL && o->block == 0 && o->level == 9);

    CK(am_sample(am, 1, 0, 1.0, 0.0, px) == AM_ERR_UV);   /* u=1 越界，不钳制 */
    o = am_last_oob(am); CK(o && o->code == AM_ERR_UV && o->block == 1 && o->level == 0);

    CK(am_sample(am, 0, 0, -0.1, 0.0, px) == AM_ERR_UV);
    CK(am_level_info(am, 9, 0, NULL, NULL) == AM_ERR_BLOCK);

    am_free(am);

    /* 构建期坏矩形：块 1 超出图集右边；对象保留，编号可查。 */
    AmBlock bad[2] = {{0, 0, 2, 2}, {3, 0, 2, 2}};
    AtlasMip *broken = NULL;
    CK(am_build(&broken, src, W, H, bad, 2, 1) == AM_ERR_BLOCK_RECT);
    o = am_last_oob(broken);
    CK(o && o->code == AM_ERR_BLOCK_RECT && o->block == 1);
    am_free(broken);

    free(src);
}

/* 新逻辑关闭：走老整图路径，与独立构建的整图参考链在同位置字节一致；
 * 且不分配任何块层链内存。 */
static void test_legacy_byte_identical(void) {
    enum { W = 8, H = 4 };
    unsigned char *src = atlas_src(W, H);
    AmBlock blk[3] = {{0, 0, 4, 2}, {4, 0, 4, 2}, {0, 2, 8, 2}};

    AtlasMip *legacy = NULL, *modern = NULL;
    CK(am_build(&legacy, src, W, H, blk, 3, 0) == AM_OK);
    CK(am_build(&modern, src, W, H, blk, 3, 1) == AM_OK);
    CK(am_uses_block_chains(legacy) == 0);
    CK(am_total_chain_bytes(legacy) == 0);   /* 老模式无块层链 */

    /* 老模式层表即整图层表 */
    CK(am_level_count(legacy, 0) == am_level_count(legacy, 2));
    int lw, lh;
    CK(am_level_info(legacy, 1, 1, &lw, &lh) == AM_OK && lw == 4 && lh == 2);

    /* 两模式 L0（及老模式全部层）同块同 u/v 取样字节一致 */
    const double uv[4][2] = {{0,0},{0.25,0.5},{0.5,0.25},{0.99,0.99}};
    for (int b = 0; b < 3; ++b)
        for (int k = 0; k < 4; ++k) {
            unsigned char a[3], c[3];
            CK(am_sample(legacy, b, 0, uv[k][0], uv[k][1], a) == AM_OK);
            CK(am_sample(modern, b, 0, uv[k][0], uv[k][1], c) == AM_OK);
            CK(a[0] == c[0] && a[1] == c[1] && a[2] == c[2]);
        }

    /* 老模式 scaled 采样只计数、不累加误差（无新链可对照） */
    unsigned char px[3];
    CK(am_sample_scaled(legacy, 0, AM_SCALE_1X, 0.5, 0.5, px) == AM_OK);
    CK(am_scale_stats(legacy, AM_SCALE_1X)->samples == 1);
    CK(am_scale_stats(legacy, AM_SCALE_1X)->abs_err_sum == 0);

    am_free(legacy);
    am_free(modern);
    free(src);
}

int main(void) {
    test_paper_values();
    test_bleed_and_stats();
    test_oob();
    test_legacy_byte_identical();
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
