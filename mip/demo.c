#include "mip.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 调试演示:把金字塔按层单独采样成缩略图,再拼成一张预览图。
 *   1) 段1 逐层采样 -> thumbs[];
 *   2) 段2 按层号拼装 -> preview,并打印层间差异;
 *   3) 再跑一遍"默认规则",核对与当前版本(整层直取)逐字节一致。 */

int main(void) {
    enum { W = 64, H = 64 };
    unsigned char *src = (unsigned char*)malloc(W*H*3);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            unsigned char *p = src + (y*W+x)*3;
            p[0] = (unsigned char)(x * 4);
            p[1] = (unsigned char)(y * 4);
            p[2] = (unsigned char)(((x / 8) + (y / 8)) & 1 ? 220 : 40);
        }
    MipChain c;
    if (mip_build(&c, src, W, H) != 0) return 1;
    printf("levels %d sizes %dx%d %dx%d\n", c.levels, c.w[0], c.h[0],
           c.w[c.levels-1], c.h[c.levels-1]);

    int n = c.levels;
    MipThumb *th = (MipThumb*)calloc((size_t)n, sizeof(MipThumb));

    /* 段1:每层缩略图缩到 8x8,每轴 2 个采样点 */
    printf("\n[段1] 按层号单独采样 (缩略图 8x8, 每轴 2 点)\n");
    printf("  层  层尺寸      缩略图  采样次数  首像素 RGB\n");
    for (int l = 0; l < n; ++l) {
        MipThumbSpec spec = { l, 8, 8, 2 };
        MipError e;
        if (mip_thumb_level(&c, &spec, &th[l], &e) != MIP_OK) {
            fprintf(stderr, "层 %d 采样失败: %s (index=%d)\n",
                    l, e.field ? e.field : "?", e.index);
            return 1;
        }
        printf("  %-3d %-10dx%-3d %-7dx%-2d %-9llu %3d,%3d,%3d\n", l,
               c.w[l], c.h[l], th[l].w, th[l].h, th[l].sample_reads,
               th[l].pixels[0], th[l].pixels[1], th[l].pixels[2]);
    }

    /* 段2:按层号拼装 */
    MipSheetSpec ss = { 2, 1 };
    MipSheet sh;
    MipError e;
    printf("\n[段2] 按层拼装 (pad=2, label_gap=1)\n");
    if (mip_sheet_assemble(&c, th, n, &ss, &sh, &e) != MIP_OK) {
        fprintf(stderr, "拼装失败: %s (index=%d)\n",
                e.field ? e.field : "?", e.index);
        return 1;
    }
    printf("  预览图 %dx%d, cols=%d, 层数 %d\n", sh.w, sh.h, sh.cols, sh.count);
    printf("  层间差异: 合计 %llu / %llu 通道, 均值 %.4f\n",
           sh.inter_layer_bytes, sh.inter_layer_px, sh.inter_layer_mean);
    for (int i = 1; i < n; ++i) {
        unsigned long long s = 0, p = 0;
        int cw = sh.tw[i-1] < sh.tw[i] ? sh.tw[i-1] : sh.tw[i];
        int chh = sh.th[i-1] < sh.th[i] ? sh.th[i-1] : sh.th[i];
        for (int y = 0; y < chh; ++y)
            for (int x = 0; x < cw; ++x)
                for (int k = 0; k < 3; ++k) {
                    int a = th[i-1].pixels[(y*th[i-1].w+x)*3+k];
                    int b = th[i].pixels[(y*th[i].w+x)*3+k];
                    s += (unsigned long long)(a > b ? a - b : b - a); ++p;
                }
        printf("   层 %d->%d: 均值 %.4f\n", i-1, i, p ? (double)s/p : 0.0);
    }
    mip_write_ppm(sh.pixels, sh.w, sh.h, "demo.ppm");
    printf("  已写出 demo.ppm (%dx%d)\n", sh.w, sh.h);

    /* 默认规则核对:整层直取应与当前版本(P0 逐层原像素)逐字节一致 */
    int ok = 1;
    for (int l = 0; l < n; ++l) {
        MipThumbSpec spec = MIP_THUMB_SPEC_DEFAULT(l, &c);
        MipThumb t;
        if (mip_thumb_level(&c, &spec, &t, NULL) != MIP_OK) { ok = 0; break; }
        if (t.w != c.w[l] || t.h != c.h[l] || t.samples != 1 ||
            memcmp(t.pixels, c.pix[l], (size_t)c.w[l]*c.h[l]*3) != 0) ok = 0;
        free(t.pixels);
    }
    printf("\n[默认] 整层直取与当前版本逐字节一致: %s\n", ok ? "是" : "否");

    free(sh.pixels); free(sh.x); free(sh.y); free(sh.tw); free(sh.th);
    for (int l = 0; l < n; ++l) free(th[l].pixels);
    free(th);
    mip_free(&c);
    free(src);
    return ok ? 0 : 1;
}
