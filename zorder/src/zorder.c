#include "zorder.h"
#include <stdlib.h>

static int is_pow2(int v) { return v > 0 && (v & (v - 1)) == 0; }

/* 把 16 bit 整数的各比特摊开到偶数位 */
static unsigned int spread_bits(unsigned int x) {
    x &= 0x0000ffffu;
    x = (x | (x << 8)) & 0x00ff00ffu;
    x = (x | (x << 4)) & 0x0f0f0f0fu;
    x = (x | (x << 2)) & 0x33333333u;
    x = (x | (x << 1)) & 0x55555555u;
    return x;
}

/* 把偶数位上的比特压紧回连续整数 */
static unsigned int compact_bits(unsigned int x) {
    x &= 0x55555555u;
    x = (x | (x >> 1)) & 0x33333333u;
    x = (x | (x >> 2)) & 0x0f0f0f0fu;
    x = (x | (x >> 4)) & 0x00ff00ffu;
    x = (x | (x >> 8)) & 0x0000ffffu;
    return x;
}

unsigned int zo_morton_encode(unsigned int x, unsigned int y) {
    return spread_bits(x) | (spread_bits(y) << 1);
}

void zo_morton_decode(unsigned int code, unsigned int *x, unsigned int *y) {
    if (x) *x = compact_bits(code);
    if (y) *y = compact_bits(code >> 1);
}

typedef struct { unsigned int key; int id; } Keyed;

static int cmp_key_asc(const void *a, const void *b) {
    const Keyed *ka = (const Keyed*)a, *kb = (const Keyed*)b;
    if (ka->key < kb->key) return -1;
    if (ka->key > kb->key) return 1;
    return (ka->id > kb->id) - (ka->id < kb->id);
}

static int cmp_area_desc(const void *a, const void *b, const long *areas) {
    long da = areas[((const Keyed*)a)->id], db = areas[((const Keyed*)b)->id];
    if (da > db) return -1;
    if (da < db) return 1;
    int ia = ((const Keyed*)a)->id, ib = ((const Keyed*)b)->id;
    return (ia > ib) - (ia < ib);
}

/* qsort 的比较函数不能带上下文，area 排序用一个文件级指针 */
static const long *g_area;
static int cmp_area_wrap(const void *a, const void *b) { return cmp_area_desc(a, b, g_area); }

static int count_jumps(const int *order, int n) {
    int jumps = 0;
    for (int i = 1; i < n; ++i) {
        int d = order[i] - order[i - 1];
        if (d < 0) d = -d;
        if (d != 1) ++jumps;
    }
    return jumps;
}

static int finish_order(ZoLayout *lay, Keyed *kv, int n) {
    lay->order = (int*)malloc(sizeof(int) * (size_t)n);
    if (!lay->order) { free(kv); return -5; }
    for (int i = 0; i < n; ++i) lay->order[i] = kv[i].id;
    lay->remap = NULL;
    lay->jumps = count_jumps(lay->order, n);
    free(kv);
    return 0;
}

int zo_reorder_codes(int w, int h, const unsigned int *codes, ZoLayout *lay) {
    if (!codes || !lay || w <= 0 || h <= 0) return -1;
    int n = w * h;
    lay->err_seq = 0;
    unsigned char *seen = (unsigned char*)calloc((size_t)n, 1);
    Keyed *kv = (Keyed*)malloc(sizeof(Keyed) * (size_t)n);
    if (!seen || !kv) { free(seen); free(kv); return -5; }
    for (int i = 0; i < n; ++i) {
        /* 码位非法：越界或重复，指出首个出错序号 */
        if ((int)codes[i] >= n) { lay->err_seq = i; free(seen); free(kv); return -4; }
        if (seen[codes[i]]) { lay->err_seq = i; free(seen); free(kv); return -3; }
        seen[codes[i]] = 1;
        kv[i].key = codes[i]; kv[i].id = i;
    }
    free(seen);
    qsort(kv, (size_t)n, sizeof(Keyed), cmp_key_asc);
    int rc = finish_order(lay, kv, n);
    if (rc) return rc;
    lay->w = w; lay->h = h; lay->n = n;
    return 0;
}

