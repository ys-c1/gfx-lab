#include "mip.h"
#include <stdlib.h>

int mip_build(MipChain *c, const unsigned char *src, int w, int h) {
    if (!c || !src || w <= 0 || h <= 0) return -1;
    int levels = 1, ww = w, hh = h;
    while (ww > 1 || hh > 1) { ww = ww > 1 ? ww / 2 : 1; hh = hh > 1 ? hh / 2 : 1; ++levels; }
    c->levels = levels;
    c->w0 = w; c->h0 = h;
    c->pix = (unsigned char**)calloc((size_t)levels, sizeof(unsigned char*));
    c->w = (int*)calloc((size_t)levels, sizeof(int));
    c->h = (int*)calloc((size_t)levels, sizeof(int));
    if (!c->pix || !c->w || !c->h) { mip_free(c); return -1; }
    c->w[0] = w; c->h[0] = h;
    c->pix[0] = (unsigned char*)malloc((size_t)w * (size_t)h * 3);
    if (!c->pix[0]) { mip_free(c); return -1; }
    for (int i = 0; i < w * h * 3; ++i) c->pix[0][i] = src[i];
    for (int l = 1; l < levels; ++l) {
        int pw = c->w[l-1], ph = c->h[l-1];
        int nw = pw > 1 ? pw / 2 : 1, nh = ph > 1 ? ph / 2 : 1;
        c->w[l] = nw; c->h[l] = nh;
        c->pix[l] = (unsigned char*)malloc((size_t)nw * (size_t)nh * 3);
        if (!c->pix[l]) { mip_free(c); return -1; }
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

void mip_free(MipChain *c) {
    if (!c) return;
    for (int l = 0; l < c->levels; ++l) free(c->pix[l]);
    free(c->pix); free(c->w); free(c->h);
    c->pix = NULL; c->w = NULL; c->h = NULL; c->levels = 0;
}

int mip_sample(const MipChain *c, int level, double u, double v, unsigned char out[3]) {
    if (!c || !out || level < 0 || level >= c->levels) return -1;
    int x = (int)(u * c->w[level]);
    int y = (int)(v * c->h[level]);
    if (x < 0) x = 0; if (y < 0) y = 0;
    if (x >= c->w[level]) x = c->w[level] - 1;
    if (y >= c->h[level]) y = c->h[level] - 1;
    for (int ch = 0; ch < 3; ++ch) out[ch] = c->pix[level][(y*c->w[level]+x)*3+ch];
    return 0;
}

/* ---- 放大锐化：三段流水线 ---- */

const char *mip_err_field(int rc) {
    switch (rc) {
    case MIP_E_WEIGHT: return "weight";
    case MIP_E_LEVEL:  return "level";
    default:           return NULL;
    }
}

/* 双线性采样：源图像素坐标 (sx, sy)，越界夹取；计 4 次纹素采样 */
static void bilinear_at(const unsigned char *img, int w, int h,
                        double sx, double sy, double out[3],
                        unsigned long *samples) {
    if (sx < 0.0) sx = 0.0;
    if (sy < 0.0) sy = 0.0;
    if (sx > (double)(w - 1)) sx = (double)(w - 1);
    if (sy > (double)(h - 1)) sy = (double)(h - 1);
    int x0 = (int)sx, y0 = (int)sy;
    int x1 = x0 + 1 < w ? x0 + 1 : x0;
    int y1 = y0 + 1 < h ? y0 + 1 : y0;
    double tx = sx - x0, ty = sy - y0;
    for (int ch = 0; ch < 3; ++ch) {
        double p00 = img[(y0*w+x0)*3+ch], p01 = img[(y0*w+x1)*3+ch];
        double p10 = img[(y1*w+x0)*3+ch], p11 = img[(y1*w+x1)*3+ch];
        out[ch] = p00*(1.0-tx)*(1.0-ty) + p01*tx*(1.0-ty)
                + p10*(1.0-tx)*ty       + p11*tx*ty;
    }
    if (samples) *samples += 4;
}

int mip_fetch_pair(const MipChain *c, int level, MipLevelPair *pair) {
    if (!c || !pair) return MIP_E_ARG;
    if (level < 0 || level + 1 >= c->levels) return MIP_E_LEVEL;
    pair->lo = c->pix[level];     pair->lw = c->w[level];     pair->lh = c->h[level];
    pair->hi = c->pix[level + 1]; pair->hw = c->w[level + 1]; pair->hh = c->h[level + 1];
    return 0;
}

int mip_recalc_sharp(const MipLevelPair *pair, double weight,
                     int out_w, int out_h, double *accum,
                     unsigned long *samples) {
    if (!pair || !accum || out_w <= 0 || out_h <= 0) return MIP_E_ARG;
    if (weight < 0.0) return MIP_E_WEIGHT;
    for (int y = 0; y < out_h; ++y)
        for (int x = 0; x < out_w; ++x) {
            double sx = (x + 0.5) * (double)pair->lw / out_w - 0.5;
            double sy = (y + 0.5) * (double)pair->lh / out_h - 0.5;
            double base[3];
            double *dst = accum + ((size_t)y * out_w + x) * 3;
            bilinear_at(pair->lo, pair->lw, pair->lh, sx, sy, base, samples);
            if (weight > 0.0) {
                /* 同一世界坐标换算到 hi 层像素坐标 */
                double hx = (sx + 0.5) * (double)pair->hw / pair->lw - 0.5;
                double hy = (sy + 0.5) * (double)pair->hh / pair->lh - 0.5;
                double blur[3];
                bilinear_at(pair->hi, pair->hw, pair->hh, hx, hy, blur, samples);
                for (int ch = 0; ch < 3; ++ch)
                    dst[ch] = base[ch] + weight * (base[ch] - blur[ch]);
            } else {
                for (int ch = 0; ch < 3; ++ch) dst[ch] = base[ch];
            }
        }
    return 0;
}

/* 锐度指标：RGB 全通道的平均梯度幅值（|dx|+|dy| 均值） */
static double mean_abs_gradient(const unsigned char *img, int w, int h) {
    double acc = 0.0;
    unsigned long n = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int ch = 0; ch < 3; ++ch) {
                int cur = img[((size_t)y*w + x) * 3 + ch];
                if (x + 1 < w) {
                    int d = cur - img[((size_t)y*w + x + 1) * 3 + ch];
                    acc += d < 0 ? -d : d; ++n;
                }
                if (y + 1 < h) {
                    int d = cur - img[((size_t)(y+1)*w + x) * 3 + ch];
                    acc += d < 0 ? -d : d; ++n;
                }
            }
    return n ? acc / (double)n : 0.0;
}

int mip_composite(const double *accum, int w, int h,
                  unsigned char *out, double *sharpness) {
    if (!accum || !out || w <= 0 || h <= 0) return MIP_E_ARG;
    for (size_t i = 0, n = (size_t)w * h * 3; i < n; ++i) {
        int r = (int)(accum[i] + 0.5); /* 四舍五入后夹取到 [0,255] */
        if (r < 0) r = 0;
        if (r > 255) r = 255;
        out[i] = (unsigned char)r;
    }
    if (sharpness) *sharpness = mean_abs_gradient(out, w, h);
    return 0;
}

int mip_upscale(const MipChain *c, int level, double weight,
                int out_w, int out_h, unsigned char *out,
                MipUpscaleStats *stats) {
    if (!c || !out || out_w <= 0 || out_h <= 0) return MIP_E_ARG;
    MipLevelPair pair;
    int rc = mip_fetch_pair(c, level, &pair);          /* 段1 */
    if (rc != 0) return rc;
    double *accum = (double*)malloc((size_t)out_w * out_h * 3 * sizeof(double));
    if (!accum) return MIP_E_ARG;
    unsigned long samples = 0;
    rc = mip_recalc_sharp(&pair, weight, out_w, out_h, accum, &samples); /* 段2 */
    if (rc == 0) {
        double sharp = 0.0;
        rc = mip_composite(accum, out_w, out_h, out, &sharp);            /* 段3 */
        if (rc == 0 && stats) { stats->sharpness = sharp; stats->samples = samples; }
    }
    free(accum);
    return rc;
}

int mip_deconv(const MipChain *c, int level, int iterations,
               int out_w, int out_h, unsigned char *out,
               MipUpscaleStats *stats) {
    if (!c || !out || out_w <= 0 || out_h <= 0) return MIP_E_ARG;
    if (level < 0 || level >= c->levels) return MIP_E_LEVEL;
    if (iterations < 1) return MIP_E_ARG;
    size_t n = (size_t)out_w * out_h * 3;
    double *obs = (double*)malloc(n * sizeof(double));
    double *cur = (double*)malloc(n * sizeof(double));
    double *tmp = (double*)malloc(n * sizeof(double));
    if (!obs || !cur || !tmp) { free(obs); free(cur); free(tmp); return MIP_E_ARG; }
    unsigned long samples = 0;
    const unsigned char *src = c->pix[level];
    int sw = c->w[level], sh = c->h[level];
    for (int y = 0; y < out_h; ++y)
        for (int x = 0; x < out_w; ++x) {
            double sx = (x + 0.5) * (double)sw / out_w - 0.5;
            double sy = (y + 0.5) * (double)sh / out_h - 0.5;
            double b[3];
            bilinear_at(src, sw, sh, sx, sy, b, &samples);
            for (int ch = 0; ch < 3; ++ch) {
                obs[((size_t)y*out_w + x) * 3 + ch] = b[ch];
                cur[((size_t)y*out_w + x) * 3 + ch] = b[ch];
            }
        }
    /* Van Cittert：cur += obs - k*cur，k 为 2x2 盒式核（mip 降采样的模糊模型） */
    for (int it = 0; it < iterations; ++it) {
        for (int y = 0; y < out_h; ++y)
            for (int x = 0; x < out_w; ++x) {
                int x1 = x + 1 < out_w ? x + 1 : x;
                int y1 = y + 1 < out_h ? y + 1 : y;
                for (int ch = 0; ch < 3; ++ch) {
                    size_t i = ((size_t)y*out_w + x) * 3 + ch;
                    double blur = (cur[i]
                                 + cur[((size_t)y*out_w + x1) * 3 + ch]
                                 + cur[((size_t)y1*out_w + x) * 3 + ch]
                                 + cur[((size_t)y1*out_w + x1) * 3 + ch]) * 0.25;
                    tmp[i] = cur[i] + (obs[i] - blur);
                }
                samples += 4;
            }
        double *sw2 = cur; cur = tmp; tmp = sw2;
    }
    double sharp = 0.0;
    int rc = mip_composite(cur, out_w, out_h, out, &sharp);
    if (rc == 0 && stats) { stats->sharpness = sharp; stats->samples = samples; }
    free(obs); free(cur); free(tmp);
    return rc;
}
