#include "mip.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

#define GAMMA 2.2

/* 与库无关的参考:按层号逐像素取最近层内纹素,不做任何平均。
 * 用于核对"整层直取"这条默认路径必须逐字节一致。 */
static void ref_thumb_identity(const MipChain *c, int level, unsigned char *out) {
    int w = c->w[level], h = c->h[level];
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int ch = 0; ch < 3; ++ch)
                out[(y*w+x)*3+ch] = c->pix[level][(y*w+x)*3+ch];
}

/* 与库无关的参考:分离式盒式平均,把层缩到 tw x th。
 * 每个缩略图像素覆盖 [x*lw/tw,(x+1)*lw/tw) x [y*lh/th,(y+1)*lh/th) 的源区域,
 * 区域内所有整纹素做算术平均(向下取整)。用于核对多采样路径。 */
static void ref_thumb_box(const MipChain *c, int level, int tw, int th,
                          unsigned char *out) {
    int lw = c->w[level], lh = c->h[level];
    for (int y = 0; y < th; ++y)
        for (int x = 0; x < tw; ++x) {
            long long sx0 = (long long)x * lw / tw;
            long long sx1 = (long long)(x + 1) * lw / tw;
            long long sy0 = (long long)y * lh / th;
            long long sy1 = (long long)(y + 1) * lh / th;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            if (sy1 <= sy0) sy1 = sy0 + 1;
            for (int ch = 0; ch < 3; ++ch) {
                long long acc = 0, cnt = 0;
                for (long long sy = sy0; sy < sy1 && sy < lh; ++sy)
                    for (long long sx = sx0; sx < sx1 && sx < lw; ++sx) {
                        acc += c->pix[level][(sy*lw+sx)*3+ch];
                        ++cnt;
                    }
                out[(y*tw+x)*3+ch] = (unsigned char)(cnt ? acc / cnt : 0);
            }
        }
}

