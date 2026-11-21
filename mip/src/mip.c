#include "mip.h"
#include <stdlib.h>

/* 单张图像允许的最大边长：3*w*h 不得超过 size_t 范围，层数也受 MipPlan 缓冲限制。 */
#define MIP_MAX_DIM  (1 << 28)
#define MIP_MAX_LEVELS 64

static size_t level_dim(int d, int l) {
    size_t v = (size_t)d;
    for (int i = 0; i < l && v > 1; ++i) v /= 2;
    return v;
}

int mip_plan(int w, int h, size_t max_bytes, MipPlan *plan, int *err_idx) {
    if (err_idx) *err_idx = -1;
    if (!plan) { if (err_idx) *err_idx = 5; return -1; }
    if (w <= 0) { if (err_idx) *err_idx = 2; return -2; }
    if (h <= 0) { if (err_idx) *err_idx = 3; return -2; }
    if (w > MIP_MAX_DIM || h > MIP_MAX_DIM) {
        if (err_idx) *err_idx = (w > MIP_MAX_DIM) ? 2 : 3;
        return -2;
    }

    int full = 1;
    size_t ww = (size_t)w, hh = (size_t)h;
    while (ww > 1 || hh > 1) {
        if (ww > 1) ww /= 2;
        if (hh > 1) hh /= 2;
        ++full;
    }
    if (full > MIP_MAX_LEVELS) { if (err_idx) *err_idx = 2; return -2; }

    for (int l = 0; l < full; ++l)
        plan->level_bytes[l] = level_dim(w, l) * level_dim(h, l) * 3u;

    /* 第一段：按可用内存预算逐层累加，找到装得下的层数上限（至少保留第 0 层）。 */
    int cap = full;
    size_t total = 0;
    for (int l = 0; l < full; ++l) {
        size_t nb = plan->level_bytes[l];
        if (max_bytes != 0 && total + nb > max_bytes) { cap = l; break; }
        total += nb;
    }
    if (cap < 1) cap = 1; /* 预算连第 0 层都装不下：仍构建第 0 层并标记降级 */

    plan->full_levels = full;
    plan->cap_levels = cap;
    plan->degraded = cap < full;
    plan->total_bytes = 0;
    for (int l = 0; l < cap; ++l) plan->total_bytes += plan->level_bytes[l];
    return 0;
}

int mip_build(MipChain *c, const unsigned char *src, int w, int h, size_t max_bytes) {
    if (!c) return -1;
    int err_idx;
    MipPlan plan;
    int rc = mip_plan(w, h, max_bytes, &plan, &err_idx);
    if (rc != 0) {
        if (!src && rc == -2) return rc;      /* 非法尺寸优先按 -2 报 */
        if (!src) return -1;
        return rc;
    }
    if (!src) return -1;

    int cap = plan.cap_levels;
    c->levels = 0; c->cap_levels = cap; c->degraded = plan.degraded;
    c->w0 = w; c->h0 = h;
    c->pix = (unsigned char**)calloc((size_t)cap, sizeof(unsigned char*));
    c->w = (int*)calloc((size_t)cap, sizeof(int));
    c->h = (int*)calloc((size_t)cap, sizeof(int));
    if (!c->pix || !c->w || !c->h) { mip_free(c); return -1; }

    /* 第二段：按上限逐层分配、逐层生成，失败即停（已生成的层保持可用并标记降级）。 */
    c->w[0] = w; c->h[0] = h;
    c->pix[0] = (unsigned char*)malloc(plan.level_bytes[0]);
    if (!c->pix[0]) { mip_free(c); return -1; }
    for (size_t i = 0; i < plan.level_bytes[0]; ++i) c->pix[0][i] = src[i];
    c->levels = 1;

    for (int l = 1; l < cap; ++l) {
        int pw = c->w[l-1], ph = c->h[l-1];
        int nw = pw > 1 ? pw / 2 : 1, nh = ph > 1 ? ph / 2 : 1;
        unsigned char *dst = (unsigned char*)malloc(plan.level_bytes[l]);
        if (!dst) { c->degraded = 1; c->cap_levels = c->levels; return -1; }
        c->w[l] = nw; c->h[l] = nh; c->pix[l] = dst;
        for (int y = 0; y < nh; ++y)
            for (int x = 0; x < nw; ++x)
                for (int ch = 0; ch < 3; ++ch) {
                    int x0 = x*2 < pw ? x*2 : pw-1, x1 = (x*2+1) < pw ? x*2+1 : pw-1;
                    int y0 = y*2 < ph ? y*2 : ph-1, y1 = (y*2+1) < ph ? y*2+1 : ph-1;
                    int s = c->pix[l-1][(y0*pw+x0)*3+ch] + c->pix[l-1][(y0*pw+x1)*3+ch]
                          + c->pix[l-1][(y1*pw+x0)*3+ch] + c->pix[l-1][(y1*pw+x1)*3+ch];
                    dst[(y*nw+x)*3+ch] = (unsigned char)(s / 4);
                }
        ++c->levels;
    }
    return 0;
}

void mip_free(MipChain *c) {
    if (!c) return;
    if (c->pix) for (int l = 0; l < c->levels; ++l) free(c->pix[l]);
    free(c->pix); free(c->w); free(c->h);
    c->pix = NULL; c->w = NULL; c->h = NULL;
    c->levels = 0; c->cap_levels = 0; c->degraded = 0;
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
