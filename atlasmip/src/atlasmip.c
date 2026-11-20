#define _POSIX_C_SOURCE 200809L
#include "atlasmip.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define AM_MAGIC "AMID"
#define AM_VERSION 1

static long now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000000000L + ts.tv_nsec;
}

static int rect_in_bounds(const AmRect *r, int w, int h) {
    return r->x >= 0 && r->y >= 0 && r->w > 0 && r->h > 0 &&
           r->x + r->w <= w && r->y + r->h <= h;
}

/* ---------- 块边界索引 ---------- */

int am_index_build(const AmRect *rects, int n, int w, int h,
                   AmIndex *out, int *bad_block) {
    if (bad_block) *bad_block = -1;
    if (!rects || !out || n <= 0 || w <= 0 || h <= 0) return -1;
    for (int i = 0; i < n; ++i)
        if (!rect_in_bounds(&rects[i], w, h)) { if (bad_block) *bad_block = i; return -3; }
    out->rects = (AmRect*)malloc((size_t)n * sizeof(AmRect));
    if (!out->rects) return -1;
    out->n = n; out->w = w; out->h = h;
    memcpy(out->rects, rects, (size_t)n * sizeof(AmRect));
    return 0;
}

int am_index_write(const char *path, const AmIndex *idx) {
    if (!path || !idx || !idx->rects || idx->n <= 0) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) return -4;
    unsigned short ver = AM_VERSION, cnt = (unsigned short)idx->n;
    fwrite(AM_MAGIC, 1, 4, f);
    fwrite(&ver, 2, 1, f);
    fwrite(&cnt, 2, 1, f);
    fwrite(&idx->w, 4, 1, f);
    fwrite(&idx->h, 4, 1, f);
    if (fwrite(idx->rects, sizeof(AmRect), (size_t)idx->n, f) != (size_t)idx->n) {
        fclose(f); return -4;
    }
    fclose(f);
    return 0;
}

int am_index_read(const char *path, AmIndex *idx) {
    if (!path || !idx) return -1;
    memset(idx, 0, sizeof(*idx));
    FILE *f = fopen(path, "rb");
    if (!f) return -4;
    char magic[4];
    unsigned short ver = 0, cnt = 0;
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, AM_MAGIC, 4) != 0 ||
        fread(&ver, 2, 1, f) != 1 || ver != AM_VERSION ||
        fread(&cnt, 2, 1, f) != 1 ||
        fread(&idx->w, 4, 1, f) != 1 ||
        fread(&idx->h, 4, 1, f) != 1) { fclose(f); return -4; }
    idx->n = cnt;
    idx->rects = (AmRect*)calloc((size_t)(cnt ? cnt : 1), sizeof(AmRect));
    if (!idx->rects) { fclose(f); return -1; }
    if (cnt && fread(idx->rects, sizeof(AmRect), cnt, f) != cnt) {
        fclose(f); am_index_free(idx); return -4;
    }
    fclose(f);
    return 0;
}

void am_index_free(AmIndex *idx) {
    if (!idx) return;
    free(idx->rects);
    idx->rects = NULL; idx->n = 0;
}

/* ---------- 盒式降采样工具（与 mip 模块同一约定：2x2 平均，越界钳制） ---------- */

static void box_down(const unsigned char *src, int sw, int sh,
                     unsigned char *dst, int dw, int dh,
                     int ox, int oy) {
    /* (ox,oy) 为子矩形在整图中的原点；所有坐标先钳制在 [0,sw)x[0,sh) 内。 */
    for (int y = 0; y < dh; ++y)
        for (int x = 0; x < dw; ++x)
            for (int ch = 0; ch < 3; ++ch) {
                int gx0 = ox + x * 2, gx1 = ox + x * 2 + 1;
                int gy0 = oy + y * 2, gy1 = oy + y * 2 + 1;
                gx0 = gx0 < sw ? gx0 : sw - 1; gx1 = gx1 < sw ? gx1 : sw - 1;
                gy0 = gy0 < sh ? gy0 : sh - 1; gy1 = gy1 < sh ? gy1 : sh - 1;
                int s = src[(gy0*sw+gx0)*3+ch] + src[(gy0*sw+gx1)*3+ch]
                      + src[(gy1*sw+gx0)*3+ch] + src[(gy1*sw+gx1)*3+ch];
                dst[(y*dw+x)*3+ch] = (unsigned char)(s / 4);
            }
}