int main(void) {
    enum { W = 16, H = 16 };
    unsigned char *src = (unsigned char*)malloc(W*H*3);
    for (int i = 0; i < W*H; ++i) {
        src[i*3+0] = (unsigned char)(i % 256);
        src[i*3+1] = 40;
        src[i*3+2] = 200;
    }
    MipChain c;
    CK(mip_build(&c, src, W, H) == 0);
    CK(c.levels == 5);
    CK(c.w[1] == 8 && c.h[1] == 8);

    /* ---- 旧行为回归:整链采样保持不变 ---- */
    unsigned char px[3];
    CK(mip_sample(&c, 0, 0.5, 0.5, px) == 0);
    CK(mip_sample(&c, 9, 0.5, 0.5, px) == -1);

    /* ---- 段1:默认规则 = 整层直取,逐字节等于 c->pix[level] ---- */
    for (int l = 0; l < c.levels; ++l) {
        MipThumbSpec spec = MIP_THUMB_SPEC_DEFAULT(l, &c);
        MipThumb t;
        MipError e;
        CK(mip_thumb_level(&c, &spec, &t, &e) == MIP_OK);
        CK(e.code == MIP_OK && e.index == l);
        CK(t.level == l && t.w == c.w[l] && t.h == c.h[l] && t.samples == 1);
        CK(t.sample_reads == (unsigned long long)c.w[l] * c.h[l]);
        CK(memcmp(t.pixels, c.pix[l], (size_t)c.w[l] * c.h[l] * 3) == 0);
        /* 与手写参考逐字节一致 */
        unsigned char *ref = (unsigned char*)malloc((size_t)c.w[l]*c.h[l]*3);
        ref_thumb_identity(&c, l, ref);
        CK(memcmp(t.pixels, ref, (size_t)c.w[l]*c.h[l]*3) == 0);
        free(ref);
        free(t.pixels);
    }

    /* ---- 段1:多采样路径与分离式盒式平均逐字节一致 ---- */
    CK(c.levels >= 3);
    for (int l = 0; l < 3; ++l) {
        int tw = c.w[l] / 2 > 0 ? c.w[l] / 2 : 1;
        int th = c.h[l] / 2 > 0 ? c.h[l] / 2 : 1;
        MipThumbSpec spec = { l, tw, th, 4 };
        MipThumb t;
        CK(mip_thumb_level(&c, &spec, &t, NULL) == MIP_OK);
        CK(t.sample_reads == (unsigned long long)16 * tw * th);
        unsigned char *ref = (unsigned char*)malloc((size_t)tw*th*3);
        ref_thumb_box(&c, l, tw, th, ref);
        CK(memcmp(t.pixels, ref, (size_t)tw*th*3) == 0);
        free(ref); free(t.pixels);
    }

    /* ---- 尺��非默认时,缩略图确实是层内容的缩放(不越界、不读错层) ---- */
    {
        MipThumbSpec spec = { 2, 3, 5, 1 }; /* 非整数比例 */
        MipThumb t;
        CK(mip_thumb_level(&c, &spec, &t, NULL) == MIP_OK);
        CK(t.w == 3 && t.h == 5);
        /* 取缩略图 (0,0) 应由层 2 的左上角纹素贡献;层 2 是 4x4 */
        CK(memcmp(t.pixels, c.pix[2], 3) == 0);
        free(t.pixels);
    }

    /* ---- 段1:非法输入报错,标出层号/字段 ---- */
    {
        MipThumb t;
        MipError e;
        MipThumbSpec spec;
        memset(&e, 0xAA, sizeof e);

        spec.level = -1; spec.tw = 4; spec.th = 4; spec.samples = 1;
        CK(mip_thumb_level(&c, &spec, &t, &e) == MIP_E_LEVEL);
        CK(e.code == MIP_E_LEVEL && e.index == -1);
        CK(e.field && strcmp(e.field, "level") == 0);

        spec.level = c.levels; spec.tw = 4; spec.th = 4; spec.samples = 1;
        CK(mip_thumb_level(&c, &spec, &t, &e) == MIP_E_LEVEL);
        CK(e.index == c.levels);

        spec.level = 0; spec.tw = 0; spec.th = 4; spec.samples = 1;
        CK(mip_thumb_level(&c, &spec, &t, &e) == MIP_E_W);
        CK(e.field && strcmp(e.field, "width") == 0);

        spec.level = 0; spec.tw = 4; spec.th = -2; spec.samples = 1;
        CK(mip_thumb_level(&c, &spec, &t, &e) == MIP_E_W);

        spec.level = 0; spec.tw = 4; spec.th = 4; spec.samples = 0;
        CK(mip_thumb_level(&c, &spec, &t, &e) == MIP_E_COUNT);
        CK(e.field && strcmp(e.field, "samples") == 0);

        spec.samples = -3;
        CK(mip_thumb_level(&c, &spec, &t, &e) == MIP_E_COUNT);

        spec.samples = 1;
        CK(mip_thumb_level(NULL, &spec, &t, &e) == MIP_E_ARG);
        CK(mip_thumb_level(&c, NULL, &t, &e) == MIP_E_ARG);
        CK(mip_thumb_level(&c, &spec, NULL, &e) == MIP_E_ARG);

        MipChain empty;
        memset(&empty, 0, sizeof empty);
        CK(mip_thumb_level(&empty, &spec, &t, &e) == MIP_E_THUMB);
        CK(e.field && strcmp(e.field, "thumb") == 0);
    }

    /* ---- 段2:按层号拼预览图,布局可核对 ---- */
    {
        enum { N = 5 };
        MipThumb th[N];
        for (int i = 0; i < N; ++i) {
            MipThumbSpec spec = MIP_THUMB_SPEC_DEFAULT(i, &c);
            CK(mip_thumb_level(&c, &spec, &th[i], NULL) == MIP_OK);
        }
        MipSheetSpec ss = MIP_SHEET_SPEC_DEFAULT;
        MipSheet sh;
        MipError e;
        CK(mip_sheet_assemble(&c, th, N, &ss, &sh, &e) == MIP_OK);
        /* cols = ceil(sqrt(5)) = 3,rows = 2;格宽 16 格高 16,pad=0 => 48x32 */
        CK(sh.cols == 3 && sh.w == 48 && sh.h == 32 && sh.count == N);
        /* 层 0 占左上 16x16,且与层像素逐字节一致 */
        CK(sh.x[0] == 0 && sh.y[0] == 0 && sh.tw[0] == 16 && sh.th[0] == 16);
        int same = 1;
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 16; ++x)
                for (int ch = 0; ch < 3; ++ch)
                    if (sh.pixels[((y*sh.w)+x)*3+ch] != c.pix[0][(y*16+x)*3+ch])
                        same = 0;
        CK(same);
        /* 层 4 是 1x1:col=1,row=1,格内水平居中,底部对齐 */
        CK(sh.tw[4] == 1 && sh.th[4] == 1);
        CK(sh.x[4] == 1*16 + 7 && sh.y[4] == 1*16 + 15);

        /* 层间差异:独立重算参考值 */
        unsigned long long ref_sum = 0, ref_px = 0;
        for (int i = 1; i < N; ++i) {
            int cw = th[i-1].w < th[i].w ? th[i-1].w : th[i].w;
            int chh = th[i-1].h < th[i].h ? th[i-1].h : th[i].h;
            for (int y = 0; y < chh; ++y)
                for (int x = 0; x < cw; ++x)
                    for (int ch = 0; ch < 3; ++ch) {
                        int a = th[i-1].pixels[(y*th[i-1].w+x)*3+ch];
                        int b = th[i].pixels[(y*th[i].w+x)*3+ch];
                        ref_sum += (unsigned long long)(a > b ? a - b : b - a);
                        ++ref_px;
                    }
        }
        CK(sh.inter_layer_bytes == ref_sum);
        CK(sh.inter_layer_px == ref_px);
        CK(sh.inter_layer_mean > 0.0);

        /* pad 与 label_gap 只影响布局,不影响像素内容 */
        MipSheetSpec sp = { 4, 3 };
        MipSheet sh2;
        CK(mip_sheet_assemble(&c, th, N, &sp, &sh2, NULL) == MIP_OK);
        CK(sh2.w == 3*16 + 2*4);
        CK(sh2.h == 2*(16+3) + 1*4);
        CK(sh2.inter_layer_bytes == sh.inter_layer_bytes);

        free(sh.pixels); free(sh.x); free(sh.y); free(sh.tw); free(sh.th);
        free(sh2.pixels); free(sh2.x); free(sh2.y); free(sh2.tw); free(sh2.th);
        for (int i = 0; i < N; ++i) free(th[i].pixels);
    }

    /* ---- 段2:缩略图非法报错,index 指出数组位置 ---- */
    {
        enum { N = 3 };
        MipThumb th[N];
        for (int i = 0; i < N; ++i) {
            MipThumbSpec spec = MIP_THUMB_SPEC_DEFAULT(i, &c);
            CK(mip_thumb_level(&c, &spec, &th[i], NULL) == MIP_OK);
        }
        MipSheet sh;
        MipError e;

        /* 乱序:层号 {0,2,1} 在 i=2 处不再升序 */
        th[1].level = 2; th[2].level = 1;
        CK(mip_sheet_assemble(&c, th, N, NULL, &sh, &e) == MIP_E_ORDER);
        CK(e.index == 2 && e.field && strcmp(e.field, "order") == 0);
        th[1].level = 1; th[2].level = 2;

        /* 重复:层号 {0,1,1} 在 i=2 处重复 */
        th[2].level = 1;
        CK(mip_sheet_assemble(&c, th, N, NULL, &sh, &e) == MIP_E_ORDER);
        CK(e.index == 2);
        th[2].level = 2;

        /* 层号越界:err->index 记为越界的层号本身 */
        th[1].level = 99;
        CK(mip_sheet_assemble(&c, th, N, NULL, &sh, &e) == MIP_E_LEVEL);
        CK(e.index == 99 && e.field && strcmp(e.field, "level") == 0);
        th[1].level = 1;

        th[0].level = -3;
        CK(mip_sheet_assemble(&c, th, N, NULL, &sh, &e) == MIP_E_LEVEL);
        CK(e.index == -3);
        th[0].level = 0;

        /* 未初始化 / 尺寸非法 / 来自别的链 */
        MipThumb bad = th[1];
        bad.pixels = NULL;
        MipThumb saved = th[1];
        th[1] = bad;
        CK(mip_sheet_assemble(&c, th, N, NULL, &sh, &e) == MIP_E_THUMB);
        CK(e.index == 1 && e.field && strcmp(e.field, "thumb") == 0);
        th[1] = saved;

        th[2].w = 0;
        CK(mip_sheet_assemble(&c, th, N, NULL, &sh, &e) == MIP_E_THUMB);
        CK(e.index == 2);
        th[2].w = 16;

        MipChain other;
        CK(mip_build(&other, src, W, H) == 0);
        MipThumbSpec ospec = MIP_THUMB_SPEC_DEFAULT(1, &other);
        MipThumb oth;
        CK(mip_thumb_level(&other, &ospec, &oth, NULL) == MIP_OK);
        MipThumb keep = th[1];
        th[1] = oth;
        CK(mip_sheet_assemble(&c, th, N, NULL, &sh, &e) == MIP_E_THUMB);
        CK(e.index == 1);
        th[1] = keep;
        mip_free(&other);
        free(oth.pixels);

        /* 布局参数非法 */
        MipSheetSpec sp = { -1, 0 };
        CK(mip_sheet_assemble(&c, th, N, &sp, &sh, &e) == MIP_E_SHEET);
        MipSheetSpec sp2 = { 0, -1 };
        CK(mip_sheet_assemble(&c, th, N, &sp2, &sh, &e) == MIP_E_SHEET);

        /* 一般参数 */
        CK(mip_sheet_assemble(&c, th, 0, NULL, &sh, &e) == MIP_E_ARG);
        CK(mip_sheet_assemble(NULL, th, N, NULL, &sh, &e) == MIP_E_ARG);
        CK(mip_sheet_assemble(&c, NULL, N, NULL, &sh, &e) == MIP_E_ARG);
        CK(mip_sheet_assemble(&c, th, N, NULL, NULL, &e) == MIP_E_ARG);

        for (int i = 0; i < N; ++i) free(th[i].pixels);
    }

    /* ---- 段1+段2 串联:只导出选定层,子集也能拼 ---- */
    {
        int lv[3] = { 0, 2, 4 };
        MipThumb th[3];
        for (int i = 0; i < 3; ++i) {
            MipThumbSpec spec = { lv[i], 8, 8, 2 };
            CK(mip_thumb_level(&c, &spec, &th[i], NULL) == MIP_OK);
        }
        MipSheet sh;
        CK(mip_sheet_assemble(&c, th, 3, NULL, &sh, NULL) == MIP_OK);
        CK(sh.count == 3 && sh.cols == 2 && sh.w == 16 && sh.h == 16);
        CK(sh.inter_layer_px == 2ULL * 8 * 8 * 3); /* 两对相邻层 */
        free(sh.pixels); free(sh.x); free(sh.y); free(sh.tw); free(sh.th);
        for (int i = 0; i < 3; ++i) free(th[i].pixels);
    }

    mip_free(&c);
    free(src);

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
