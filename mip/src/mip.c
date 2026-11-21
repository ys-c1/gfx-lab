#include "mip.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* ------------------------------------------------------------------
 * 原有接口：行为与返回码保持不变
 * ------------------------------------------------------------------ */

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

/* ------------------------------------------------------------------
 * 内部错误记录
 * ------------------------------------------------------------------ */

static void err_clear(MipError *err) {
    if (!err) return;
    err->code = MIP_OK;
    err->field = NULL;
    err->index = -1;
    err->level = -1;
}

static void err_set(MipError *err, int code, const char *field, int index, int level) {
    if (!err) return;
    err->code = code;
    err->field = field;
    err->index = index;
    err->level = level;
}

unsigned int mip_checksum(const unsigned char *data, size_t bytes) {
    unsigned int h = 2166136261u;
    for (size_t i = 0; i < bytes; ++i) {
        h ^= data[i];
        h *= 16777619u;
    }
    return h;
}

/* ------------------------------------------------------------------
 * 层表
 * ------------------------------------------------------------------ */

int mip_layer_table_build(const MipChain *c, MipLayerTable *t, MipError *err) {
    err_clear(err);
    if (!c || !t) {
        err_set(err, MIP_ERR_ARG, c ? "table" : "chain", -1, -1);
        return MIP_ERR_ARG;
    }
    if (c->levels <= 0 || !c->pix || !c->w || !c->h) {
        err_set(err, MIP_ERR_CACHE, "chain", -1, -1);
        return MIP_ERR_CACHE;
    }
    MipLayerTable built;
    built.count = c->levels;
    built.layers = (MipLayer*)calloc((size_t)built.count, sizeof(MipLayer));
    built.checksum = (unsigned int*)calloc((size_t)built.count, sizeof(unsigned int));
    if (!built.layers || !built.checksum) {
        free(built.layers);
        free(built.checksum);
        err_set(err, MIP_ERR_MEM, "table", -1, -1);
        return MIP_ERR_MEM;
    }
    for (int l = 0; l < built.count; ++l) {
        if (!c->pix[l] || c->w[l] <= 0 || c->h[l] <= 0) {
            mip_layer_table_free(&built);
            err_set(err, MIP_ERR_LEVEL, "chain", -1, l);
            return MIP_ERR_LEVEL;
        }
        MipLayer *layer = &built.layers[l];
        layer->level = l;
        layer->w = c->w[l];
        layer->h = c->h[l];
        layer->bytes = (size_t)c->w[l] * (size_t)c->h[l] * 3;
        layer->pix = c->pix[l];
        built.checksum[l] = mip_checksum(layer->pix, layer->bytes);
    }
    *t = built;
    return MIP_OK;
}

void mip_layer_table_free(MipLayerTable *t) {
    if (!t) return;
    free(t->layers);
    free(t->checksum);
    t->layers = NULL;
    t->checksum = NULL;
    t->count = 0;
}

int mip_layer_table_write_csv(const MipLayerTable *t, const char *path, MipError *err) {
    err_clear(err);
    if (!t || !path) {
        err_set(err, MIP_ERR_ARG, t ? "path" : "table", -1, -1);
        return MIP_ERR_ARG;
    }
    if (t->count < 0 || (t->count > 0 && (!t->layers || !t->checksum))) {
        err_set(err, MIP_ERR_CACHE, "table", -1, -1);
        return MIP_ERR_CACHE;
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        err_set(err, MIP_ERR_IO, "path", -1, -1);
        return MIP_ERR_IO;
    }
    fprintf(f, "level,width,height,bytes,checksum_fnv1a\n");
    for (int i = 0; i < t->count; ++i)
        fprintf(f, "%d,%d,%d,%lu,0x%08x\n",
                t->layers[i].level, t->layers[i].w, t->layers[i].h,
                (unsigned long)t->layers[i].bytes, t->checksum[i]);
    if (ferror(f)) {
        fclose(f);
        err_set(err, MIP_ERR_IO, "path", -1, -1);
        return MIP_ERR_IO;
    }
    if (fclose(f) != 0) {
        err_set(err, MIP_ERR_IO, "path", -1, -1);
        return MIP_ERR_IO;
    }
    return MIP_OK;
}