/* 朴素全图链：直接在整图尺寸上盒式降采样（渗色对照基准）。 */
static int build_naive(AtlasMip *am, const unsigned char *src) {
    int w = am->idx.w, h = am->idx.h;
    int levels = 1, ww = w, hh = h;
    while (ww > 1 || hh > 1) {
        ww = ww > 1 ? ww / 2 : 1; hh = hh > 1 ? hh / 2 : 1; ++levels;
    }
    am->naive_levels = levels;
    am->naive_w = (int*)calloc((size_t)levels, sizeof(int));
    am->naive_h = (int*)calloc((size_t)levels, sizeof(int));
    am->naive_pix = (unsigned char**)calloc((size_t)levels, sizeof(unsigned char*));
    if (!am->naive_w || !am->naive_h || !am->naive_pix) return -1;
    am->naive_w[0] = w; am->naive_h[0] = h;
    am->naive_pix[0] = (unsigned char*)malloc((size_t)w * (size_t)h * 3);
    if (!am->naive_pix[0]) return -1;
    memcpy(am->naive_pix[0], src, (size_t)w * (size_t)h * 3);
    for (int l = 1; l < levels; ++l) {
        int pw = am->naive_w[l-1], ph = am->naive_h[l-1];
        int nw = pw > 1 ? pw / 2 : 1, nh = ph > 1 ? ph / 2 : 1;
        am->naive_w[l] = nw; am->naive_h[l] = nh;
        am->naive_pix[l] = (unsigned char*)malloc((size_t)nw * (size_t)nh * 3);
        if (!am->naive_pix[l]) return -1;
        box_down(am->naive_pix[l-1], pw, ph, am->naive_pix[l], nw, nh, 0, 0);
    }
    return 0;
}

/* ---------- 阶段 2：为一块单独建层链，所有降采样在块内钳制 ---------- */

static int build_block_chain(AtlasMip *am, int bi) {
    const AmRect *r = &am->idx.rects[bi];
    int levels = am->levels;
    AmBlockChain *c = &am->chains[bi];
    c->levels = levels;
    c->w = (int*)calloc((size_t)levels, sizeof(int));
    c->h = (int*)calloc((size_t)levels, sizeof(int));
    c->pix = (unsigned char**)calloc((size_t)levels, sizeof(unsigned char*));
    if (!c->w || !c->h || !c->pix) return -1;
    c->w[0] = r->w; c->h[0] = r->h;
    /* 第 0 层直接从扩展基底取块内像素（extend=0 时基底就是原图，块内值不变）。 */
    c->pix[0] = (unsigned char*)malloc((size_t)r->w * (size_t)r->h * 3);
    if (!c->pix[0]) return -1;
    for (int y = 0; y < r->h; ++y)
        memcpy(c->pix[0] + (size_t)y * r->w * 3,
               am->extended + ((size_t)(r->y + y) * am->idx.w + r->x) * 3,
               (size_t)r->w * 3);
    for (int l = 1; l < levels; ++l) {
        int pw = c->w[l-1], ph = c->h[l-1];
        int nw = pw > 1 ? pw / 2 : 1, nh = ph > 1 ? ph / 2 : 1;
        c->w[l] = nw; c->h[l] = nh;
        c->pix[l] = (unsigned char*)malloc((size_t)nw * (size_t)nh * 3);
        if (!c->pix[l]) return -1;
        /* 原点传 0：源图就是该块自己的上一层，索引天然不会跨出块。 */
        box_down(c->pix[l-1], pw, ph, c->pix[l], nw, nh, 0, 0);
    }
    return 0;
}

static void free_chains(AtlasMip *am) {
    if (!am->chains) return;
    for (int i = 0; i < am->idx.n; ++i) {
        AmBlockChain *c = &am->chains[i];
        for (int l = 0; l < c->levels; ++l) free(c->pix[l]);
        free(c->pix); free(c->w); free(c->h);
    }
    free(am->chains);
    am->chains = NULL;
}

static void free_naive(AtlasMip *am) {
    for (int l = 0; l < am->naive_levels; ++l) free(am->naive_pix[l]);
    free(am->naive_pix); free(am->naive_w); free(am->naive_h);
    am->naive_pix = NULL; am->naive_w = NULL; am->naive_h = NULL;
    am->naive_levels = 0;
}

/* ---------- 阶段 1：贴边扩展 ---------- */

/* 块外像素取最近块的块边像素（各轴独立 clamp），块内像素原样保留。
 * 平局按块序号最小、再按距离顺序确定性打破。 */
static void edge_extend(AtlasMip *am, const unsigned char *src, int extend) {
    int W = am->idx.w, H = am->idx.h, n = am->idx.n;
    am->extended = (unsigned char*)malloc((size_t)W * (size_t)H * 3);
    if (!am->extended) return;
    memcpy(am->extended, src, (size_t)W * (size_t)H * 3);
    if (!extend) return;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            int inside = 0;
            for (int i = 0; i < n; ++i) {
                const AmRect *r = &am->idx.rects[i];
                if (x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h) { inside = 1; break; }
            }
            if (inside) continue;
            int best = -1, best_d = 0, best_gx = 0, best_gy = 0;
            for (int i = 0; i < n; ++i) {
                const AmRect *r = &am->idx.rects[i];
                int gx = x < r->x ? r->x : (x >= r->x + r->w ? r->x + r->w - 1 : x);
                int gy = y < r->y ? r->y : (y >= r->y + r->h ? r->y + r->h - 1 : y);
                int d = abs(gx - x) + abs(gy - y);
                if (best < 0 || d < best_d) { best = i; best_d = d; best_gx = gx; best_gy = gy; }
            }
            if (best >= 0)
                memcpy(am->extended + ((size_t)y * W + x) * 3,
                       src + ((size_t)best_gy * W + best_gx) * 3, 3);
        }
}

