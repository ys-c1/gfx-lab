#include "mip.h"
#include <stdlib.h>
#include <math.h>

static int count_levels(int w, int h) {
    int levels = 1;
    while (w > 1 || h > 1) {
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
        ++levels;
    }
    return levels;
}

static int alloc_chain(MipChain *c, int w, int h) {
    c->levels = count_levels(w, h);
    c->w0 = w;
    c->h0 = h;
    c->pix = (unsigned char **)calloc((size_t)c->levels, sizeof(unsigned char *));
    c->lin = (float **)calloc((size_t)c->levels, sizeof(float *));
    c->w = (int *)calloc((size_t)c->levels, sizeof(int));
    c->h = (int *)calloc((size_t)c->levels, sizeof(int));
    if (!c->pix || !c->lin || !c->w || !c->h) return -1;
    c->w[0] = w;
    c->h[0] = h;
    for (int l = 1; l < c->levels; ++l) {
        c->w[l] = c->w[l - 1] > 1 ? c->w[l - 1] / 2 : 1;
        c->h[l] = c->h[l - 1] > 1 ? c->h[l - 1] / 2 : 1;
    }
    return 0;
}

/* 旧路径:编码空间字节金字塔,逻辑与原先逐字节一致 */
static int build_byte_pyramid(MipChain *c, const unsigned char *src) {
    int w = c->w[0], h = c->h[0];
    c->pix[0] = (unsigned char *)malloc((size_t)w * (size_t)h * 3);
    if (!c->pix[0]) return -1;
    for (int i = 0; i < w * h * 3; ++i) c->pix[0][i] = src[i];
    for (int l = 1; l < c->levels; ++l) {
        int pw = c->w[l - 1], ph = c->h[l - 1];
        int nw = c->w[l], nh = c->h[l];
        c->pix[l] = (unsigned char *)malloc((size_t)nw * (size_t)nh * 3);
        if (!c->pix[l]) return -1;
        for (int y = 0; y < nh; ++y)
            for (int x = 0; x < nw; ++x)
                for (int ch = 0; ch < 3; ++ch) {
                    int x0 = x * 2 < pw ? x * 2 : pw - 1, x1 = (x * 2 + 1) < pw ? x * 2 + 1 : pw - 1;
                    int y0 = y * 2 < ph ? y * 2 : ph - 1, y1 = (y * 2 + 1) < ph ? y * 2 + 1 : ph - 1;
                    int s = c->pix[l - 1][(y0 * pw + x0) * 3 + ch] + c->pix[l - 1][(y0 * pw + x1) * 3 + ch]
                          + c->pix[l - 1][(y1 * pw + x0) * 3 + ch] + c->pix[l - 1][(y1 * pw + x1) * 3 + ch];
                    c->pix[l][(y * nw + x) * 3 + ch] = (unsigned char)(s / 4);
                }
    }
    return 0;
}

/* 新路径:第一段,先解码到线性,盒式平均在线性空间做 */
static int build_lin_pyramid(MipChain *c, const unsigned char *src, double gamma) {
    int w = c->w[0], h = c->h[0];
    c->lin[0] = (float *)malloc((size_t)w * (size_t)h * 3 * sizeof(float));
    if (!c->lin[0]) return -1;
    for (int i = 0; i < w * h * 3; ++i)
        c->lin[0][i] = (float)pow(src[i] / 255.0, gamma);
    for (int l = 1; l < c->levels; ++l) {
        int pw = c->w[l - 1], ph = c->h[l - 1];
        int nw = c->w[l], nh = c->h[l];
        c->lin[l] = (float *)malloc((size_t)nw * (size_t)nh * 3 * sizeof(float));
        if (!c->lin[l]) return -1;
        for (int y = 0; y < nh; ++y)
            for (int x = 0; x < nw; ++x)
                for (int ch = 0; ch < 3; ++ch) {
                    int x0 = x * 2 < pw ? x * 2 : pw - 1, x1 = (x * 2 + 1) < pw ? x * 2 + 1 : pw - 1;
                    int y0 = y * 2 < ph ? y * 2 : ph - 1, y1 = (y * 2 + 1) < ph ? y * 2 + 1 : ph - 1;
                    float s = c->lin[l - 1][(y0 * pw + x0) * 3 + ch] + c->lin[l - 1][(y0 * pw + x1) * 3 + ch]
                            + c->lin[l - 1][(y1 * pw + x0) * 3 + ch] + c->lin[l - 1][(y1 * pw + x1) * 3 + ch];
                    c->lin[l][(y * nw + x) * 3 + ch] = s * 0.25f;
                }
    }
    return 0;
}