/* ------------------------------------------------------------------
 * 选层缓存
 * ------------------------------------------------------------------ */

int mip_cache_init(MipLayerCache *cache, const MipChain *chain, MipError *err) {
    err_clear(err);
    if (!cache || !chain) {
        err_set(err, MIP_ERR_ARG, cache ? "chain" : "cache", -1, -1);
        return MIP_ERR_ARG;
    }
    if (chain->levels <= 0 || !chain->pix || !chain->w || !chain->h) {
        err_set(err, MIP_ERR_CACHE, "chain", -1, -1);
        return MIP_ERR_CACHE;
    }
    for (int l = 0; l < chain->levels; ++l) {
        if (!chain->pix[l] || chain->w[l] <= 0 || chain->h[l] <= 0) {
            err_set(err, MIP_ERR_CACHE, "chain", -1, l);
            return MIP_ERR_CACHE;
        }
    }
    MipLayerCache built;
    built.chain = chain;
    built.slot_count = chain->levels;
    built.slots = (MipLayer*)calloc((size_t)built.slot_count, sizeof(MipLayer));
    built.bound = (unsigned char*)calloc((size_t)built.slot_count, 1);
    built.frame_cap = built.slot_count;
    built.frame = (MipLayer*)calloc((size_t)built.frame_cap, sizeof(MipLayer));
    built.cache_enabled = 1;
    built.batches = 0;
    built.selects = 0;
    built.hits = 0;
    built.misses = 0;
    built.samples = 0;
    if (!built.slots || !built.bound || !built.frame) {
        free(built.slots);
        free(built.bound);
        free(built.frame);
        err_set(err, MIP_ERR_MEM, "cache", -1, -1);
        return MIP_ERR_MEM;
    }
    *cache = built;
    return MIP_OK;
}

void mip_cache_free(MipLayerCache *cache) {
    if (!cache) return;
    free(cache->slots);
    free(cache->bound);
    free(cache->frame);
    cache->slots = NULL;
    cache->bound = NULL;
    cache->frame = NULL;
    cache->chain = NULL;
    cache->slot_count = 0;
    cache->frame_cap = 0;
}

void mip_cache_set_enabled(MipLayerCache *cache, int enabled) {
    if (cache) cache->cache_enabled = enabled ? 1 : 0;
}

int mip_cache_enabled(const MipLayerCache *cache) {
    return (cache && cache->cache_enabled) ? 1 : 0;
}

void mip_cache_reset_stats(MipLayerCache *cache) {
    if (!cache) return;
    cache->batches = 0;
    cache->selects = 0;
    cache->hits = 0;
    cache->misses = 0;
    cache->samples = 0;
}

void mip_cache_stats(const MipLayerCache *cache, MipStats *stats) {
    if (!stats) return;
    if (!cache) {
        stats->levels = 0;
        stats->batches = stats->selects = stats->hits = stats->misses = stats->samples = 0;
        return;
    }
    stats->levels = cache->slot_count;
    stats->batches = cache->batches;
    stats->selects = cache->selects;
    stats->hits = cache->hits;
    stats->misses = cache->misses;
    stats->samples = cache->samples;
}

/*
 * 选层核心。list_pos >= 0 表示错误索引按"数组下标"报告；list_pos < 0
 * 表示单层选择，错误索引按层号本身报告。
 */
