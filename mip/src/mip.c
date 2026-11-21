#include "mip.h"
#include <math.h>
#include <stdio.h>
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

/* 历史夹边路径：算术与旧版 mip_sample 逐像素一致，关闭寻址时走这里。 */
static int sample_clamped(const MipChain *c, int level, double u, double v, unsigned char out[3]) {
    int x = (int)(u * c->w[level]);
    int y = (int)(v * c->h[level]);
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= c->w[level]) x = c->w[level] - 1;
    if (y >= c->h[level]) y = c->h[level] - 1;
    for (int ch = 0; ch < 3; ++ch) out[ch] = c->pix[level][(y*c->w[level]+x)*3+ch];
    return 0;
}

int mip_sample(const MipChain *c, int level, double u, double v, unsigned char out[3]) {
    if (!c || !out || level < 0 || level >= c->levels) return -1;
    return sample_clamped(c, level, u, v, out);
}

static const char *axis_name(int axis) { return axis == 0 ? "u" : "v"; }

/* 单轴换算：把 t 按模式映射到 [0,1)。调用方已完成全部校验。 */
static double apply_mode(int mode, double t) {
    switch (mode) {
        case MIP_ADDR_CLAMP:
            if (t < 0.0) return 0.0;
            if (t >= 1.0) return nextafter(1.0, 0.0); /* 保证 (int)(t*w) == w-1 */
            return t;
        case MIP_ADDR_WRAP:
            return t - floor(t);                       /* 负数也能正确回绕 */
        case MIP_ADDR_MIRROR: {
            double z = fabs(t);
            double p = floor(z);
            double f = z - p;
            double r = fmod(p, 2.0) == 0.0 ? f : 1.0 - f;
            if (r >= 1.0) r = nextafter(1.0, 0.0);  /* 奇数整数点：严格收敛回 [0,1) */
            return r;
        }
        default:
            return t;
    }
}

int mip_address(const MipSampler *s, int axis, double t, int tri_id, double *out_t) {
    if (!s || !out_t) {
        fprintf(stderr, "mip_address: null argument (triangle %d)\n", tri_id);
        return MIP_ERR_ARG;
    }
    if (axis != 0 && axis != 1) {
        fprintf(stderr, "mip_address: invalid axis %d (triangle %d)\n", axis, tri_id);
        return MIP_ERR_ARG;
    }
    if (!s->enabled) { *out_t = t; return 0; }        /* 关寻址：恒等，不碰寻址表 */
    if (!s->addr) {
        fprintf(stderr, "mip_address: addressing enabled but address table missing for %s axis (triangle %d)\n",
                axis_name(axis), tri_id);
        return MIP_ERR_NOTABLE;
    }
    if (!isfinite(t)) {
        fprintf(stderr, "mip_address: non-finite %s coordinate %g (triangle %d)\n",
                axis_name(axis), t, tri_id);
        return MIP_ERR_NONFIN;
    }
    int mode = s->addr[axis].mode;
    if (mode != MIP_ADDR_CLAMP && mode != MIP_ADDR_WRAP && mode != MIP_ADDR_MIRROR) {
        fprintf(stderr, "mip_address: invalid address mode %d on %s axis (triangle %d)\n",
                mode, axis_name(axis), tri_id);
        return MIP_ERR_MODE;
    }
    *out_t = apply_mode(mode, t);
    return 0;
}

int mip_sample_ex(const MipChain *c, int level, const MipSampler *s,
                  double u, double v, int tri_id, unsigned char out[3]) {
    if (!c || !out) {
        fprintf(stderr, "mip_sample_ex: null argument (triangle %d)\n", tri_id);
        return MIP_ERR_ARG;
    }
    if (level < 0 || level >= c->levels) {
        fprintf(stderr, "mip_sample_ex: invalid mip level %d (triangle %d)\n", level, tri_id);
        return MIP_ERR_LEVEL;
    }
    /* 关闭寻址（含 s==NULL）：与旧 mip_sample 逐像素相同。 */
    if (!s || !s->enabled) return sample_clamped(c, level, u, v, out);

    /* 先换算坐标，再按换算结果采样；全部参数校验集中在 mip_address。 */
    double tu, tv;
    int rc = mip_address(s, 0, u, tri_id, &tu);
    if (rc) return rc;
    rc = mip_address(s, 1, v, tri_id, &tv);
    if (rc) return rc;

    int x = (int)(tu * c->w[level]);
    int y = (int)(tv * c->h[level]);
    /* 纵深防御：换算契约保证 [0,1)，这里再夹一次杜绝任何越界读。 */
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= c->w[level]) x = c->w[level] - 1;
    if (y >= c->h[level]) y = c->h[level] - 1;
    for (int ch = 0; ch < 3; ++ch) out[ch] = c->pix[level][(y*c->w[level]+x)*3+ch];
    return 0;
}
