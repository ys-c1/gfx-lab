#include "grid.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* 错误码：
 *  -1 通用参数错误（空指针/负值数量）
 *  -2 cfg.cell_w 非法（格宽为零或负）
 *  -3 cfg.density 非法（密度为负）
 *  -4 世界范围非法（max <= min）
 *  -5 增删的 id 越界或已不存在
 *  -6 内存分配失败
 */
const char *grid_error_field(int err) {
    switch (err) {
    case -2: return "cell_w";
    case -3: return "density";
    case -4: return "minx/miny/maxx/maxy";
    case -5: return "id";
    default: return NULL;
    }
}

typedef struct { int *ids; int count, cap; } Cell;

struct GdGrid {
    GdConfig cfg;
    Cell *cells;
    int dimx, dimy;
    GdBox *boxes;   /* id -> 包围盒，alive=0 的 id 保留槽位 */
    char *alive;
    int n, cap;
    unsigned tick;          /* 每次查询递增，用于格访问去重 */
    unsigned *stamp;        /* 对象戳：stamp[id]=上次测试它的对象 mark（id+1） */
    unsigned *cell_stamp;   /* 格戳：cell_stamp[cell]=上次访问该格的 tick */
    GdStats stats;
};

static int cell_of(const GdGrid *g, double p, int dim, double min) {
    double c = floor((p - min) / g->cfg.cell_w);
    if (c < 0) c = 0;
    if (c >= dim) c = dim - 1;
    return (int)c;
}

static void cell_range(const GdGrid *g, const GdBox *b, int *x0, int *y0, int *x1, int *y1) {
    *x0 = cell_of(g, b->x, g->dimx, g->cfg.minx);
    *y0 = cell_of(g, b->y, g->dimy, g->cfg.miny);
    *x1 = cell_of(g, b->x + b->w, g->dimx, g->cfg.minx);
    *y1 = cell_of(g, b->y + b->h, g->dimy, g->cfg.miny);
}

static int cell_push(Cell *c, int id) {
    if (c->count == c->cap) {
        int ncap = c->cap ? c->cap * 2 : 4;
        int *p = (int *)realloc(c->ids, (size_t)ncap * sizeof(int));
        if (!p) return -6;
        c->ids = p;
        c->cap = ncap;
    }
    c->ids[c->count++] = id;
    return 0;
}

static void cell_remove(Cell *c, int id) {
    for (int i = 0; i < c->count; ++i) {
        if (c->ids[i] == id) {
            c->ids[i] = c->ids[--c->count];
            return;
        }
    }
}

static int overlap(const GdBox *a, const GdBox *b) {
    return !(a->x + a->w < b->x || b->x + b->w < a->x ||
             a->y + a->h < b->y || b->y + b->h < a->y);
}

int grid_pick_cell(GdConfig *cfg, const GdBox *boxes, int n) {
    if (!cfg || n < 0 || (n > 0 && !boxes)) return -1;
    if (cfg->density < 0.0) return -3;
    double minx, miny, maxx, maxy;
    if (n == 0) {
        minx = cfg->minx; miny = cfg->miny; maxx = cfg->maxx; maxy = cfg->maxy;
    } else {
        minx = maxx = boxes[0].x;
        miny = maxy = boxes[0].y;
        for (int i = 0; i < n; ++i) {
            double x1 = boxes[i].x, y1 = boxes[i].y;
            double x2 = x1 + boxes[i].w, y2 = y1 + boxes[i].h;
            if (x1 < minx) minx = x1;
            if (y1 < miny) miny = y1;
            if (x2 > maxx) maxx = x2;
            if (y2 > maxy) maxy = y2;
        }
    }
    cfg->minx = minx; cfg->miny = miny; cfg->maxx = maxx; cfg->maxy = maxy;
    if (!(maxx > minx) || !(maxy > miny)) return -4;

    double density = cfg->density > 0.0 ? cfg->density : 4.0;
    double area = (maxx - minx) * (maxy - miny);
    /* 目标格数 = n / density，格宽 = sqrt(面积 / 目标格数) */
    double w = sqrt(area * density / (n > 0 ? (double)n : 1.0));
    if (!(w > 0.0) || !isfinite(w)) return -4;
    cfg->cell_w = w;
    if (!(cfg->density > 0.0)) cfg->density = density;
    return 0;
}

