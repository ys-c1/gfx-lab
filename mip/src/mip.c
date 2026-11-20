#include "mip.h"
#include <stdlib.h>
#include <math.h>

const char *mip_strerror(int err) {
    switch (err) {
    case MIP_OK: return "ok";
    case MIP_ERR_ARG: return "arg: null pointer or unbuilt chain";
    case MIP_ERR_LEVEL: return "level: out of range";
    case MIP_ERR_UV: return "u/v: not finite";
    case MIP_ERR_DUDX: return "dudx: not finite";
    case MIP_ERR_DVDX: return "dvdx: not finite";
    case MIP_ERR_DUDY: return "dudy: not finite";
    case MIP_ERR_DVDY: return "dvdy: not finite";
    default: return "unknown error";
    }
}

int mip_build(MipChain *c, const unsigned char *src, int w, int h) {
    if (!c || !src || w <= 0 || h <= 0) return MIP_ERR_ARG;
    int levels = 1, ww = w, hh = h;
    while (ww > 1 || hh > 1) { ww = ww > 1 ? ww / 2 : 1; hh = hh > 1 ? hh / 2 : 1; ++levels; }
    c->levels = levels;
    c->w0 = w; c->h0 = h;
    c->pix = (unsigned char**)calloc((size_t)levels, sizeof(unsigned char*));
    c->w = (int*)calloc((size_t)levels, sizeof(int));
    c->h = (int*)calloc((size_t)levels, sizeof(int));
    if (!c->pix || !c->w || !c->h) { mip_free(c); return MIP_ERR_ARG; }
    c->w[0] = w; c->h[0] = h;
    c->pix[0] = (unsigned char*)malloc((size_t)w * (size_t)h * 3);
    if (!c->pix[0]) { mip_free(c); return MIP_ERR_ARG; }
    for (int i = 0; i < w * h * 3; ++i) c->pix[0][i] = src[i];
    for (int l = 1; l < levels; ++l) {
        int pw = c->w[l-1], ph = c->h[l-1];
        int nw = pw > 1 ? pw / 2 : 1, nh = ph > 1 ? ph / 2 : 1;
        c->w[l] = nw; c->h[l] = nh;
        c->pix[l] = (unsigned char*)malloc((size_t)nw * (size_t)nh * 3);
        if (!c->pix[l]) { mip_free(c); return MIP_ERR_ARG; }
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
    return MIP_OK;
}

void mip_free(MipChain *c) {
    if (!c) return;
    for (int l = 0; l < c->levels; ++l) free(c->pix[l]);
    free(c->pix); free(c->w); free(c->h);
    c->pix = NULL; c->w = NULL; c->h = NULL; c->levels = 0;
}

int mip_sample(const MipChain *c, int level, double u, double v, unsigned char out[3]) {
    if (!c || !c->pix || !out) return MIP_ERR_ARG;
    if (level < 0 || level >= c->levels) return MIP_ERR_LEVEL;
    if (!isfinite(u) || !isfinite(v)) return MIP_ERR_UV;
    int x = (int)(u * c->w[level]);
    int y = (int)(v * c->h[level]);
    if (x < 0) x = 0; if (y < 0) y = 0;
    if (x >= c->w[level]) x = c->w[level] - 1;
    if (y >= c->h[level]) y = c->h[level] - 1;
    for (int ch = 0; ch < 3; ++ch) out[ch] = c->pix[level][(y*c->w[level]+x)*3+ch];
    return MIP_OK;
}

/* 按纹素空间足迹长度选层：log2 四舍五入，夹到 [0, levels-1]。
 * 用比较代替直接强转，texel_len 为 0/inf 也安全。 */
static int pick_level(const MipChain *c, double texel_len) {
    if (!(texel_len > 1.0)) return 0;
    double lam = log2(texel_len);
    if (lam >= (double)(c->levels - 1)) return c->levels - 1;
    return (int)(lam + 0.5);
}

int mip_sample_aniso(const MipChain *c, double u, double v,
                     double dudx, double dvdx, double dudy, double dvdy,
                     int max_aniso, unsigned char out[3], MipAnisoInfo *info) {
    if (!c || !c->pix || !out) return MIP_ERR_ARG;
    if (!isfinite(u) || !isfinite(v)) return MIP_ERR_UV;
    if (!isfinite(dudx)) return MIP_ERR_DUDX;
    if (!isfinite(dvdx)) return MIP_ERR_DVDX;
    if (!isfinite(dudy)) return MIP_ERR_DUDY;
    if (!isfinite(dvdy)) return MIP_ERR_DVDY;

    /* 像素足迹在纹素空间的两轴长度 */
    double lx = hypot(dudx * c->w0, dvdx * c->h0);
    double ly = hypot(dudy * c->w0, dvdy * c->h0);
    double major = lx >= ly ? lx : ly;
    double minor = lx >= ly ? ly : lx;

    int nmax = max_aniso > 1 ? max_aniso : 1;
    if (nmax > MIP_ANISO_CAP) nmax = MIP_ANISO_CAP;

    /* 沿长轴的子样本数：由长宽比决定，封顶 */
    int n = 1;
    if (nmax > 1) {
        if (minor > 0.0) {
            double ratio = major / minor;   /* >= 1，可能为 inf */
            if (ratio >= (double)nmax) n = nmax;
            else if (ratio > 1.0) n = (int)ceil(ratio);
        } else if (major > 0.0) {
            n = nmax;
        }
    }

    /* 选层：各向异性按短轴；关闭时按长轴（各向同性，与现状一致） */
    int level = pick_level(c, n > 1 ? minor : major);

    if (info) { info->level = level; info->nsamples = n; }

    if (n == 1)
        return mip_sample(c, level, u, v, out);

    /* 长轴方向（uv 空间），沿它均布 n 个子样本，覆盖整个像素足迹 */
    double du = lx >= ly ? dudx : dudy;
    double dv = lx >= ly ? dvdx : dvdy;

    int acc[3] = {0, 0, 0};
    unsigned char px[3];
    for (int i = 0; i < n; ++i) {
        double f = ((double)i + 0.5) / (double)n - 0.5;
        int rc = mip_sample(c, level, u + f * du, v + f * dv, px);
        if (rc != MIP_OK) return rc;
        acc[0] += px[0]; acc[1] += px[1]; acc[2] += px[2];
    }
    out[0] = (unsigned char)(acc[0] / n);
    out[1] = (unsigned char)(acc[1] / n);
    out[2] = (unsigned char)(acc[2] / n);
    return MIP_OK;
}
