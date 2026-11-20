#include "mip.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

/* 与库无关的手工双线性参照：weight==0 时输出必须逐字节一致 */
static void ref_bilinear(const unsigned char *img, int w, int h, int ow, int oh, unsigned char *out) {
    for (int y = 0; y < oh; ++y)
        for (int x = 0; x < ow; ++x) {
            double sx = (x + 0.5) * (double)w / ow - 0.5;
            double sy = (y + 0.5) * (double)h / oh - 0.5;
            if (sx < 0) sx = 0;
            if (sy < 0) sy = 0;
            if (sx > w - 1) sx = w - 1;
            if (sy > h - 1) sy = h - 1;
            int x0 = (int)sx, y0 = (int)sy;
            int x1 = x0 + 1 < w ? x0 + 1 : x0, y1 = y0 + 1 < h ? y0 + 1 : y0;
            double tx = sx - x0, ty = sy - y0;
            for (int ch = 0; ch < 3; ++ch) {
                double v = img[(y0*w+x0)*3+ch]*(1-tx)*(1-ty) + img[(y0*w+x1)*3+ch]*tx*(1-ty)
                         + img[(y1*w+x0)*3+ch]*(1-tx)*ty     + img[(y1*w+x1)*3+ch]*tx*ty;
                int r = (int)(v + 0.5);
                if (r < 0) r = 0;
                if (r > 255) r = 255;
                out[(y*ow+x)*3+ch] = (unsigned char)r;
            }
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

    /* 段1：按层号取相邻两层，层号越界报错并指名字段 */
    MipLevelPair pr;
    CK(mip_fetch_pair(&c, 0, &pr) == 0);
    CK(pr.lo == c.pix[0] && pr.hi == c.pix[1] && pr.lw == 16 && pr.hw == 8);
    CK(mip_fetch_pair(&c, -1, &pr) == MIP_E_LEVEL);
    CK(mip_fetch_pair(&c, c.levels - 1, &pr) == MIP_E_LEVEL);
    CK(strcmp(mip_err_field(MIP_E_LEVEL), "level") == 0);
    CK(strcmp(mip_err_field(MIP_E_WEIGHT), "weight") == 0);
    CK(mip_err_field(0) == NULL);

    enum { OW = 32, OH = 32 };
    unsigned char *up0 = (unsigned char*)malloc(OW*OH*3);
    unsigned char *up1 = (unsigned char*)malloc(OW*OH*3);
    unsigned char *ref = (unsigned char*)malloc(OW*OH*3);
    MipUpscaleStats s0, s1, sd;

    /* 权重为负：报错并指名字段 */
    CK(mip_upscale(&c, 0, -0.5, OW, OH, up0, &s0) == MIP_E_WEIGHT);
    double *accum = (double*)malloc(OW*OH*3*sizeof(double));
    CK(mip_recalc_sharp(&pr, -1.0, OW, OH, accum, NULL) == MIP_E_WEIGHT);
    free(accum);
    /* 层号越界：便捷入口同样报 level */
    CK(mip_upscale(&c, c.levels - 1, 1.0, OW, OH, up0, &s0) == MIP_E_LEVEL);
    CK(mip_deconv(&c, c.levels, 4, OW, OH, up0, &sd) == MIP_E_LEVEL);

    /* 不做锐化（weight==0）：输出与手工双线性参照逐字节一致 */
    CK(mip_upscale(&c, 0, 0.0, OW, OH, up0, &s0) == 0);
    ref_bilinear(c.pix[0], W, H, OW, OH, ref);
    CK(memcmp(up0, ref, OW*OH*3) == 0);
    CK(s0.samples == (unsigned long)OW*OH*4);

    /* 锐化：锐度提升，采样次数翻倍（两层） */
    CK(mip_upscale(&c, 0, 1.0, OW, OH, up1, &s1) == 0);
    CK(s1.sharpness > s0.sharpness);
    CK(s1.samples == 2 * s0.samples);

    /* 手工反卷积对照：可运行且同样提升锐度 */
    CK(mip_deconv(&c, 0, 4, OW, OH, up1, &sd) == 0);
    CK(sd.sharpness > s0.sharpness);
    CK(sd.samples == (unsigned long)OW*OH*4*(1 + 4));

    free(up0); free(up1); free(ref);
    mip_free(&c);
    free(src);
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
