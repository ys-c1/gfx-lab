#include "mip.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

/* ---- 独立参考采样器：故意用另一套写法（四点权重直接展开），用来对账 ---- */
static void ref_bilinear(const MipChain *c, int level, double u, double v, double out[3]) {
    int w = c->w[level], h = c->h[level];
    double x = u * w - 0.5, y = v * h - 0.5;
    if (x < 0.0) x = 0.0;
    if (x > w - 1.0) x = w - 1.0;
    if (y < 0.0) y = 0.0;
    if (y > h - 1.0) y = h - 1.0;
    int x0 = (int)x, y0 = (int)y;
    int x1 = x0 + 1 < w ? x0 + 1 : w - 1;
    int y1 = y0 + 1 < h ? y0 + 1 : h - 1;
    double fx = x - x0, fy = y - y0;
    double wgt[4] = { (1-fx)*(1-fy), fx*(1-fy), (1-fx)*fy, fx*fy };
    const unsigned char *t[4] = {
        c->pix[level] + (y0*w + x0)*3, c->pix[level] + (y0*w + x1)*3,
        c->pix[level] + (y1*w + x0)*3, c->pix[level] + (y1*w + x1)*3
    };
    for (int ch = 0; ch < 3; ++ch) {
        double s = 0.0;
        for (int k = 0; k < 4; ++k) s += wgt[k] * t[k][ch];
        out[ch] = s;
    }
}
static unsigned char ref_byte(double x) {
    if (x <= 0.0) return 0;
    if (x >= 255.0) return 255;
    return (unsigned char)(x + 0.5);
}
static void ref_trilinear(const MipChain *c, double lod, double u, double v,
                          unsigned char out[3]) {
    if (lod < 0.0) lod = 0.0;
    double max_lod = (double)(c->levels - 1);
    if (lod > max_lod) lod = max_lod;
    int lo = (int)floor(lod);
    double w = lod - lo;
    if (lo >= c->levels - 1) { lo = c->levels - 1; w = 0.0; }
    double a[3], b[3];
    ref_bilinear(c, lo, u, v, a);
    if (w > 0.0) ref_bilinear(c, lo + 1, u, v, b); else memcpy(b, a, sizeof a);
    for (int ch = 0; ch < 3; ++ch) out[ch] = ref_byte(a[ch] * (1.0 - w) + b[ch] * w);
}

static void test_validation(const MipChain *c) {
    unsigned char px[3];
    const char *f = NULL;
    CK(mip_sample_trilinear(c, -1, 0.0, 0.5, 0.5, 1, px, NULL, &f) == -1 && f && !strcmp(f, "level_lo"));
    CK(mip_sample_trilinear(c, c->levels, 0.0, 0.5, 0.5, 1, px, NULL, &f) == -1 && f && !strcmp(f, "level_lo"));
    CK(mip_sample_trilinear(c, 0, -0.01, 0.5, 0.5, 1, px, NULL, &f) == -1 && f && !strcmp(f, "weight"));
    CK(mip_sample_trilinear(c, 0, 1.01, 0.5, 0.5, 1, px, NULL, &f) == -1 && f && !strcmp(f, "weight"));
    CK(mip_sample_trilinear(c, 0, NAN, 0.5, 0.5, 1, px, NULL, &f) == -1 && f && !strcmp(f, "weight"));
    CK(mip_sample_trilinear(c, 0, INFINITY, 0.5, 0.5, 1, px, NULL, &f) == -1 && f && !strcmp(f, "weight"));
    /* 层数不足：最后一层还要求混合 */
    CK(mip_sample_trilinear(c, c->levels - 1, 0.5, 0.5, 0.5, 1, px, NULL, &f) == -1 && f && !strcmp(f, "levels"));
    /* 最后一层 weight==0 合法 */
    CK(mip_sample_trilinear(c, c->levels - 1, 0.0, 0.5, 0.5, 1, px, NULL, &f) == 0);
    CK(mip_sample_trilinear(c, 0, 0.5, NAN, 0.5, 1, px, NULL, &f) == -1 && f && !strcmp(f, "u"));
    CK(mip_sample_trilinear(c, 0, 0.5, 0.5, INFINITY, 1, px, NULL, &f) == -1 && f && !strcmp(f, "v"));
    /* 导数非有限 */
    CK(mip_lod_from_derivs(c, NAN, 0, 0, 0, &f) == -1.0 && f && !strcmp(f, "dudx"));
    CK(mip_lod_from_derivs(c, 0, INFINITY, 0, 0, &f) == -1.0 && f && !strcmp(f, "dudy"));
    CK(mip_lod_from_derivs(c, 0, 0, -INFINITY, 0, &f) == -1.0 && f && !strcmp(f, "dvdx"));
    CK(mip_lod_from_derivs(c, 0, 0, 0, NAN, &f) == -1.0 && f && !strcmp(f, "dvdy"));
    /* lod 非有限 */
    int lo; double w;
    CK(mip_lod_split(c, NAN, &lo, &w, &f) == -1 && f && !strcmp(f, "lod"));
    CK(mip_lod_split(c, INFINITY, &lo, &w, &f) == -1 && f && !strcmp(f, "lod"));
}