static int select_one(MipLayerCache *cache, int level, MipLayer *out,
                      MipError *err, int list_pos) {
    int err_index = list_pos >= 0 ? list_pos : level;
    if (!cache || !out || !cache->chain || cache->slot_count <= 0 ||
        !cache->slots || !cache->bound) {
        err_set(err, MIP_ERR_CACHE, "cache", err_index, level);
        return MIP_ERR_CACHE;
    }
    const MipChain *c = cache->chain;
    if (level < 0 || level >= cache->slot_count) {
        err_set(err, MIP_ERR_LEVEL, "levels", err_index, level);
        return MIP_ERR_LEVEL;
    }
    if (!c->pix || !c->pix[level] || c->w[level] <= 0 || c->h[level] <= 0) {
        err_set(err, MIP_ERR_CACHE, "chain", err_index, level);
        return MIP_ERR_CACHE;
    }
    ++cache->selects;
    if (cache->cache_enabled && cache->bound[level]) {
        ++cache->hits;
        *out = cache->slots[level];
        return MIP_OK;
    }
    ++cache->misses;
    MipLayer *slot = &cache->slots[level];
    slot->level = level;
    slot->w = c->w[level];
    slot->h = c->h[level];
    slot->bytes = (size_t)c->w[level] * (size_t)c->h[level] * 3;
    slot->pix = c->pix[level];
    if (cache->cache_enabled) cache->bound[level] = 1;
    *out = *slot;
    return MIP_OK;
}

int mip_layer_select(MipLayerCache *cache, int level, MipLayer *out, MipError *err) {
    err_clear(err);
    return select_one(cache, level, out, err, -1);
}

int mip_layer_select_list(MipLayerCache *cache, const int *levels, int count,
                          MipLayer *out, MipError *err) {
    err_clear(err);
    if (!levels || !out || count < 0) {
        err_set(err, MIP_ERR_ARG, !levels ? "levels" : (!out ? "out" : "count"), -1, -1);
        return MIP_ERR_ARG;
    }
    if (!cache || !cache->chain || cache->slot_count <= 0 || !cache->slots || !cache->bound) {
        err_set(err, MIP_ERR_CACHE, "cache", -1, -1);
        return MIP_ERR_CACHE;
    }
    /* 先整组校验，出错时不写 out、也不动统计。 */
    for (int i = 0; i < count; ++i) {
        if (levels[i] < 0 || levels[i] >= cache->slot_count) {
            err_set(err, MIP_ERR_LEVEL, "levels", i, levels[i]);
            return MIP_ERR_LEVEL;
        }
        const MipChain *c = cache->chain;
        if (!c->pix || !c->pix[levels[i]] ||
            c->w[levels[i]] <= 0 || c->h[levels[i]] <= 0) {
            err_set(err, MIP_ERR_CACHE, "chain", i, levels[i]);
            return MIP_ERR_CACHE;
        }
    }
    for (int i = 0; i < count; ++i) {
        int rc = select_one(cache, levels[i], &out[i], err, i);
        if (rc != MIP_OK) return rc;
    }
    return MIP_OK;
}

/* ------------------------------------------------------------------
 * 第二段：采样
 * ------------------------------------------------------------------ */

static int layer_valid(const MipLayer *layer) {
    return layer && layer->pix && layer->w > 0 && layer->h > 0;
}

/* 有限但极端的 u/v 先夹到 int 可表示范围再取整：
 * 对正常范围 [0,1] 结果与旧接口的截断公式逐位相同，
 * 同时避免 (int) 转换越界（C 里是未定义行为）。 */
static int uv_to_index(double t, int size) {
    double x = t * (double)size;
    if (!(x > 0.0)) return 0;               /* 负数与 NaN 都落到 0 */
    if (x >= (double)(size - 1)) return size - 1;
    return (int)x;
}

int mip_layer_sample(const MipLayer *layer, double u, double v,
                     unsigned char out[3], MipError *err) {
    err_clear(err);
    if (!layer || !out) {
        err_set(err, MIP_ERR_ARG, layer ? "out" : "layer", -1, -1);
        return MIP_ERR_ARG;
    }
    if (!layer_valid(layer)) {
        err_set(err, MIP_ERR_ARG, "layer", -1, layer->level);
        return MIP_ERR_ARG;
    }
    if (!isfinite(u)) {
        err_set(err, MIP_ERR_UV, "u", -1, layer->level);
        return MIP_ERR_UV;
    }
    if (!isfinite(v)) {
        err_set(err, MIP_ERR_UV, "v", -1, layer->level);
        return MIP_ERR_UV;
    }
    int x = uv_to_index(u, layer->w);
    int y = uv_to_index(v, layer->h);
    for (int ch = 0; ch < 3; ++ch) out[ch] = layer->pix[(y*layer->w+x)*3+ch];
    err_clear(err);
    return MIP_OK;
}

