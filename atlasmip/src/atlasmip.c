#include "atlasmip.h"
#include <stdlib.h>
#include <math.h>

/* ---- 与 mip 模块完全相同的盒式金字塔实现（保证老路径字节一致） ---- */

typedef struct {
    int levels;
    int w0, h0;
    unsigned char **pix;
    int *w, *h;
} Chain;

static int chain_build(Chain *c, const unsigned char *src, int w, int h) {
    if (!c || !src || w <= 0 || h <= 0) return -1;
    int levels = 1, ww = w, hh = h;
    while (ww > 1 || hh > 1) { ww = ww > 1 ? ww / 2 : 1; hh = hh > 1 ? hh / 2 : 1; ++levels; }
    c->levels = levels;
    c->w0 = w; c->h0 = h;
    c->pix = (unsigned char**)calloc((size_t)levels, sizeof(unsigned char*));
    c->w = (int*)calloc((size_t)levels, sizeof(int));
    c->h = (int*)calloc((size_t)levels, sizeof(int));
    if (!c->pix || !c->w || !c->h) return -1;
    c->w[0] = w; c->h[0] = h;
    c->pix[0] = (unsigned char*)malloc((size_t)w * (size_t)h * 3);
    if (!c->pix[0]) return -1;
    for (int i = 0; i < w * h * 3; ++i) c->pix[0][i] = src[i];
    for (int l = 1; l < levels; ++l) {
        int pw = c->w[l-1], ph = c->h[l-1];
        int nw = pw > 1 ? pw / 2 : 1, nh = ph > 1 ? ph / 2 : 1;
        c->w[l] = nw; c->h[l] = nh;
        c->pix[l] = (unsigned char*)malloc((size_t)nw * (size_t)nh * 3);
        if (!c->pix[l]) return -1;
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

static void chain_free(Chain *c) {
    if (!c || !c->pix) return;
    for (int l = 0; l < c->levels; ++l) free(c->pix[l]);
    free(c->pix); free(c->w); free(c->h);
    c->pix = NULL; c->w = NULL; c->h = NULL; c->levels = 0;
}

/* 老路径取样：与 mip_sample 同样的取整与钳制规则。 */
static int chain_sample_clamped(const Chain *c, int level, double u, double v,
                                unsigned char out[3]) {
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

/* 严格取样：u/v 必须落在 [0,1)，越界不钳制，返回 AM_ERR_UV。 */
static int chain_sample_strict(const Chain *c, int level, double u, double v,
                               unsigned char out[3]) {
    if (u < 0.0 || u >= 1.0 || v < 0.0 || v >= 1.0 || isnan(u) || isnan(v))
        return AM_ERR_UV;
    int x = (int)(u * c->w[level]);
    int y = (int)(v * c->h[level]);
    if (x >= c->w[level]) x = c->w[level] - 1;
    if (y >= c->h[level]) y = c->h[level] - 1;
    for (int ch = 0; ch < 3; ++ch) out[ch] = c->pix[level][(y*c->w[level]+x)*3+ch];
    return AM_OK;
}

/* ---- AtlasMip 对象 ---- */

struct AtlasMip {
    int aw, ah;
    int n;
    AmBlock *blocks;
    int use_blocks;
    Chain full;                 /* 整图老链（两种模式都建） */
    Chain *chains;              /* n 条逐块层链；老模式为 NULL */
    long *chain_bytes;          /* 每条块层链的单独内存（字节） */
    AmScaleStat stats[AM_SCALE_N];
    AmOob oob;
    int have_oob;
};

static long chain_pixel_bytes(const Chain *c) {
    long total = 0;
    for (int l = 0; l < c->levels; ++l)
        total += (long)c->w[l] * (long)c->h[l] * 3;
    return total;
}

int am_build(AtlasMip **out, const unsigned char *src, int w, int h,
             const AmBlock *blocks, int n, int use_block_chains) {
    if (!out) return AM_ERR_NULL;
    *out = NULL;
    if (!src || !blocks) return AM_ERR_NULL;
    if (w <= 0 || h <= 0 || n <= 0) return AM_ERR_PARAM;

    AtlasMip *am = (AtlasMip*)calloc(1, sizeof(AtlasMip));
    if (!am) return AM_ERR_NOMEM;
    am->aw = w; am->ah = h; am->n = n;
    am->have_oob = 0;

    am->blocks = (AmBlock*)malloc((size_t)n * sizeof(AmBlock));
    if (!am->blocks) { am_free(am); return AM_ERR_NOMEM; }
    for (int i = 0; i < n; ++i) {
        am->blocks[i] = blocks[i];
        const AmBlock *b = &blocks[i];
        if (b->w <= 0 || b->h <= 0 || b->x < 0 || b->y < 0 ||
            b->x + b->w > w || b->y + b->h > h) {
            /* 保留对象，调用方可经 *out + am_last_oob() 查出坏块编号。 */
            am->oob.code = AM_ERR_BLOCK_RECT;
            am->oob.block = i;
            am->oob.level = -1;
            am->have_oob = 1;
            *out = am;
            return AM_ERR_BLOCK_RECT;
        }
    }

    /* 第一段（老路径）：整图层链，两种模式都构建，用于对照与回退。 */
    if (chain_build(&am->full, src, w, h) != 0) { am_free(am); return AM_ERR_NOMEM; }

    am->use_blocks = use_block_chains ? 1 : 0;
    if (use_block_chains) {
        am->chains = (Chain*)calloc((size_t)n, sizeof(Chain));
        am->chain_bytes = (long*)calloc((size_t)n, sizeof(long));
        if (!am->chains || !am->chain_bytes) { am_free(am); return AM_ERR_NOMEM; }
        for (int i = 0; i < n; ++i) {
            const AmBlock *b = &am->blocks[i];
            unsigned char *sub = (unsigned char*)malloc((size_t)b->w * (size_t)b->h * 3);
            if (!sub) { am_free(am); return AM_ERR_NOMEM; }
            /* 第一段：按块从整图拷贝块像素（不跨块）。 */
            for (int yy = 0; yy < b->h; ++yy)
                for (int xx = 0; xx < b->w; ++xx) {
                    const unsigned char *sp = src + (((b->y + yy) * w + (b->x + xx)) * 3);
                    unsigned char *dp = sub + ((yy * b->w + xx) * 3);
                    dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2];
                }
            if (chain_build(&am->chains[i], sub, b->w, b->h) != 0) {
                free(sub); am_free(am); return AM_ERR_NOMEM;
            }
            free(sub);
            am->chain_bytes[i] = chain_pixel_bytes(&am->chains[i]);
        }
    }
    *out = am;
    return AM_OK;
}

void am_free(AtlasMip *am) {
    if (!am) return;
    if (am->chains) {
        for (int i = 0; i < am->n; ++i) chain_free(&am->chains[i]);
        free(am->chains);
    }
    free(am->chain_bytes);
    chain_free(&am->full);
    free(am->blocks);
    free(am);
}

static int record_oob(AtlasMip *am, int code, int block, int level) {
    am->oob.code = code;
    am->oob.block = block;
    am->oob.level = level;
    am->have_oob = 1;
    return code;
}

int am_sample(AtlasMip *am, int block, int level, double u, double v,
              unsigned char out[3]) {
    if (!am || !out) return AM_ERR_NULL;
    if (block < 0 || block >= am->n)
        return record_oob(am, AM_ERR_BLOCK, block, -1);
    if (am->use_blocks) {
        if (level < 0 || level >= am->chains[block].levels)
            return record_oob(am, AM_ERR_LEVEL, block, level);
        int rc = chain_sample_strict(&am->chains[block], level, u, v, out);
        if (rc == AM_ERR_UV) return record_oob(am, AM_ERR_UV, block, level);
        return AM_OK;
    }
    /* 老路径：块号 + 块内 u/v 先合成全局归一化坐标，再走整图老链，
     * 取整/钳制全部委托给与 mip_sample 同实现的 chain_sample_clamped，
     * 保证与 mip 模块逐字节一致（不在块内先取整，避免取整误差）。 */
    if (level < 0 || level >= am->full.levels)
        return record_oob(am, AM_ERR_LEVEL, block, level);
    const AmBlock *b = &am->blocks[block];
    double gu = ((double)b->x + u * (double)b->w) / (double)am->aw;
    double gv = ((double)b->y + v * (double)b->h) / (double)am->ah;
    return chain_sample_clamped(&am->full, level, gu, gv, out) == 0
               ? AM_OK : record_oob(am, AM_ERR_LEVEL, block, level);
}

int am_sample_scaled(AtlasMip *am, int block, AmScale scale, double u, double v,
                     unsigned char out[3]) {
    if (!am || !out) return AM_ERR_NULL;
    if (scale < 0 || scale >= AM_SCALE_N) return AM_ERR_PARAM;
    if (block < 0 || block >= am->n)
        return record_oob(am, AM_ERR_BLOCK, block, -1);

    int level = (scale == AM_SCALE_2X) ? 1 : 0;

    if (!am->use_blocks) {
        int rc = am_sample(am, block, level, u, v, out);
        if (rc == AM_OK) am->stats[scale].samples++;
        return rc;
    }

    /* 块链不足两层时钳到末层（例如 1 像素宽的块只有 1 层）。 */
    int clevel = level;
    if (clevel >= am->chains[block].levels) clevel = am->chains[block].levels - 1;
    unsigned char nb[3];
    int rc = am_sample(am, block, clevel, u, v, nb);
    if (rc != AM_OK) return rc;

    /* 同位置老路径取值，用于误差统计（老链层号不钳制，按全局坐标）。 */
    unsigned char old[3];
    int flevel = level < am->full.levels ? level : am->full.levels - 1;
    const AmBlock *b = &am->blocks[block];
    chain_sample_clamped(&am->full, flevel,
                         ((double)b->x + u * b->w) / (double)am->aw,
                         ((double)b->y + v * b->h) / (double)am->ah, old);

    AmScaleStat *st = &am->stats[scale];
    st->samples++;
    for (int ch = 0; ch < 3; ++ch) {
        int d = (int)nb[ch] - (int)old[ch];
        if (d < 0) d = -d;
        st->abs_err_sum += (unsigned long)d;
        if (d > st->max_err) st->max_err = d;
    }
    out[0] = nb[0]; out[1] = nb[1]; out[2] = nb[2];
    return AM_OK;
}

int am_level_info(const AtlasMip *am, int block, int level, int *lw, int *lh) {
    if (!am) return AM_ERR_NULL;
    if (block < 0 || block >= am->n) return AM_ERR_BLOCK;
    const Chain *c = am->use_blocks ? &am->chains[block] : &am->full;
    if (level < 0 || level >= c->levels) return AM_ERR_LEVEL;
    if (lw) *lw = c->w[level];
    if (lh) *lh = c->h[level];
    return AM_OK;
}

int am_level_count(const AtlasMip *am, int block) {
    if (!am || block < 0 || block >= am->n) return -1;
    return am->use_blocks ? am->chains[block].levels : am->full.levels;
}

long am_chain_bytes(const AtlasMip *am, int block) {
    if (!am || block < 0 || block >= am->n) return -1;
    if (!am->use_blocks) return 0;
    return am->chain_bytes[block];
}

long am_total_chain_bytes(const AtlasMip *am) {
    if (!am) return -1;
    long total = 0;
    if (am->use_blocks)
        for (int i = 0; i < am->n; ++i) total += am->chain_bytes[i];
    return total;
}

long am_full_chain_bytes(const AtlasMip *am) {
    if (!am) return -1;
    return chain_pixel_bytes(&am->full);
}

int am_block_count(const AtlasMip *am) { return am ? am->n : -1; }
int am_uses_block_chains(const AtlasMip *am) { return am ? am->use_blocks : -1; }

const AmScaleStat *am_scale_stats(const AtlasMip *am, AmScale scale) {
    if (!am || scale < 0 || scale >= AM_SCALE_N) return NULL;
    return &am->stats[scale];
}

const AmOob *am_last_oob(const AtlasMip *am) {
    if (!am || !am->have_oob) return NULL;
    return &am->oob;
}
