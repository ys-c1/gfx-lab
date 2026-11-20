#include "ninepatch.h"
#include <stdio.h>
#include <stdlib.h>

/* 最近邻缩放：把 src 的 sw*sh 区域拉伸/收缩到 dst 的 dw*dh 区域（逐通道拷贝） */
static void blit_stretch(const unsigned char *src, int sw, int sh,
                         unsigned char *dst, int dw, int dh,
                         int dx, int dy, int dst_pitch) {
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    for (int y = 0; y < dh; ++y) {
        int sy = sh > 1 ? (y * sh) / dh : 0;
        if (sy >= sh) sy = sh - 1;
        for (int x = 0; x < dw; ++x) {
            int sx = sw > 1 ? (x * sw) / dw : 0;
            if (sx >= sw) sx = sw - 1;
            const unsigned char *sp = src + ((size_t)sy * sw + sx) * 3;
            unsigned char *dp = dst + ((size_t)(dy + y) * dst_pitch + (dx + x)) * 3;
            dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2];
        }
    }
}

/* 平铺：从源区域 (sx0,sy0) 起 sw*sh 的图块，重复铺到目标矩形，右侧/底侧按需要截断 */
static void blit_tile(const unsigned char *src, int src_w,
                      int sx0, int sy0, int sw, int sh,
                      unsigned char *dst, int dst_pitch,
                      int dx0, int dy0, int dw, int dh) {
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    for (int y = 0; y < dh; ++y) {
        int sy = sy0 + (y % sh);
        for (int x = 0; x < dw; ++x) {
            int sx = sx0 + (x % sw);
            const unsigned char *sp = src + ((size_t)sy * src_w + sx) * 3;
            unsigned char *dp = dst + ((size_t)(dy0 + y) * dst_pitch + (dx0 + x)) * 3;
            dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2];
        }
    }
}

/* 逐像素直接拷贝一个矩形（四角使用，保证 1:1 不重采样） */
static void blit_copy(const unsigned char *src, int src_w,
                      int sx0, int sy0, int sw, int sh,
                      unsigned char *dst, int dst_pitch,
                      int dx0, int dy0) {
    for (int y = 0; y < sh; ++y)
        for (int x = 0; x < sw; ++x) {
            const unsigned char *sp = src + ((size_t)(sy0 + y) * src_w + (sx0 + x)) * 3;
            unsigned char *dp = dst + ((size_t)(dy0 + y) * dst_pitch + (dx0 + x)) * 3;
            dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2];
        }
}

int np_render(const unsigned char *src, int w, int h,
              const NpConfig *cfg, unsigned char *dst, int dw, int dh) {
    if (!src || !cfg || !dst || w <= 0 || h <= 0 || dw <= 0 || dh <= 0) return -1;

    /* 关闭九宫格：整图最近邻缩放到目标尺寸，与普通位图拉伸布局完全相同 */
    if (!cfg->enabled) {
        blit_stretch(src, w, h, dst, dw, dh, 0, 0, dw);
        return 0;
    }

    /* 边宽必须为非负，且两对边之和不得超过图块尺寸（切分越界 / 边宽超图块） */
    if (cfg->left < 0 || cfg->right < 0 || cfg->top < 0 || cfg->bottom < 0 ||
        cfg->left + cfg->right > w || cfg->top + cfg->bottom > h)
        return -2;

    if (cfg->h_strategy != NP_STRETCH && cfg->h_strategy != NP_TILE) return -3;
    if (cfg->v_strategy != NP_STRETCH && cfg->v_strategy != NP_TILE) return -3;

    const int L = cfg->left, R = cfg->right, T = cfg->top, B = cfg->bottom;
    const int cw = w - L - R;   /* 中心源宽 */
    const int ch = h - T - B;   /* 中心源高 */
    const int dcw = dw - L - R; /* 目标中心宽 */
    const int dch = dh - T - B;

    /* 源九宫格切分坐标 */
    const int x0 = L, x1 = w - R;
    const int y0 = T, y1 = h - B;
    /* 目标九宫格切分坐标 */
    const int dx0 = L, dx1 = dw - R;
    const int dy0 = T, dy1 = dh - B;

    /* 1) 四角：直接拷贝，不做任何缩放 */
    blit_copy(src, w, 0, 0, L, T, dst, dw, 0, 0);                 /* 左上 */
    blit_copy(src, w, x1, 0, R, T, dst, dw, dx1, 0);              /* 右上 */
    blit_copy(src, w, 0, y1, L, B, dst, dw, 0, dy1);              /* 左下 */
    blit_copy(src, w, x1, y1, R, B, dst, dw, dx1, dy1);           /* 右下 */

    /* 2) 上/下边：水平方向按 h_strategy 拉伸或平铺，高度与源边相同。
          中心源宽为 0 时没有边块内容可铺（目标中心宽也必为 0），直接跳过。 */
    if (dcw > 0 && T > 0) {
        if (cfg->h_strategy == NP_STRETCH)
            blit_stretch(src + ((size_t)0 * w + x0) * 3, cw, T,
                         dst + ((size_t)0 * dw + dx0) * 3, dcw, T, 0, 0, dw);
        else
            blit_tile(src, w, x0, 0, cw, T, dst, dw, dx0, 0, dcw, T);
    }
    if (dcw > 0 && B > 0) {
        if (cfg->h_strategy == NP_STRETCH)
            blit_stretch(src + ((size_t)y1 * w + x0) * 3, cw, B,
                         dst + ((size_t)dy1 * dw + dx0) * 3, dcw, B, 0, 0, dw);
        else
            blit_tile(src, w, x0, y1, cw, B, dst, dw, dx0, dy1, dcw, B);
    }

    /* 3) 左/右边：垂直方向按 v_strategy 拉伸或平铺，宽度与源边相同 */
    if (dch > 0 && L > 0) {
        if (cfg->v_strategy == NP_STRETCH)
            blit_stretch(src + ((size_t)y0 * w + 0) * 3, L, ch,
                         dst + ((size_t)dy0 * dw + 0) * 3, L, dch, 0, 0, dw);
        else
            blit_tile(src, w, 0, y0, L, ch, dst, dw, 0, dy0, L, dch);
    }
    if (dch > 0 && R > 0) {
        if (cfg->v_strategy == NP_STRETCH)
            blit_stretch(src + ((size_t)y0 * w + x1) * 3, R, ch,
                         dst + ((size_t)dy0 * dw + dx1) * 3, R, dch, 0, 0, dw);
        else
            blit_tile(src, w, x1, y0, R, ch, dst, dw, dx1, dy0, R, dch);
    }

    /* 4) 中心：双向拉伸（九宫格标准行为）；退化尺寸下循环自然为空 */
    blit_stretch(src + ((size_t)y0 * w + x0) * 3, cw, ch,
                 dst + ((size_t)dy0 * dw + dx0) * 3, dcw, dch, 0, 0, dw);
    return 0;
}

int np_write_ppm(const unsigned char *rgb, int w, int h, const char *path) {
    if (!rgb || !path || w <= 0 || h <= 0) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) return -2;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    fwrite(rgb, 1, (size_t)w * (size_t)h * 3, f);
    fclose(f);
    return 0;
}