int grid_build(GdGrid **out, const GdBox *boxes, int n, GdConfig cfg) {
    if (!out || n < 0 || (n > 0 && !boxes)) return -1;
    if (cfg.cell_w <= 0.0) return -2;
    if (cfg.density < 0.0) return -3;
    if (!(cfg.maxx > cfg.minx) || !(cfg.maxy > cfg.miny)) return -4;

    GdGrid *g = (GdGrid *)calloc(1, sizeof(GdGrid));
    if (!g) return -6;
    g->cfg = cfg;
    g->dimx = (int)ceil((cfg.maxx - cfg.minx) / cfg.cell_w);
    g->dimy = (int)ceil((cfg.maxy - cfg.miny) / cfg.cell_w);
    if (g->dimx < 1) g->dimx = 1;
    if (g->dimy < 1) g->dimy = 1;
    size_t ncells = (size_t)g->dimx * (size_t)g->dimy;
    g->cells = (Cell *)calloc(ncells, sizeof(Cell));
    g->cap = n > 0 ? n : 16;
    g->boxes = (GdBox *)malloc((size_t)g->cap * sizeof(GdBox));
    g->alive = (char *)calloc((size_t)g->cap, 1);
    g->stamp = (unsigned *)calloc((size_t)g->cap, sizeof(unsigned));
    g->cell_stamp = (unsigned *)calloc(ncells, sizeof(unsigned));
    if (!g->cells || !g->boxes || !g->alive || !g->stamp || !g->cell_stamp) {
        grid_free(g);
        return -6;
    }
    g->stats.dimx = g->dimx;
    g->stats.dimy = g->dimy;
    g->stats.rebuilds = 1;

    for (int id = 0; id < n; ++id) {
        g->boxes[id] = boxes[id];
        g->alive[id] = 1;
        int x0, y0, x1, y1;
        cell_range(g, &boxes[id], &x0, &y0, &x1, &y1);
        for (int cy = y0; cy <= y1; ++cy)
            for (int cx = x0; cx <= x1; ++cx)
                if (cell_push(&g->cells[(size_t)cy * g->dimx + cx], id) != 0) {
                    grid_free(g);
                    return -6;
                }
    }
    g->n = n;
    *out = g;
    return 0;
}

void grid_pairs_free(GdPairList *l) {
    if (l) { free(l->pairs); l->pairs = NULL; l->count = 0; }
}

void grid_free(GdGrid *g) {
    if (!g) return;
    if (g->cells) {
        size_t ncells = (size_t)g->dimx * (size_t)g->dimy;
        for (size_t i = 0; i < ncells; ++i) free(g->cells[i].ids);
        free(g->cells);
    }
    free(g->boxes);
    free(g->alive);
    free(g->stamp);
    free(g->cell_stamp);
    free(g);
}

static int grow_id_storage(GdGrid *g, int need) {
    if (need < g->cap) return 0;
    int ncap = g->cap;
    while (ncap <= need) ncap *= 2;
    GdBox *nb = (GdBox *)realloc(g->boxes, (size_t)ncap * sizeof(GdBox));
    char *na = (char *)realloc(g->alive, (size_t)ncap);
    unsigned *ns = (unsigned *)realloc(g->stamp, (size_t)ncap * sizeof(unsigned));
    if (!nb || !na || !ns) { free(nb); free(na); free(ns); return -6; }
    memset(na + g->cap, 0, (size_t)(ncap - g->cap));
    memset(ns + g->cap, 0, (size_t)(ncap - g->cap) * sizeof(unsigned));
    g->boxes = nb; g->alive = na; g->stamp = ns; g->cap = ncap;
    return 0;
}