int mip_layer_sample_same(const MipLayer *layers, int count, double u, double v,
                          unsigned char *out, MipError *err) {
    err_clear(err);
    if (!layers || !out || count < 0) {
        err_set(err, MIP_ERR_ARG, !layers ? "layers" : (!out ? "out" : "count"), -1, -1);
        return MIP_ERR_ARG;
    }
    for (int i = 0; i < count; ++i) {
        if (!layer_valid(&layers[i])) {
            err_set(err, MIP_ERR_ARG, "layers", i, layers[i].level);
            return MIP_ERR_ARG;
        }
    }
    if (!isfinite(u)) {
        err_set(err, MIP_ERR_UV, "u", -1, -1);
        return MIP_ERR_UV;
    }
    if (!isfinite(v)) {
        err_set(err, MIP_ERR_UV, "v", -1, -1);
        return MIP_ERR_UV;
    }
    for (int i = 0; i < count; ++i) {
        const MipLayer *layer = &layers[i];
        int x = uv_to_index(u, layer->w);
        int y = uv_to_index(v, layer->h);
        const unsigned char *p = layer->pix + ((size_t)y * (size_t)layer->w + (size_t)x) * 3;
        out[i*3+0] = p[0];
        out[i*3+1] = p[1];
        out[i*3+2] = p[2];
    }
    return MIP_OK;
}

/* ------------------------------------------------------------------
 * 两段合并入口
 * ------------------------------------------------------------------ */

int mip_batch_sample(MipLayerCache *cache, const int *levels, int count,
                     double u, double v, unsigned char *out,
                     MipStats *stats, MipError *err) {
    err_clear(err);
    if (stats) {
        if (cache) mip_cache_stats(cache, stats);
        else {
            stats->levels = 0;
            stats->batches = stats->selects = stats->hits = stats->misses = stats->samples = 0;
        }
    }
    if (!levels || count < 0) {
        err_set(err, MIP_ERR_ARG, !levels ? "levels" : "count", -1, -1);
        return MIP_ERR_ARG;
    }
    if (count > 0 && !out) {
        err_set(err, MIP_ERR_ARG, "out", -1, -1);
        return MIP_ERR_ARG;
    }
    if (cache) {
        if (!cache->chain || cache->slot_count <= 0 || !cache->slots) {
            err_set(err, MIP_ERR_CACHE, "cache", -1, -1);
            return MIP_ERR_CACHE;
        }
        if (cache->frame_cap < count) {
            MipLayer *grown = (MipLayer*)realloc(cache->frame, (size_t)count * sizeof(MipLayer));
            if (!grown) {
                err_set(err, MIP_ERR_MEM, "cache", -1, -1);
                return MIP_ERR_MEM;
            }
            cache->frame = grown;
            cache->frame_cap = count;
        }
        for (int i = 0; i < count; ++i) {
            int rc = select_one(cache, levels[i], &cache->frame[i], err, i);
            if (rc != MIP_OK) {
                if (stats) mip_cache_stats(cache, stats);
                return rc;
            }
        }
        int rc = mip_layer_sample_same(cache->frame, count, u, v, out, err);
        if (rc != MIP_OK) {
            if (stats) mip_cache_stats(cache, stats);
            return rc;
        }
        ++cache->batches;
        cache->samples += count;
        if (stats) mip_cache_stats(cache, stats);
        return MIP_OK;
    }

    /* 无缓存时仍可按链选层：批量入口只接受带链的缓存。 */
    err_set(err, MIP_ERR_CACHE, "cache", -1, -1);
    if (stats) mip_cache_stats(NULL, stats);
    return MIP_ERR_CACHE;
}
