#include "mip.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

/* 参考实现：复刻旧版本的完整层链构建，用于逐项核对像素与尺寸。 */
static int ref_build(MipChain *c, const unsigned char *src, int w, int h) {
    int levels = 1, ww = w, hh = h;
    while (ww > 1 || hh > 1) { ww = ww > 1 ? ww / 2 : 1; hh = hh > 1 ? hh / 2 : 1; ++levels; }
    c->levels = levels; c->w0 = w; c->h0 = h;
    c->pix = (unsigned char**)calloc((size_t)levels, sizeof(unsigned char*));
    c->w = (int*)calloc((size_t)levels, sizeof(int));
    c->h = (int*)calloc((size_t)levels, sizeof(int));
    if (!c->pix || !c->w || !c->h) return -1;
    c->w[0] = w; c->h[0] = h;
    c->pix[0] = (unsigned char*)malloc((size_t)w * (size_t)h * 3);
    if (!c->pix[0]) return -1;
    memcpy(c->pix[0], src, (size_t)w * (size_t)h * 3);
    for (int l = 1; l < levels; ++l) {
        int pw = c->w[l-1], ph = c->h[l-1];
        int nw = pw > 1 ? pw / 2 : 1, nh = ph > 1 ? ph / 2 : 1;
        c->w[l] = nw; c->h[l] = nh;
        c->pix[l] = (unsigned char*)malloc((size_t)nw * (size_t)nh * 3);
        if (!c->pix[l]) return -1;
        for (int y = 0; y < nh; ++y)
            for (int x = 0; x < nw; ++x)
                for (int ch = 0; ch < 3; ++ch) {
                    int x0 = x*2 < pw ? x*2 : pw-1, x1 = (x*2+1) < pw ? x*2+1 : pw-1;
                    int y0 = y*2 < ph ? y*2 : ph-1, y1 = (y*2+1) < ph ? y*2+1 : ph-1;
                    int s = c->pix[l-1][(y0*pw+x0)*3+ch] + c->pix[l-1][(y0*pw+x1)*3+ch]
                          + c->pix[l-1][(y1*pw+x0)*3+ch] + c->pix[l-1][(y1*pw+x1)*3+ch];
                    c->pix[l][(y*nw+x)*3+ch] = (unsigned char)(s / 4);
                }
    }
    return 0;
}

int main(void) {
    enum { W = 16, H = 16 };
    unsigned char *src = (unsigned char*)malloc(W*H*3);
    for (int i = 0; i < W*H; ++i) { src[i*3+0] = (unsigned char)(i % 256); src[i*3+1] = 40; src[i*3+2] = 200; }

    /* --- 默认（无预算）：与旧版行为一致 --- */
    MipChain c, ref;
    CK(mip_build(&c, src, W, H, 0) == 0);
    CK(c.levels == 5 && c.cap_levels == 5 && c.degraded == 0);
    CK(c.w[1] == 8 && c.h[1] == 8);
    CK(ref_build(&ref, src, W, H) == 0);
    CK(ref.levels == c.levels);
    for (int l = 0; l < c.levels; ++l) {
        CK(c.w[l] == ref.w[l] && c.h[l] == ref.h[l]);
        CK(memcmp(c.pix[l], ref.pix[l], (size_t)c.w[l]*c.h[l]*3) == 0);
    }
    unsigned char px[3];
    CK(mip_sample(&c, 0, 0.5, 0.5, px) == 0);
    CK(mip_sample(&c, 9, 0.5, 0.5, px) == -1);
    mip_free(&ref);
    mip_free(&c);

    /* --- mip_plan：层数与逐层字节数 --- */
    MipPlan p;
    int idx = 99;
    CK(mip_plan(W, H, 0, &p, &idx) == 0 && idx == -1);
    CK(p.full_levels == 5 && p.cap_levels == 5 && p.degraded == 0);
    CK(p.level_bytes[0] == (size_t)16*16*3 && p.level_bytes[4] == 3);
    CK(p.total_bytes == (16*16 + 8*8 + 4*4 + 2*2 + 1) * 3);

    /* 预算只够前两层：16x16x3=768, +8x8x3=192 => 960 */
    CK(mip_plan(W, H, 960, &p, &idx) == 0);
    CK(p.cap_levels == 2 && p.degraded == 1 && p.total_bytes == 960);
    CK(mip_plan(W, H, 959, &p, &idx) == 0 && p.cap_levels == 1);
    /* 预算连第 0 层都不够：下限 1 层，标记降级 */
    CK(mip_plan(W, H, 10, &p, &idx) == 0 && p.cap_levels == 1 && p.degraded == 1);

    /* --- 预算下构建：截断层数，已生成层与参考实现逐字节相同 --- */
    CK(mip_build(&c, src, W, H, 960) == 0);
    CK(c.levels == 2 && c.cap_levels == 2 && c.degraded == 1);
    CK(ref_build(&ref, src, W, H) == 0);
    for (int l = 0; l < c.levels; ++l) {
        CK(c.w[l] == ref.w[l] && c.h[l] == ref.h[l]);
        CK(memcmp(c.pix[l], ref.pix[l], (size_t)c.w[l]*c.h[l]*3) == 0);
    }
    CK(mip_sample(&c, 2, 0.5, 0.5, px) == -1); /* 超出实际层数 */
    mip_free(&c); mip_free(&ref);

    /* --- 非 2 次幂尺寸也能正确降级且与参考一致 --- */
    CK(mip_plan(10, 6, 0, &p, &idx) == 0 && p.full_levels == 4);
    CK(mip_build(&c, src, 10, 6, p.level_bytes[0] + p.level_bytes[1]) == 0);
    CK(ref_build(&ref, src, 10, 6) == 0);
    CK(c.levels == 2 && c.degraded == 1);
    for (int l = 0; l < c.levels; ++l)
        CK(memcmp(c.pix[l], ref.pix[l], (size_t)c.w[l]*c.h[l]*3) == 0);
    mip_free(&c); mip_free(&ref);

    /* --- 非法输入：报错并标出参数索引（c=0 src=1 w=2 h=3 max_bytes=4 plan=5） --- */
    idx = 99;
    CK(mip_plan(0, H, 0, &p, &idx) == -2 && idx == 2);
    idx = 99;
    CK(mip_plan(W, -3, 0, &p, &idx) == -2 && idx == 3);
    idx = 99;
    CK(mip_plan(W, H, 0, NULL, &idx) == -1 && idx == 5);
    CK(mip_build(NULL, src, W, H, 0) == -1);
    CK(mip_build(&c, NULL, W, H, 0) == -1);
    CK(mip_build(&c, src, 0, H, 0) == -2);
    CK(mip_build(&c, src, W, -1, 0) == -2);
    CK(mip_build(&c, src, 1 << 29, H, 0) == -2);

    free(src);
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