int grid_insert(GdGrid *g, int id, GdBox box) {
    if (!g || id < 0) return -1;
    if (grow_id_storage(g, id) != 0) return -6;
    if (g->alive[id]) return -5;
    g->boxes[id] = box;
    g->alive[id] = 1;
    if (id >= g->n) g->n = id + 1;
    int x0, y0, x1, y1;
    cell_range(g, &box, &x0, &y0, &x1, &y1);
    for (int cy = y0; cy <= y1; ++cy)
        for (int cx = x0; cx <= x1; ++cx)
            if (cell_push(&g->cells[(size_t)cy * g->dimx + cx], id) != 0) return -6;
    ++g->stats.updates;
    return 0;
}

int grid_remove(GdGrid *g, int id) {
    if (!g) return -1;
    if (id < 0 || id >= g->n || !g->alive[id]) return -5;
    GdBox *b = &g->boxes[id];
    int x0, y0, x1, y1;
    cell_range(g, b, &x0, &y0, &x1, &y1);
    for (int cy = y0; cy <= y1; ++cy)
        for (int cx = x0; cx <= x1; ++cx)
            cell_remove(&g->cells[(size_t)cy * g->dimx + cx], id);
    g->alive[id] = 0;
    ++g->stats.updates;
    return 0;
}

int grid_pairs(GdGrid *g, GdPairList *out, GdStats *stats) {
    if (!g || !out) return -1;
    out->pairs = NULL;
    out->count = 0;
    size_t pair_cap = 0;
    long long hits = 0, tests = 0;
    ++g->tick;
    if (g->tick == 0) { /* 翻转：清空格戳后从 1 重新开始 */
        memset(g->cell_stamp, 0, (size_t)g->dimx * (size_t)g->dimy * sizeof(unsigned));
        g->tick = 1;
    }
    /* 每对象戳记录“上次测试它的对象 id”，跨查询必须清空，
       否则残留值可能等于本次的 id+1 而漏测 */
    memset(g->stamp, 0, (size_t)g->cap * sizeof(unsigned));
    for (int id = 0; id < g->n; ++id) {
        if (!g->alive[id]) continue;
        unsigned mark = (unsigned)id + 1; /* stamp[other] 记录上次测试它的对象 */
        int x0, y0, x1, y1;
        cell_range(g, &g->boxes[id], &x0, &y0, &x1, &y1);
        for (int cy = y0; cy <= y1; ++cy) {
            for (int cx = x0; cx <= x1; ++cx) {
                size_t ci = (size_t)cy * g->dimx + cx;
                if (g->cell_stamp[ci] != g->tick) {
                    g->cell_stamp[ci] = g->tick;
                    ++hits;
                }
                Cell *c = &g->cells[ci];
                for (int k = 0; k < c->count; ++k) {
                    int other = c->ids[k];
                    if (other <= id || !g->alive[other]) continue;
                    if (g->stamp[other] == mark) continue;
                    g->stamp[other] = mark;
                    ++tests;
                    if (overlap(&g->boxes[id], &g->boxes[other])) {
                        if ((size_t)out->count == pair_cap) {
                            size_t ncap = pair_cap ? pair_cap * 2 : 256;
                            GdPair *p = (GdPair *)realloc(out->pairs, ncap * sizeof(GdPair));
                            if (!p) { grid_pairs_free(out); return -6; }
                            out->pairs = p;
                            pair_cap = ncap;
                        }
                        out->pairs[out->count++] = (GdPair){id, other};
                    }
                }
            }
        }
    }
    g->stats.cell_hits = hits;
    g->stats.candidates = tests;
    if (stats) *stats = g->stats;
    return 0;
}

int grid_stats(const GdGrid *g, GdStats *out) {
    if (!g || !out) return -1;
    *out = g->stats;
    return 0;
}

long long grid_table_bytes(const GdGrid *g) {
    if (!g) return 0;
    size_t ncells = (size_t)g->dimx * (size_t)g->dimy;
    long long bytes = (long long)(ncells * sizeof(Cell) +
                                  ncells * sizeof(unsigned) /* cell_stamp */);
    for (size_t i = 0; i < ncells; ++i)
        bytes += (long long)(size_t)g->cells[i].cap * (long long)sizeof(int);
    return bytes;
}

/* ---------------- 对照组 1：暴力遍历 ---------------- */