int zo_reorder(int w, int h, const long *areas, int strategy, ZoLayout *lay) {
    if (!lay || w <= 0 || h <= 0) return -1;
    if (!is_pow2(w) || !is_pow2(h)) { lay->err_seq = 0; return -2; }
    int n = w * h;
    lay->w = w; lay->h = h; lay->n = n; lay->err_seq = 0;

    if (strategy == ZO_NONE) {
        lay->order = (int*)malloc(sizeof(int) * (size_t)n);
        if (!lay->order) return -5;
        for (int i = 0; i < n; ++i) lay->order[i] = i;
        lay->remap = NULL; lay->jumps = 0;   /* 不打乱顺序，布局不变，无跳转 */
        return 0;
    }

    Keyed *kv = (Keyed*)malloc(sizeof(Keyed) * (size_t)n);
    if (!kv) return -5;
    for (int i = 0; i < n; ++i) { kv[i].id = i; kv[i].key = 0; }

    if (strategy == ZO_MORTON) {
        unsigned int *codes = (unsigned int*)malloc(sizeof(unsigned int) * (size_t)n);
        if (!codes) { free(kv); return -5; }
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                codes[y * w + x] = zo_morton_encode((unsigned)x, (unsigned)y);
        free(kv);
        int rc = zo_reorder_codes(w, h, codes, lay);
        free(codes);
        return rc;
    }

    if (strategy == ZO_AREA) {
        if (!areas) { free(kv); return -1; }
        g_area = areas;
        qsort(kv, (size_t)n, sizeof(Keyed), cmp_area_wrap);
        return finish_order(lay, kv, n);
    }

    free(kv);
    return -1;
}

int zo_backfill(ZoLayout *lay) {
    if (!lay || !lay->order || lay->n <= 0) return -1;
    if (!lay->remap) {
        lay->remap = (int*)malloc(sizeof(int) * (size_t)lay->n);
        if (!lay->remap) return -5;
    }
    for (int pos = 0; pos < lay->n; ++pos) lay->remap[lay->order[pos]] = pos;
    return 0;
}

int zo_layout(int w, int h, const long *areas, int strategy, ZoLayout *lay) {
    int rc = zo_reorder(w, h, areas, strategy, lay);
    if (rc != 0) return rc;
    return zo_backfill(lay);
}

void zo_free(ZoLayout *lay) {
    if (!lay) return;
    free(lay->order); free(lay->remap);
    lay->order = NULL; lay->remap = NULL;
    lay->w = lay->h = lay->n = lay->jumps = lay->err_seq = 0;
}

static int cmp_int_asc(const void *a, const void *b) {
    int ia = *(const int*)a, ib = *(const int*)b;
    return (ia > ib) - (ia < ib);
}

int zo_window_stats(const ZoLayout *lay, int bw, int bh, int stride, int cache_line,
                    double *jumps_win, double *span_ratio, double *occupancy) {
    if (!lay || !lay->order || !lay->remap || !jumps_win || !span_ratio || !occupancy) return -1;
    if (bw <= 0 || bh <= 0 || stride <= 0 || cache_line <= 0) return -1;
    if (bw > lay->w || bh > lay->h) return -1;

    int cnt = bw * bh;
    int total_lines = (lay->n + cache_line - 1) / cache_line;
    int *pos = (int*)malloc(sizeof(int) * (size_t)cnt);
    char *line_hit = (char*)calloc((size_t)total_lines, 1);
    if (!pos || !line_hit) { free(pos); free(line_hit); return -5; }

    double sum_jumps = 0.0, sum_span = 0.0, sum_occ = 0.0;
    int windows = 0;
    for (int wy = 0; wy + bh <= lay->h; wy += stride) {
        for (int wx = 0; wx + bw <= lay->w; wx += stride) {
            int k = 0, pmin = lay->n, pmax = -1;
            for (int yy = 0; yy < bh; ++yy)
                for (int xx = 0; xx < bw; ++xx) {
                    int id = (wy + yy) * lay->w + (wx + xx);
                    int p = lay->remap[id];
                    pos[k++] = p;
                    if (p < pmin) pmin = p;
                    if (p > pmax) pmax = p;
                }
            qsort(pos, (size_t)cnt, sizeof(int), cmp_int_asc);
            int jumps = 0, lines = 0;
            for (int i = 0; i < cnt; ++i) {
                if (i > 0 && pos[i] - pos[i - 1] != 1) ++jumps;
                int ln = pos[i] / cache_line;
                if (!line_hit[ln]) { line_hit[ln] = 1; ++lines; }
            }
            for (int i = 0; i < cnt; ++i) line_hit[pos[i] / cache_line] = 0;
            sum_jumps += (double)jumps;
            sum_span += (double)(pmax - pmin + 1) / (double)cnt;
            sum_occ += (double)lines / (double)total_lines;
            ++windows;
        }
    }
    free(pos);
    free(line_hit);
    if (windows == 0) return -1;
    *jumps_win = sum_jumps / (double)windows;
    *span_ratio = sum_span / (double)windows;
    *occupancy = sum_occ / (double)windows;
    return 0;
}
