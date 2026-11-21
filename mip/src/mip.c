#include "mip.h"
#include <math.h>
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
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= c->w[level]) x = c->w[level] - 1;
    if (y >= c->h[level]) y = c->h[level] - 1;
    for (int ch = 0; ch < 3; ++ch) out[ch] = c->pix[level][(y*c->w[level]+x)*3+ch];
    return 0;
}

void mip_diag_init(MipDiag *d, int enable_center) {
    if (!d) return;
    d->enable_center = enable_center ? 1 : 0;
    d->samples = d->corrections = d->oob = 0;
    d->last_oob = 0;
    d->last_raw_x = d->last_raw_y = 0;
    d->last_old_x = d->last_old_y = 0;
    d->last_ou = d->last_ov = 0.0;
}

int mip_sample_ex(const MipChain *c, int level, double u, double v, unsigned char out[3], MipDiag *d) {
    if (!c || !out || level < 0 || level >= c->levels) return -1;
    int W = c->w[level], H = c->h[level];

    /* 关闭新逻辑：完全走老路径，输出与 mip_sample 字节一致 */
    if (!d || !d->enable_center) {
        if (d) ++d->samples;
        return mip_sample(c, level, u, v, out);
    }

    ++d->samples;

    /* 第一段：纹素中心偏移（半纹素） */
    double ou = 0.5 / (double)W;
    double ov = 0.5 / (double)H;
    /* 第二段：按偏移修正坐标，floor 保证负坐标向负无穷取整以检出越界 */
    int rx = (int)floor(u * (double)W - 0.5);
    int ry = (int)floor(v * (double)H - 0.5);

    /* 老路径坐标（clamp 后），仅用于对照与修正计数 */
    int ox = (int)(u * W), oy = (int)(v * H);
    if (ox < 0) ox = 0;
    if (oy < 0) oy = 0;
    if (ox >= W) ox = W - 1;
    if (oy >= H) oy = H - 1;

    /* 越界检出：raw 在合法纹素范围之外即报告，绝不带越界下标访问像素 */
    int bad = (rx < 0 || rx >= W || ry < 0 || ry >= H);
    if (bad) ++d->oob;
    d->last_oob = bad;

    /* 安全 clamp 后再取色 */
    int x = rx, y = ry;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= W) x = W - 1;
    if (y >= H) y = H - 1;

    /* 修正计数只统计 raw 合法、但与老路径坐标不同的轴（越界另计） */
    if (rx >= 0 && rx < W && rx != ox) ++d->corrections;
    if (ry >= 0 && ry < H && ry != oy) ++d->corrections;

    d->last_raw_x = rx; d->last_raw_y = ry;
    d->last_old_x = ox; d->last_old_y = oy;
    d->last_ou = ou; d->last_ov = ov;

    for (int ch = 0; ch < 3; ++ch) out[ch] = c->pix[level][(y*W+x)*3+ch];
    return bad ? 1 : 0;
}