int grid_brute_pairs(const GdBox *boxes, int n, GdPairList *out, long long *candidates_out) {
    if (!out || n < 0 || (n > 0 && !boxes)) return -1;
    out->pairs = NULL;
    out->count = 0;
    size_t pair_cap = 0;
    long long tests = 0;
    for (int i = 0; i < n; ++i) {
        for (int j = i + 1; j < n; ++j) {
            ++tests;
            if (overlap(&boxes[i], &boxes[j])) {
                if ((size_t)out->count == pair_cap) {
                    size_t ncap = pair_cap ? pair_cap * 2 : 256;
                    GdPair *p = (GdPair *)realloc(out->pairs, ncap * sizeof(GdPair));
                    if (!p) { grid_pairs_free(out); return -6; }
                    out->pairs = p;
                    pair_cap = ncap;
                }
                out->pairs[out->count++] = (GdPair){i, j};
            }
        }
    }
    if (candidates_out) *candidates_out = tests;
    return 0;
}

/* ---------------- 对照组 2：层次包围盒 BVH ---------------- */

typedef struct {
    GdBox box;
    int left, right;   /* 叶子：left=idx 首位置, right=末位置(含)；内部节点：子节点号 */
    int leaf;
} BvhNode;

static GdBox union_box(const GdBox *a, const GdBox *b) {
    GdBox r;
    r.x = a->x < b->x ? a->x : b->x;
    r.y = a->y < b->y ? a->y : b->y;
    double ax = a->x + a->w, bx = b->x + b->w;
    double ay = a->y + a->h, by = b->y + b->h;
    double rx = ax > bx ? ax : bx;
    double ry = ay > by ? ay : by;
    r.w = rx - r.x;
    r.h = ry - r.y;
    return r;
}

/* qsort 无可携带上下文，用文件作用域指针（对照组单线程使用）。 */
static const GdBox *g_sort_boxes;
static int g_sort_axis;
static int bvh_cmp(const void *pa, const void *pb) {
    int ia = *(const int *)pa, ib = *(const int *)pb;
    double ca, cb;
    if (g_sort_axis == 0) {
        ca = g_sort_boxes[ia].x + g_sort_boxes[ia].w * 0.5;
        cb = g_sort_boxes[ib].x + g_sort_boxes[ib].w * 0.5;
    } else {
        ca = g_sort_boxes[ia].y + g_sort_boxes[ia].h * 0.5;
        cb = g_sort_boxes[ib].y + g_sort_boxes[ib].h * 0.5;
    }
    return (ca > cb) - (ca < cb);
}

static int bvh_build_rec(BvhNode *nodes, int *count, int *idx, int lo, int hi,
                         const GdBox *boxes) {
    int node = (*count)++;
    GdBox bound = boxes[idx[lo]];
    for (int i = lo + 1; i <= hi; ++i) bound = union_box(&bound, &boxes[idx[i]]);
    nodes[node].box = bound;
    int m = hi - lo + 1;
    if (m <= 8) {
        nodes[node].leaf = 1;
        nodes[node].left = lo;
        nodes[node].right = hi;
        return node;
    }
    double wx = bound.w, hy = bound.h;
    g_sort_boxes = boxes;
    g_sort_axis = wx >= hy ? 0 : 1;
    qsort(idx + lo, (size_t)m, sizeof(int), bvh_cmp);
    int mid = lo + m / 2;
    nodes[node].leaf = 0;
    nodes[node].left = bvh_build_rec(nodes, count, idx, lo, mid - 1, boxes);
    nodes[node].right = bvh_build_rec(nodes, count, idx, mid, hi, boxes);
    return node;
}

static int emit_pair(GdPairList *out, size_t *cap, const GdBox *boxes, int a, int b,
                     long long *tests) {
    ++(*tests); /* 一次对象级包围盒测试 */
    if (overlap(&boxes[a], &boxes[b])) {
        if (a > b) { int t = a; a = b; b = t; } /* 统一小编号在前 */
        if ((size_t)out->count == *cap) {
            size_t ncap = *cap ? *cap * 2 : 256;
            GdPair *p = (GdPair *)realloc(out->pairs, ncap * sizeof(GdPair));
            if (!p) return -6;
            out->pairs = p;
            *cap = ncap;
        }
        out->pairs[out->count++] = (GdPair){a, b};
    }
    return 0;
}