static void test_lod(const MipChain *c, int w0) {
    const char *f = NULL;
    /* 2 倍缩小：每像素走 2 纹素 -> lod 1；8 倍 -> lod 3 */
    double lod2 = mip_lod_from_derivs(c, 2.0 / w0, 0.0, 0.0, 2.0 / w0, &f);
    CK(fabs(lod2 - 1.0) < 1e-9);
    double lod8 = mip_lod_from_derivs(c, 8.0 / w0, 0.0, 0.0, 8.0 / w0, &f);
    CK(fabs(lod8 - 3.0) < 1e-9);
    /* 放大时钳到 0，超过链尾钳到 levels-1 */
    CK(mip_lod_from_derivs(c, 0.1 / w0, 0, 0, 0.1 / w0, &f) == 0.0);
    CK(mip_lod_from_derivs(c, 1e6, 0, 0, 1e6, &f) == (double)(c->levels - 1));
    int lo; double w;
    CK(mip_lod_split(c, 2.25, &lo, &w, &f) == 0 && lo == 2 && fabs(w - 0.25) < 1e-12);
    CK(mip_lod_split(c, -3.0, &lo, &w, &f) == 0 && lo == 0 && w == 0.0);
    CK(mip_lod_split(c, 99.0, &lo, &w, &f) == 0 && lo == c->levels - 1 && w == 0.0);
}

static void test_blend_off_identical(const MipChain *c) {
    /* 关闭混合时必须与 mip_sample 逐像素相同，含越界坐标 */
    for (int level = 0; level < c->levels; ++level)
        for (int i = 0; i <= 64; ++i)
            for (int j = 0; j <= 64; ++j) {
                double u = -0.25 + 1.5 * i / 64.0;
                double v = -0.25 + 1.5 * j / 64.0;
                unsigned char a[3], b[3];
                CK(mip_sample(c, level, u, v, a) == 0);
                CK(mip_sample_trilinear(c, level, 0.0, u, v, 0, b, NULL, NULL) == 0);
                CK(a[0] == b[0] && a[1] == b[1] && a[2] == b[2]);
            }
}

static void test_trilinear_values(const MipChain *c) {
    unsigned char px[3];
    MipTrace tr;
    /* 整层中点：weight 0 时输出等于该层双线性 */
    CK(mip_sample_trilinear(c, 1, 0.0, 0.5, 0.5, 1, px, &tr, NULL) == 0);
    CK(tr.level_lo == 1 && tr.level_hi == 1 && tr.weight == 0.0 && tr.texel_reads == 4);
    /* 半权重：输出为两层各自双线性之均值（±1 取整），trace 报权重与两层颜色 */
    CK(mip_sample_trilinear(c, 1, 0.5, 0.5, 0.5, 1, px, &tr, NULL) == 0);
    CK(tr.level_lo == 1 && tr.level_hi == 2 && tr.weight == 0.5 && tr.texel_reads == 8);
    for (int ch = 0; ch < 3; ++ch) {
        double expect = (tr.lo[ch] + tr.hi[ch]) / 2.0;
        CK(fabs(px[ch] - expect) <= 1.0);
    }
    /* weight==1 时输出等于 hi 层 */
    CK(mip_sample_trilinear(c, 1, 1.0, 0.3, 0.7, 1, px, &tr, NULL) == 0);
    CK(px[0] == tr.hi[0] && px[1] == tr.hi[1] && px[2] == tr.hi[2]);
}

static void test_reconcile(const MipChain *c) {
    /* 与独立参考实现对账：lod/u/v 网格上最大通道差不得超过 1 */
    int max_diff = 0, n = 0;
    for (int li = 0; li <= 40; ++li) {
        double lod = (c->levels - 1) * li / 40.0;
        int lo; double w;
        CK(mip_lod_split(c, lod, &lo, &w, NULL) == 0);
        for (int i = 0; i <= 32; ++i)
            for (int j = 0; j <= 32; ++j) {
                double u = i / 32.0, v = j / 32.0;
                unsigned char got[3], want[3];
                CK(mip_sample_trilinear(c, lo, w, u, v, 1, got, NULL, NULL) == 0);
                ref_trilinear(c, lod, u, v, want);
                for (int ch = 0; ch < 3; ++ch) {
                    int d = abs(got[ch] - want[ch]);
                    if (d > max_diff) max_diff = d;
                }
                ++n;
            }
    }
    printf("reconcile: %d samples, max channel diff %d\n", n, max_diff);
    CK(max_diff <= 1);
}

int main(void) {
    enum { W = 16, H = 16 };
    unsigned char *src = (unsigned char*)malloc(W*H*3);
    for (int i = 0; i < W*H; ++i) { src[i*3+0] = (unsigned char)(i % 256); src[i*3+1] = 40; src[i*3+2] = 200; }
    MipChain c;
    CK(mip_build(&c, src, W, H) == 0);
    CK(c.levels == 5);
    CK(c.w[1] == 8 && c.h[1] == 8);
    unsigned char px[3];
    CK(mip_sample(&c, 0, 0.5, 0.5, px) == 0);
    CK(mip_sample(&c, 9, 0.5, 0.5, px) == -1);

    test_validation(&c);
    test_lod(&c, W);
    test_blend_off_identical(&c);
    test_trilinear_values(&c);
    test_reconcile(&c);

    mip_free(&c);
    free(src);
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
