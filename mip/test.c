#include "mip.h"
#include "sharpen.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

static void gen_img(unsigned char *img, int w, int h) {
    unsigned s = 12345u;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            unsigned char *px = img + ((size_t)y * w + x) * 3;
            int edge = ((x / 5) + (y / 4)) % 2;
            s = s * 1664525u + 1013904223u;
            int nz = (int)((s >> 24) & 7);
            px[0] = (unsigned char)(edge ? 200 + nz : 30 + nz);
            px[1] = (unsigned char)(edge ? 180 - nz : 60 + nz);
            px[2] = (unsigned char)((x * 7 + y * 3) & 0xff);
        }
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
    mip_free(&c);
    free(src);

    /* ---- 两段式锐化 ---- */
    enum { SW = 24, SH = 16, SN = SW * SH * 3 };
    unsigned char *img = (unsigned char*)malloc(SN);
    unsigned char *dst = (unsigned char*)malloc(SN);
    unsigned char *dst2 = (unsigned char*)malloc(SN);
    unsigned char *ref = (unsigned char*)malloc(SN);
    float *amt = (float*)malloc(sizeof(float) * SW * SH);
    gen_img(img, SW, SH);
    MipSharpenStats st, rst, st2;

    /* 恢复默认: 输出与当前版本(未锐化)逐字节一致 */
    MipSharpenParams def = mip_sharpen_default();
    CK(def.strength == 0.0);
    CK(mip_sharpen(img, SW, SH, &def, dst, &st) == 0);
    CK(memcmp(dst, img, SN) == 0);
    CK(st.edge_pixels == 0 && st.ringing_pixels == 0 && st.overshoot == 0.0);

    /* 非默认参数: 与参考实现逐项核对 */
    MipSharpenParams p = { 1.5, 4.0, 80.0 };
    CK(mip_sharpen(img, SW, SH, &p, dst, &st) == 0);
    CK(mip_sharpen_ref(img, SW, SH, &p, ref, &rst) == 0);
    long nmis = 0, first = -1;
    for (long i = 0; i < SN; ++i)
        if (dst[i] != ref[i]) { ++nmis; if (first < 0) first = i; }
    if (nmis) printf("  impl/ref mismatch at byte %ld (%d vs %d)\n", first, dst[first], ref[first]);
    CK(nmis == 0);
    CK(fabs(st.overshoot - rst.overshoot) < 1e-9);
    CK(fabs(st.strength_mean - rst.strength_mean) < 1e-9);
    CK(fabs(st.strength_max - rst.strength_max) < 1e-9);
    CK(st.edge_pixels == rst.edge_pixels);
    CK(st.ringing_pixels == rst.ringing_pixels);
    CK(st.edge_pixels > 0 && st.edge_pixels < SW * SH);
    CK(st.ringing_pixels > 0 && st.overshoot > 0.0);   /* 强参数下未钳制必有振铃 */
    CK(st.ringing_pixels <= st.edge_pixels);

    /* 两段拆分结果与合并调用一致 */
    CK(mip_sharpen_amount(img, SW, SH, &p, amt) == 0);
    CK(mip_sharpen_apply(img, amt, SW, SH, dst2, &st2) == 0);
    CK(memcmp(dst, dst2, SN) == 0);
    CK(st2.edge_pixels == st.edge_pixels && st2.ringing_pixels == st.ringing_pixels);

    /* 无振铃性质: 输出不越出输入 3x3 局部 [min,max] */
    int ring_out = 0;
    for (int y = 0; y < SH; ++y)
        for (int x = 0; x < SW; ++x)
            for (int ch = 0; ch < 3; ++ch) {
                int mn = 255, mx = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        int xx = x + dx < 0 ? 0 : (x + dx >= SW ? SW - 1 : x + dx);
                        int yy = y + dy < 0 ? 0 : (y + dy >= SH ? SH - 1 : y + dy);
                        int v = img[((size_t)yy * SW + xx) * 3 + ch];
                        if (v < mn) mn = v;
                        if (v > mx) mx = v;
                    }
                int o = dst[((size_t)y * SW + x) * 3 + ch];
                if (o < mn || o > mx) ++ring_out;
            }
    CK(ring_out == 0);

    /* 非法输入: 报错并标出索引 */
    CK(mip_sharpen(NULL, SW, SH, &p, dst, NULL) == -1);
    CK(mip_sharpen(img, 0, SH, &p, dst, NULL) == -1);
    { MipSharpenParams bad = { 1.0, 80.0, 4.0 }; CK(mip_sharpen(img, SW, SH, &bad, dst, NULL) == -3); }
    { MipSharpenParams bad = { -1.0, 4.0, 80.0 }; CK(mip_sharpen(img, SW, SH, &bad, dst, NULL) == -3); }
    CK(mip_sharpen_amount(img, SW, SH, &p, NULL) == -1);
    CK(mip_sharpen_apply(img, NULL, SW, SH, dst, NULL) == -1);
    {
        unsigned char b0[3 * 4 * 4], b1[3 * 4 * 4], b2[3 * 4 * 4];
        unsigned char o0[3 * 4 * 4], o1[3 * 4 * 4], o2[3 * 4 * 4];
        const unsigned char *srcs[3] = { b0, NULL, b2 };
        unsigned char *dsts[3] = { o0, o1, o2 };
        int ws[3] = { 4, 4, 4 }, hs[3] = { 4, 4, 4 }, bad = -1;
        CK(mip_sharpen_batch(3, srcs, ws, hs, &p, dsts, &bad) == -2 && bad == 1);
        srcs[1] = b1; ws[2] = 0;
        CK(mip_sharpen_batch(3, srcs, ws, hs, &p, dsts, &bad) == -2 && bad == 2);
        ws[2] = 4;
        CK(mip_sharpen_batch(3, srcs, ws, hs, &p, dsts, &bad) == 0 && bad == -1);
        CK(mip_sharpen_batch(0, srcs, ws, hs, &p, dsts, &bad) == -1);
    }

    free(img); free(dst); free(dst2); free(ref); free(amt);
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