/* 逐对象树查询：对每个对象 i 遍历整棵树，跳过包围盒不相交的
 * 子树，只收集 id 更小的碰撞对象 j<i——每对恰好产出一次，天然去重。 */
static int bvh_walk(const BvhNode *nodes, int node_count, const GdBox *boxes,
                    const int *idx, int n, GdPairList *out, size_t *cap,
                    long long *tests) {
    int *stack = (int *)malloc((size_t)(node_count + 1) * sizeof(int));
    if (!stack) return -6;
    for (int id = 0; id < n; ++id) {
        const GdBox *q = &boxes[id];
        size_t sp = 0;
        stack[sp++] = 0;
        while (sp > 0) {
            int ni = stack[--sp];
            const BvhNode *nd = &nodes[ni];
            ++(*tests); /* 一次节点级包围盒测试（根节点与每个下钻节点） */
            if (!overlap(q, &nd->box)) continue;
            if (nd->leaf) {
                for (int k = nd->left; k <= nd->right; ++k) {
                    int j = idx[k];
                    if (j >= id) continue; /* 自配与重复对 */
                    int rc = emit_pair(out, cap, boxes, id, j, tests);
                    if (rc) { free(stack); return rc; }
                }
            } else {
                stack[sp++] = nd->left;
                stack[sp++] = nd->right;
            }
        }
    }
    free(stack);
    return 0;
}

int grid_bvh_pairs(const GdBox *boxes, int n, GdPairList *out, long long *tests_out) {
    if (!out || n < 0 || (n > 0 && !boxes)) return -1;
    out->pairs = NULL;
    out->count = 0;
    long long tests = 0;
    size_t cap = 0;
    if (n < 2) {
        if (tests_out) *tests_out = 0;
        return 0;
    }
    BvhNode *nodes = (BvhNode *)malloc((size_t)(2 * n) * sizeof(BvhNode));
    int *idx = (int *)malloc((size_t)n * sizeof(int));
    if (!nodes || !idx) { free(nodes); free(idx); return -6; }
    for (int i = 0; i < n; ++i) idx[i] = i;
    int node_count = 0;
    bvh_build_rec(nodes, &node_count, idx, 0, n - 1, boxes);
    int rc = bvh_walk(nodes, node_count, boxes, idx, n, out, &cap, &tests);
    free(nodes);
    free(idx);
    if (rc) { grid_pairs_free(out); return rc; }
    if (tests_out) *tests_out = tests;
    return 0;
}

long long grid_bvh_bytes(int n) {
    if (n <= 0) return 0;
    /* 2n 个节点 + n 个排序索引，对照结构本身的增量 */
    return (long long)(2 * (size_t)n * sizeof(BvhNode) + (size_t)n * sizeof(int));
}

/* ---------------- 结果集合比较 ---------------- */

static int pair_cmp(const void *pa, const void *pb) {
    const GdPair *a = (const GdPair *)pa, *b = (const GdPair *)pb;
    if (a->a != b->a) return a->a - b->a;
    return a->b - b->b;
}

int grid_pairs_equal(const GdPairList *a, const GdPairList *b) {
    if (!a || !b || a->count != b->count) return 0;
    if (a->count == 0) return 1;
    GdPair *ca = (GdPair *)malloc((size_t)a->count * sizeof(GdPair));
    GdPair *cb = (GdPair *)malloc((size_t)b->count * sizeof(GdPair));
    if (!ca || !cb) { free(ca); free(cb); return -6; }
    memcpy(ca, a->pairs, (size_t)a->count * sizeof(GdPair));
    memcpy(cb, b->pairs, (size_t)b->count * sizeof(GdPair));
    qsort(ca, (size_t)a->count, sizeof(GdPair), pair_cmp);
    qsort(cb, (size_t)b->count, sizeof(GdPair), pair_cmp);
    int eq = 1;
    for (int i = 0; eq && i < a->count; ++i)
        eq = ca[i].a == cb[i].a && ca[i].b == cb[i].b;
    free(ca);
    free(cb);
    return eq;
}
