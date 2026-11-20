#include "mip.h"
#include <stdlib.h>
#include <math.h>

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

double mip_lod_from_derivs(const MipChain *c,
                           double dudx, double dudy, double dvdx, double dvdy,
                           const char **err_field) {
    if (err_field) *err_field = NULL;
    if (!c) return -1.0;
    if (!isfinite(dudx)) { if (err_field) *err_field = "dudx"; return -1.0; }
    if (!isfinite(dudy)) { if (err_field) *err_field = "dudy"; return -1.0; }
    if (!isfinite(dvdx)) { if (err_field) *err_field = "dvdx"; return -1.0; }
    if (!isfinite(dvdy)) { if (err_field) *err_field = "dvdy"; return -1.0; }
    double fx = hypot(dudx * c->w0, dvdx * c->h0);
    double fy = hypot(dudy * c->w0, dvdy * c->h0);
    double foot = fx > fy ? fx : fy;
    double lod = foot > 1.0 ? log2(foot) : 0.0;
    double max_lod = (double)(c->levels - 1);
    return lod > max_lod ? max_lod : lod;
}

int mip_lod_split(const MipChain *c, double lod, int *level_lo, double *weight,
                  const char **err_field) {
    if (err_field) *err_field = NULL;
    if (!c || !level_lo || !weight) return -1;
    if (!isfinite(lod)) { if (err_field) *err_field = "lod"; return -1; }
    if (lod < 0.0) lod = 0.0;
    double max_lod = (double)(c->levels - 1);
    if (lod > max_lod) lod = max_lod;
    int lo = (int)floor(lod);
    if (lo >= c->levels - 1) { *level_lo = c->levels - 1; *weight = 0.0; }
    else { *level_lo = lo; *weight = lod - (double)lo; }
    return 0;
}

/* 单层双线性，结果以 double 返回，读取纹素数计入 *reads */
static void bilinear(const MipChain *c, int level, double u, double v,
                     double out[3], int *reads) {
    int w = c->w[level], h = c->h[level];
    double x = u * w - 0.5, y = v * h - 0.5;
    int x0, y0, x1, y1;
    double fx, fy;
    if (x <= 0.0) { x0 = 0; fx = 0.0; }
    else if (x >= (double)(w - 1)) { x0 = w - 1; fx = 0.0; }
    else { x0 = (int)floor(x); fx = x - (double)x0; }
    if (y <= 0.0) { y0 = 0; fy = 0.0; }
    else if (y >= (double)(h - 1)) { y0 = h - 1; fy = 0.0; }
    else { y0 = (int)floor(y); fy = y - (double)y0; }
    x1 = x0 + 1 < w ? x0 + 1 : w - 1;
    y1 = y0 + 1 < h ? y0 + 1 : h - 1;
    const unsigned char *p = c->pix[level];
    const unsigned char *t00 = p + (y0*w + x0)*3;
    const unsigned char *t10 = p + (y0*w + x1)*3;
    const unsigned char *t01 = p + (y1*w + x0)*3;
    const unsigned char *t11 = p + (y1*w + x1)*3;
    *reads += 4;
    for (int ch = 0; ch < 3; ++ch) {
        double top = t00[ch] + (t10[ch] - t00[ch]) * fx;
        double bot = t01[ch] + (t11[ch] - t01[ch]) * fx;
        out[ch] = top + (bot - top) * fy;
    }
}

static unsigned char to_byte(double x) {
    if (x <= 0.0) return 0;
    if (x >= 255.0) return 255;
    return (unsigned char)(x + 0.5);
}

int mip_sample_trilinear(const MipChain *c, int level_lo, double weight,
                         double u, double v, int blend,
                         unsigned char out[3], MipTrace *trace,
                         const char **err_field) {
    if (err_field) *err_field = NULL;
    if (!c || !out) return -1;
    if (level_lo < 0 || level_lo >= c->levels) {
        if (err_field) *err_field = "level_lo";
        return -1;
    }
    if (!isfinite(weight) || weight < 0.0 || weight > 1.0) {
        if (err_field) *err_field = "weight";
        return -1;
    }
    if (weight > 0.0 && level_lo + 1 >= c->levels) {
        if (err_field) *err_field = "levels";
        return -1;
    }
    if (!isfinite(u)) { if (err_field) *err_field = "u"; return -1; }
    if (!isfinite(v)) { if (err_field) *err_field = "v"; return -1; }

    if (!blend) {
        /* 与 mip_sample 逐像素一致的旧路径 */
        int rc = mip_sample(c, level_lo, u, v, out);
        if (rc == 0 && trace) {
            trace->level_lo = level_lo;
            trace->level_hi = level_lo;
            trace->weight = 0.0;
            trace->texel_reads = 1;
            for (int ch = 0; ch < 3; ++ch) {
                trace->lo[ch] = out[ch];
                trace->hi[ch] = out[ch];
            }
        }
        return rc;
    }

    int reads = 0;
    double lo[3], hi[3];
    bilinear(c, level_lo, u, v, lo, &reads);
    if (weight > 0.0) {
        bilinear(c, level_lo + 1, u, v, hi, &reads);
        for (int ch = 0; ch < 3; ++ch)
            out[ch] = to_byte(lo[ch] + (hi[ch] - lo[ch]) * weight);
    } else {
        for (int ch = 0; ch < 3; ++ch) hi[ch] = lo[ch];
        for (int ch = 0; ch < 3; ++ch) out[ch] = to_byte(lo[ch]);
    }
    if (trace) {
        trace->level_lo = level_lo;
        trace->level_hi = weight > 0.0 ? level_lo + 1 : level_lo;
        trace->weight = weight;
        trace->texel_reads = reads;
        for (int ch = 0; ch < 3; ++ch) {
            trace->lo[ch] = to_byte(lo[ch]);
            trace->hi[ch] = to_byte(hi[ch]);
        }
    }
    return 0;
}