int mip_build_ex(MipChain *c, const unsigned char *src, int w, int h,
                 double gamma, MipError *err) {
    MipError local;
    if (!err) err = &local;
    err->code = MIP_OK;
    err->field = NULL;
    if (!c || !src || w <= 0 || h <= 0) {
        err->code = MIP_ERR_ARG;
        err->field = "src";
        return MIP_ERR_ARG;
    }
    if (!(gamma >= 0.0) || !isfinite(gamma)) {
        err->code = MIP_ERR_GAMMA;
        err->field = "gamma";
        return MIP_ERR_GAMMA;
    }
    c->levels = 0;
    c->pix = NULL;
    c->lin = NULL;
    c->w = NULL;
    c->h = NULL;
    c->gamma = gamma;
    int rc = alloc_chain(c, w, h);
    if (rc == 0)
        rc = gamma > 0.0 ? build_lin_pyramid(c, src, gamma) : build_byte_pyramid(c, src);
    if (rc != 0) {
        mip_free(c);
        err->code = MIP_ERR_ARG;
        err->field = "alloc";
        return MIP_ERR_ARG;
    }
    return MIP_OK;
}

int mip_build(MipChain *c, const unsigned char *src, int w, int h) {
    return mip_build_ex(c, src, w, h, 0.0, NULL) == MIP_OK ? 0 : -1;
}

void mip_free(MipChain *c) {
    if (!c) return;
    if (c->pix)
        for (int l = 0; l < c->levels; ++l) free(c->pix[l]);
    if (c->lin)
        for (int l = 0; l < c->levels; ++l) free(c->lin[l]);
    free(c->pix);
    free(c->lin);
    free(c->w);
    free(c->h);
    c->pix = NULL;
    c->lin = NULL;
    c->w = NULL;
    c->h = NULL;
    c->levels = 0;
    c->gamma = 0.0;
}

static void fetch_byte(const MipChain *c, int level, double u, double v, unsigned char out[3]) {
    int x = (int)(u * c->w[level]);
    int y = (int)(v * c->h[level]);
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= c->w[level]) x = c->w[level] - 1;
    if (y >= c->h[level]) y = c->h[level] - 1;
    const unsigned char *p = c->pix[level] + ((size_t)y * c->w[level] + x) * 3;
    out[0] = p[0];
    out[1] = p[1];
    out[2] = p[2];
}

static void fetch_lin(const MipChain *c, int level, double u, double v, float out[3]) {
    int x = (int)(u * c->w[level]);
    int y = (int)(v * c->h[level]);
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= c->w[level]) x = c->w[level] - 1;
    if (y >= c->h[level]) y = c->h[level] - 1;
    const float *p = c->lin[level] + ((size_t)y * c->w[level] + x) * 3;
    out[0] = p[0];
    out[1] = p[1];
    out[2] = p[2];
}

int mip_sample(const MipChain *c, int level, double u, double v, unsigned char out[3]) {
    if (!c || !out || level < 0 || level >= c->levels) return -1;
    fetch_byte(c, level, u, v, out);
    return 0;
}

int mip_sample_ex(const MipChain *c, double level, double u, double v,
                  unsigned char out[3], MipTrace *trace, MipError *err) {
    MipError local;
    if (!err) err = &local;
    err->code = MIP_OK;
    err->field = NULL;
    if (!c || !out) {
        err->code = MIP_ERR_ARG;
        err->field = "chain";
        return MIP_ERR_ARG;
    }
    if (c->levels <= 0) {
        err->code = MIP_ERR_LEVELS;
        err->field = "levels";
        return MIP_ERR_LEVELS;
    }
    if (!(level >= 0.0) || level > (double)(c->levels - 1)) {
        err->code = MIP_ERR_LEVEL;
        err->field = "level";
        return MIP_ERR_LEVEL;
    }
    int lo = (int)level;
    if (lo > c->levels - 1) lo = c->levels - 1;
    double t = level - lo;
    int hi = (t > 0.0 && lo + 1 < c->levels) ? lo + 1 : lo;

    if (c->gamma == 0.0) {
        /* 不转空间:与 mip_sample 完全一致 */
        fetch_byte(c, lo, u, v, out);
        if (trace) {
            trace->lo = lo;
            trace->hi = lo;
            trace->t = 0.0;
            trace->samples = 1;
        }
        return MIP_OK;
    }

    /* 第二段:相邻两层线性结果按 t 插值;第三段:编回 */
    float a[3], b[3];
    fetch_lin(c, lo, u, v, a);
    int ns = 1;
    if (hi != lo && t > 0.0) {
        fetch_lin(c, hi, u, v, b);
        ns = 2;
    } else {
        b[0] = a[0];
        b[1] = a[1];
        b[2] = a[2];
    }
    double inv = 1.0 / c->gamma;
    for (int ch = 0; ch < 3; ++ch) {
        double lin = a[ch] + ((double)b[ch] - a[ch]) * t;
        int v8 = (int)(pow(lin, inv) * 255.0 + 0.5);
        out[ch] = (unsigned char)(v8 < 0 ? 0 : v8 > 255 ? 255 : v8);
    }
    if (trace) {
        trace->lo = lo;
        trace->hi = hi;
        trace->t = t;
        trace->samples = ns;
    }
    return MIP_OK;
}
