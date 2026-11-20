#include "atlasmip.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

static const unsigned char C_R[3] = {255, 0, 0};
static const unsigned char C_G[3] = {0, 255, 0};
static const unsigned char C_B[3] = {0, 0, 255};
static const unsigned char C_W[3] = {255, 255, 255};

/* 4 个等宽竖条，条 i 填 colors[i]。 */
static void stripes(unsigned char *src, int Wd, int H, int bw) {
    const unsigned char *col[4] = {C_R, C_G, C_B, C_W};
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < Wd; ++x)
            memcpy(src + (y * Wd + x) * 3, col[x / bw], 3);
}

int main(void) {
    /* ===== 算例 A：8x8，4 个 2x8 竖条，4 层（手推 L2/L3） ===== */
    enum { WA = 8, HA = 8 };
    AmRect ra[4];
    for (int i = 0; i < 4; ++i) ra[i] = (AmRect){i * 2, 0, 2, HA};
    unsigned char srcA[WA * HA * 3];
    stripes(srcA, WA, HA, 2);

    AtlasMip a, a0;
    int bad = -99;
    CK(am_build(&a, srcA, ra, 4, WA, HA, 4, 1, &bad) == 0 && bad == -1);
    CK(am_build(&a0, srcA, ra, 4, WA, HA, 4, 0, NULL) == 0);

    /* 不做扩展：基底与输入逐字节一致；紧密排布时扩展结果也与输入一致。 */
    CK(memcmp(a0.extended, srcA, sizeof(srcA)) == 0);
    CK(memcmp(a.extended, srcA, sizeof(srcA)) == 0);

    /* 每块层链尺寸：2x8 -> 1x4 -> 1x2 -> 1x1，且各层保持纯色 */
    CK(a.chains[0].levels == 4 && a.chains[0].w[0] == 2 && a.chains[0].w[3] == 1);
    unsigned char px[3];
    CK(am_sample(&a, 0, 0.5, 0.5, 2, px) == 0 && memcmp(px, C_R, 3) == 0);
    CK(am_sample(&a, 1, 0.9, 0.1, 3, px) == 0 && memcmp(px, C_G, 3) == 0);
    CK(am_sample(&a, 2, 0.0, 0.0, 0, px) == 0 && memcmp(px, C_B, 3) == 0);
    CK(am_sample(&a, 3, 1.0, 1.0, 1, px) == 0 && memcmp(px, C_W, 3) == 0);

    /* L1（2x 降采样）：朴素链 4x4=16 像素，footprint 全在条内，零渗色 */
    AmReport rep;
    CK(am_measure(&a, 1, &rep) == 0);
    CK(rep.pixels == 16 && rep.bleed_px == 0 && rep.err_sum == 0 && rep.err_max == 0);

    /* L2（4x）手推：朴素链 2x2=4 像素。左列 (127,127,0) 中心落块1纯 C_G，
       单像素误差 |127|+|127|+0=255；右列 (127,127,255) 对块3纯 C_W，
       误差 128+128+0=256；每列 2 行，全部渗色。 */
    CK(am_measure(&a, 2, &rep) == 0);
    CK(rep.pixels == 4);
    CK(rep.bleed_px == 4);
    CK(rep.err_sum == (255L + 256L) * 2);   /* 1022 */
    CK(rep.err_max == 128);

    /* L3（8x）手推：朴素链 1x1，(127,127,127) 中心落块2纯 C_B，
       误差 127+127+|127-255|=382，单像素渗色。 */
    CK(am_measure(&a, 3, &rep) == 0);
    CK(rep.pixels == 1 && rep.bleed_px == 1);
    CK(rep.err_sum == 382L);
    CK(rep.err_max == 128);
    am_free(&a); am_free(&a0);

    /* ===== 算例 B：32x8，4 个 8x8 块，5 层（手推 L2 零渗色 / L4 渗色） ===== */
    enum { WB = 32, HB = 8 };
    AmRect rb[4];
    for (int i = 0; i < 4; ++i) rb[i] = (AmRect){i * 8, 0, 8, HB};
    unsigned char srcB[WB * HB * 3];
    stripes(srcB, WB, HB, 8);
    AtlasMip b;
    CK(am_build(&b, srcB, rb, 4, WB, HB, 5, 1, NULL) == 0);

    CK(am_measure(&b, 2, &rep) == 0);   /* 4x：朴素 8x2=16，footprint 不出块 */
    CK(rep.pixels == 16 && rep.bleed_px == 0 && rep.err_sum == 0);
    CK(am_measure(&b, 3, &rep) == 0);   /* 8x：朴素 4x1=4，footprint 恰好一块宽 */
    CK(rep.pixels == 4 && rep.bleed_px == 0);
    /* L4（16x）手推：朴素 2x1，两像素为 C_R/C_G、C_B/W 的均值，
       中心分别落块1(C_G)、块3(C_W)，每像素误差255/256。 */
    CK(am_measure(&b, 4, &rep) == 0);
    CK(rep.pixels == 2 && rep.bleed_px == 2);
    CK(rep.err_sum == 511 && rep.err_max == 128);

    /* 取样裁剪：越界 UV 被钳到块边，结果等于边界像素，绝不取到邻居色 */
    unsigned char edge0[3], edge1[3], clip_lo[3], clip_hi[3];
    CK(am_sample(&b, 0, 0.0, 0.5, 2, edge0) == 0);
    CK(am_sample(&b, 0, 1.0, 0.5, 2, edge1) == 0);
    CK(am_sample(&b, 0, -5.0, 0.5, 2, clip_lo) == 0);
    CK(am_sample(&b, 0, 9.0, 0.5, 2, clip_hi) == 0);
    CK(memcmp(clip_lo, edge0, 3) == 0 && memcmp(clip_hi, edge1, 3) == 0);
    CK(memcmp(edge0, C_R, 3) == 0);      /* 钳制后仍是本块红色，不是邻块绿色 */
    am_free(&b);

    /* ===== 留白扩展：块外像素贴边，关闭扩展时保持原样 ===== */
    enum { WG = 10, HG = 2 };
    AmRect rg[1] = {{1, 0, 2, 2}};
    unsigned char srcG[WG * HG * 3], mag[3] = {255, 0, 255};
    for (int i = 0; i < WG * HG; ++i) memcpy(srcG + i * 3, mag, 3);
    memcpy(srcG + (0 * WG + 1) * 3, C_G, 3);
    memcpy(srcG + (0 * WG + 2) * 3, C_G, 3);
    memcpy(srcG + (1 * WG + 1) * 3, C_G, 3);
    memcpy(srcG + (1 * WG + 2) * 3, C_G, 3);
    AtlasMip g1, g0;
    CK(am_build(&g1, srcG, rg, 1, WG, HG, 2, 1, NULL) == 0);
    CK(am_build(&g0, srcG, rg, 1, WG, HG, 2, 0, NULL) == 0);
    CK(memcmp(g0.extended, srcG, sizeof(srcG)) == 0);                 /* 不扩展：原样 */
    CK(memcmp(g1.extended + (0 * WG + 0) * 3, C_G, 3) == 0);            /* 左贴边 */
    CK(memcmp(g1.extended + (0 * WG + 9) * 3, C_G, 3) == 0);            /* 右贴边 */
    CK(memcmp(g0.extended + (0 * WG + 0) * 3, mag, 3) == 0);          /* 不扩展留白仍是品红 */
    CK(memcmp(g1.extended + (0 * WG + 1) * 3, C_G, 3) == 0);            /* 块内不动 */
    am_free(&g1); am_free(&g0);

    /* ===== 错误处理：层数为零、块越界并指出序号 ===== */
    CK(am_build(&a, srcA, ra, 4, WA, HA, 0, 1, NULL) == -2);
    AmRect badrect[3] = {{0, 0, 2, HA}, {6, 0, 4, HA}, {0, 0, 2, HA}}; /* 第1块 x+w=10>8 */
    bad = -99;
    CK(am_build(&a, srcA, badrect, 3, WA, HA, 2, 1, &bad) == -3 && bad == 1);
    AmRect badrect0[2] = {{-1, 0, 2, HA}, {0, 0, 2, HA}};
    CK(am_build(&a, srcA, badrect0, 2, WA, HA, 2, 1, &bad) == -3 && bad == 0);
    CK(am_build(NULL, srcA, ra, 4, WA, HA, 2, 1, NULL) == -1);
    CK(am_index_build(ra, 4, WA, HA, NULL, NULL) == -1);

    /* 取样参数非法 */
    AtlasMip ok;
    CK(am_build(&ok, srcA, ra, 4, WA, HA, 4, 1, NULL) == 0);
    CK(am_sample(&ok, 4, 0.5, 0.5, 0, px) == -3);    /* 块序号越界 */
    CK(am_sample(&ok, -1, 0.5, 0.5, 0, px) == -3);
    CK(am_sample(&ok, 0, 0.5, 0.5, 4, px) == -2);    /* 层越界 */
    CK(am_measure(&ok, 9, &rep) == -2);
    am_free(&ok);

    /* ===== 块边界索引：构建、序列化、读回比对 ===== */
    AmIndex idx;
    CK(am_index_build(rb, 4, WB, HB, &idx, &bad) == 0 && bad == -1);
    CK(am_index_write("build/atlasmip.idx", &idx) == 0);
    AmIndex in;
    CK(am_index_read("build/atlasmip.idx", &in) == 0);
    CK(in.n == 4 && in.w == WB && in.h == HB);
    int same = 1;
    for (int i = 0; i < 4; ++i)
        if (memcmp(&in.rects[i], &rb[i], sizeof(AmRect)) != 0) same = 0;
    CK(same);
    am_index_free(&idx); am_index_free(&in);
    CK(am_index_read("build/no-such-file.idx", &in) == -4);

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