/* ---------- 主入口 ---------- */

int am_build(AtlasMip *am, const unsigned char *src,
             const AmRect *rects, int n, int w, int h,
             int levels, int extend, int *bad_block) {
    long t0, t1;
    if (bad_block) *bad_block = -1;
    if (!am || !src || !rects || n <= 0 || w <= 0 || h <= 0) return -1;
    memset(am, 0, sizeof(*am));
    if (levels <= 0) return -2;
    int rc = am_index_build(rects, n, w, h, &am->idx, bad_block);
    if (rc != 0) return rc;

    t0 = now_ns();
    edge_extend(am, src, extend);
    if (!am->extended) { am_index_free(&am->idx); return -1; }
    am->extend = extend ? 1 : 0;
    t1 = now_ns(); am->ns_extend = t1 - t0;

    am->levels = levels;
    am->chains = (AmBlockChain*)calloc((size_t)n, sizeof(AmBlockChain));
    if (!am->chains) { am_free(am); return -1; }
    t0 = now_ns();
    for (int i = 0; i < n; ++i)
        if (build_block_chain(am, i) != 0) { am_free(am); return -1; }
    t1 = now_ns(); am->ns_chains = t1 - t0;

    t0 = now_ns();
    if (build_naive(am, src) != 0) { am_free(am); return -1; }
    t1 = now_ns(); am->ns_naive = t1 - t0;
    return 0;
}

/* ---------- 阶段 3：按块裁剪取样 ---------- */

int am_sample(const AtlasMip *am, int block, double u, double v,
              int level, unsigned char out[3]) {
    if (!am || !out) return -1;
    if (block < 0 || block >= am->idx.n) return -3;
    if (level < 0 || level >= am->levels) return -2;
    if (u < 0.0) u = 0.0; else if (u > 1.0) u = 1.0;
    if (v < 0.0) v = 0.0; else if (v > 1.0) v = 1.0;
    const AmBlockChain *c = &am->chains[block];
    int x = (int)(u * c->w[level]);
    int y = (int)(v * c->h[level]);
    if (x >= c->w[level]) x = c->w[level] - 1;
    if (y >= c->h[level]) y = c->h[level] - 1;
    memcpy(out, c->pix[level] + ((size_t)y * c->w[level] + x) * 3, 3);
    return 0;
}

/* ---------- 渗色度量 ---------- */

int am_measure(const AtlasMip *am, int level, AmReport *rep) {
    if (!am || !rep) return -1;
    if (level < 0 || level >= am->naive_levels) return -2;
    memset(rep, 0, sizeof(*rep));
    int nw = am->naive_w[level], nh = am->naive_h[level];
    const unsigned char *npix = am->naive_pix[level];
    for (int ny = 0; ny < nh; ++ny)
        for (int nx = 0; nx < nw; ++nx) {
            /* footprint 中心映射回基底找所属块。中心=(i+0.5)*2^L=(2i+1)*2^(L-1)，
               span 为 2 的幂，中心恒为整数，纯整数计算即可，无浮点取整歧义。 */
            int span = 1 << level;
            int cx = (2 * nx + 1) * span / 2;
            int cy = (2 * ny + 1) * span / 2;
            if (cx >= am->idx.w) cx = am->idx.w - 1;
            if (cy >= am->idx.h) cy = am->idx.h - 1;
            int bi = -1;
            for (int i = 0; i < am->idx.n; ++i) {
                const AmRect *r = &am->idx.rects[i];
                if (cx >= r->x && cx < r->x + r->w && cy >= r->y && cy < r->y + r->h) { bi = i; break; }
            }
            if (bi < 0) continue;                 /* 中心落在块间留白：无可信块链对照 */
            const AmRect *r = &am->idx.rects[bi];
            if (level >= am->chains[bi].levels) continue;
            /* 块内第 L 层像素坐标：基底中块内位置右移 L 位，尺寸在 1 处钳制。 */
            int lx = (cx - r->x) >> level;
            int ly = (cy - r->y) >> level;
            if (lx >= am->chains[bi].w[level]) lx = am->chains[bi].w[level] - 1;
            if (ly >= am->chains[bi].h[level]) ly = am->chains[bi].h[level] - 1;
            const unsigned char *cp = am->chains[bi].pix[level]
                                    + ((size_t)ly * am->chains[bi].w[level] + lx) * 3;
            const unsigned char *qp = npix + ((size_t)ny * nw + nx) * 3;
            ++rep->pixels;
            int diff = 0;
            for (int ch = 0; ch < 3; ++ch) {
                int e = abs((int)qp[ch] - (int)cp[ch]);
                rep->err_sum += e;
                if (e > rep->err_max) rep->err_max = e;
                if (e) diff = 1;
            }
            if (diff) ++rep->bleed_px;
        }
    return 0;
}

void am_free(AtlasMip *am) {
    if (!am) return;
    free_chains(am);
    free_naive(am);
    free(am->extended);
    am->extended = NULL;
    am_index_free(&am->idx);
}
