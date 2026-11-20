#include "cull.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static int overlaps(CullBox b, CullView v) {
    return !(b.x + b.w < v.x || b.x > v.x + v.w ||
             b.y + b.h < v.y || b.y > v.y + v.h);
}

static int view_valid(CullView v) {
    return isfinite(v.x) && isfinite(v.y) && isfinite(v.w) && isfinite(v.h) &&
           v.w > 0.0 && v.h > 0.0;
}

static void set_err(CullError *err, int code, int index, const char *field,
                    const char *fmt, ...) {
    if (!err) return;
    err->code = code;
    err->index = index;
    err->field = field;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err->message, sizeof(err->message), fmt, ap);
    va_end(ap);
}

/* ---------- 暴力遍历（原接口，作为对照基准） ---------- */

int cull_run(const CullBox *boxes, int n, CullView view, CullResult *out) {
    if (!boxes || !out || n < 0) return CULL_ERR_ARG;
    if (!view_valid(view)) return CULL_ERR_VIEW;
    out->ids = (int *)malloc(sizeof(int) * (size_t)(n > 0 ? n : 1));
    if (!out->ids) return CULL_ERR_MEMORY;
    out->count = 0;
    for (int i = 0; i < n; ++i)
        if (overlaps(boxes[i], view)) out->ids[out->count++] = i;
    return CULL_OK;
}

int cull_count_within(const CullBox *boxes, int n, CullView view) {
    if (!boxes || n < 0 || !view_valid(view)) return CULL_ERR_ARG;
    int c = 0;
    for (int i = 0; i < n; ++i)
        if (overlaps(boxes[i], view)) ++c;
    return c;
}

/* ---------- 第一层：层次包围盒构建 ---------- */

typedef struct { double minx, miny, maxx, maxy; } Rect;

static int node_grow(CullBVH *t) {
    int cap = t->node_count ? t->node_count : 0;
    if (cap == 0) {
        CullNode *p = (CullNode *)malloc(sizeof(CullNode) * 8);
        if (!p) return -1;
        t->nodes = p;
    } else {
        int ncap = cap * 2;
        CullNode *p = (CullNode *)realloc(t->nodes, sizeof(CullNode) * (size_t)ncap);
        if (!p) return -1;
        t->nodes = p;
    }
    return 0;
}

typedef struct {
    const CullBox *boxes;
    int *idx;
    CullBVH *tree;
    int error;
} BuildCtx;

typedef struct { double key; int id; } SortItem;

/* 严格弱序：先按排序键、再按对象 id，保证任意 qsort 实现下结果一致 */
static int cmp_sort_item(const void *pa, const void *pb) {
    const SortItem *a = (const SortItem *)pa, *b = (const SortItem *)pb;
    if (a->key < b->key) return -1;
    if (a->key > b->key) return 1;
    return (a->id > b->id) - (a->id < b->id);
}

static int build_rec(BuildCtx *c, int lo, int hi, int depth) {
    int node_id = c->tree->node_count;
    if (node_grow(c->tree) != 0) { c->error = CULL_ERR_MEMORY; return -1; }
    ++c->tree->node_count;
    CullNode *node = &c->tree->nodes[node_id];

    Rect r;
    const CullBox *b0 = &c->boxes[c->idx[lo]];
    r.minx = b0->x; r.miny = b0->y;
    r.maxx = b0->x + b0->w; r.maxy = b0->y + b0->h;
    for (int k = lo + 1; k < hi; ++k) {
        const CullBox *b = &c->boxes[c->idx[k]];
        if (b->x < r.minx) r.minx = b->x;
        if (b->y < r.miny) r.miny = b->y;
        if (b->x + b->w > r.maxx) r.maxx = b->x + b->w;
        if (b->y + b->h > r.maxy) r.maxy = b->y + b->h;
    }
    node->minx = r.minx; node->miny = r.miny;
    node->maxx = r.maxx; node->maxy = r.maxy;
    node->left = node->right = -1;
    node->first = lo; node->count = hi - lo;

    int cnt = hi - lo;
    if (cnt == 1) { /* 叶子 */
        if (depth + 1 > c->tree->levels) c->tree->levels = depth + 1;
        ++c->tree->leaf_count;
        return node_id;
    }

    /* 选跨度最大的轴，按该轴中心做中位切分；键相同以 id 决胜（确定性） */
    double span_x = r.maxx - r.minx, span_y = r.maxy - r.miny;
    int axis_x = span_x >= span_y;
    SortItem *items = (SortItem *)malloc(sizeof(SortItem) * (size_t)cnt);
    if (!items) { c->error = CULL_ERR_MEMORY; return -1; }
    for (int k = 0; k < cnt; ++k) {
        const CullBox *b = &c->boxes[c->idx[lo + k]];
        items[k].key = axis_x ? (b->x + b->w * 0.5) : (b->y + b->h * 0.5);
        items[k].id = c->idx[lo + k];
    }
    /* 严格弱序比较：先键后 id，结果与 qsort 实现无关、跨平台一致 */
    qsort(items, (size_t)cnt, sizeof(SortItem), cmp_sort_item);
    for (int k = 0; k < cnt; ++k) c->idx[lo + k] = items[k].id;
    free(items);

    int mid = lo + cnt / 2;
    int left = build_rec(c, lo, mid, depth + 1);
    if (left < 0) return -1;
    int right = build_rec(c, mid, hi, depth + 1);
    if (right < 0) return -1;
    /* 递归可能 realloc 移动 nodes，必须按 id 重新取址，不能用递归前的 node 指针 */
    c->tree->nodes[node_id].left = left;
    c->tree->nodes[node_id].right = right;
    return node_id;
}

int cull_bvh_build(const CullBox *boxes, int n, CullBVH *tree, CullError *err) {
    if (err) { err->code = CULL_OK; err->index = -1; err->field = NULL; }
    if (!boxes || !tree || n <= 0) {
        set_err(err, CULL_ERR_ARG, -1, NULL,
                "invalid argument: boxes=%p tree=%p n=%d (require n > 0)",
                (const void *)boxes, (void *)tree, n);
        return CULL_ERR_ARG;
    }
    /* 入参校验：包围盒退化（非有限值或 w/h 非正）必须报错并指出字段与下标 */
    for (int i = 0; i < n; ++i) {
        const CullBox *b = &boxes[i];
        if (!isfinite(b->x) || !isfinite(b->y) ||
            !isfinite(b->w) || !isfinite(b->h)) {
            const char *f = !isfinite(b->x) ? "x"
                          : !isfinite(b->y) ? "y"
                          : !isfinite(b->w) ? "w" : "h";
            set_err(err, CULL_ERR_DEGENERATE, i, f,
                    "degenerate box at index %d: field '%s' is not finite", i, f);
            return CULL_ERR_DEGENERATE;
        }
        if (b->w <= 0.0) {
            set_err(err, CULL_ERR_DEGENERATE, i, "w",
                    "degenerate box at index %d: field 'w' = %.17g (require w > 0)",
                    i, b->w);
            return CULL_ERR_DEGENERATE;
        }
        if (b->h <= 0.0) {
            set_err(err, CULL_ERR_DEGENERATE, i, "h",
                    "degenerate box at index %d: field 'h' = %.17g (require h > 0)",
                    i, b->h);
            return CULL_ERR_DEGENERATE;
        }
    }

    tree->nodes = NULL;
    tree->order = (int *)malloc(sizeof(int) * (size_t)n);
    if (!tree->order) {
        set_err(err, CULL_ERR_MEMORY, -1, NULL, "out of memory allocating order");
        return CULL_ERR_MEMORY;
    }
    for (int i = 0; i < n; ++i) tree->order[i] = i;

    tree->node_count = 0;
    tree->leaf_count = 0;
    tree->object_count = n;
    tree->levels = 0;

    BuildCtx ctx = { boxes, tree->order, tree, 0 };
    int root = build_rec(&ctx, 0, n, 0);
    if (root < 0) {
        free(tree->nodes);
        free(tree->order);
        tree->nodes = NULL;
        tree->order = NULL;
        if (err) {
            err->code = CULL_ERR_MEMORY;
            err->index = -1;
            err->field = NULL;
            snprintf(err->message, sizeof(err->message), "out of memory building BVH");
        }
        return CULL_ERR_MEMORY;
    }

    /* 层数为零属于结构错误，明确报出字段名 levels */
    if (tree->levels <= 0) {
        free(tree->nodes);
        free(tree->order);
        tree->nodes = NULL;
        tree->order = NULL;
        set_err(err, CULL_ERR_ZERO_LEVELS, -1, "levels",
                "zero tree levels: field 'levels' = %d (require levels >= 1)",
                tree->levels);
        return CULL_ERR_ZERO_LEVELS;
    }
    return CULL_OK;
}

void cull_bvh_free(CullBVH *tree) {
    if (!tree) return;
    free(tree->nodes);
    free(tree->order);
    tree->nodes = NULL;
    tree->order = NULL;
    tree->node_count = tree->leaf_count = tree->object_count = tree->levels = 0;
}

/* ---------- 第二层：视口递归剪枝 ---------- */

static int node_hit(const CullNode *n, CullView v) {
    return !(n->maxx < v.x || n->minx > v.x + v.w ||
             n->maxy < v.y || n->miny > v.y + v.h);
}

/* 递归只做两件事：访问计数 + 对命中对象打标记；标记数组随递归栈分配 */
static void query_rec(const CullBVH *tree, const CullBox *boxes, int node_id,
                      CullView view, unsigned char *mark, CullQueryStats *st) {
    ++st->visited_nodes;
    const CullNode *node = &tree->nodes[node_id];
    if (!node_hit(node, view)) return; /* 整棵子树剪枝 */

    if (node->left < 0) {
        for (int k = node->first; k < node->first + node->count; ++k) {
            int id = tree->order[k];
            ++st->candidates; /* 候选：包围盒相交后进入逐对象实测的对象 */
            if (overlaps(boxes[id], view)) mark[id] = 1;
        }
        return;
    }
    query_rec(tree, boxes, node->left, view, mark, st);
    query_rec(tree, boxes, node->right, view, mark, st);
}

int cull_bvh_query(const CullBVH *tree, const CullBox *boxes, CullView view,
                   CullResult *out, CullQueryStats *stats, CullError *err) {
    if (err) { err->code = CULL_OK; err->index = -1; err->field = NULL; }
    if (!tree || !boxes || !out) {
        set_err(err, CULL_ERR_ARG, -1, NULL,
                "invalid argument: tree=%p boxes=%p out=%p",
                (const void *)tree, (const void *)boxes, (void *)out);
        return CULL_ERR_ARG;
    }
    if (!tree->nodes || tree->node_count <= 0 || tree->levels <= 0) {
        /* 层数为零属于结构错误，明确报出字段名 levels */
        set_err(err, CULL_ERR_ZERO_LEVELS, -1, "levels",
                "zero tree levels: field 'levels' = %d (require levels >= 1)",
                tree->levels);
        return CULL_ERR_ZERO_LEVELS;
    }
    if (!view_valid(view)) {
        set_err(err, CULL_ERR_VIEW, -1, NULL,
                "invalid view: w=%.17g h=%.17g (require w > 0 and h > 0)",
                view.w, view.h);
        return CULL_ERR_VIEW;
    }

    CullQueryStats local = {0, 0, 0};
    if (!stats) stats = &local;
    stats->visited_nodes = 0;
    stats->candidates = 0;
    stats->visible = 0;

    int n = tree->object_count;
    unsigned char *mark = (unsigned char *)calloc((size_t)n, 1);
    int *ids = (int *)malloc(sizeof(int) * (size_t)(n > 0 ? n : 1));
    if (!mark || !ids) {
        free(mark);
        free(ids);
        set_err(err, CULL_ERR_MEMORY, -1, NULL, "out of memory in bvh query");
        return CULL_ERR_MEMORY;
    }
    query_rec(tree, boxes, 0, view, mark, stats);

    /* 线性扫描标记数组即得升序 id：O(n)、无比较排序，顺序确定可复现 */
    int count = 0;
    for (int id = 0; id < n; ++id)
        if (mark[id]) ids[count++] = id;
    free(mark);
    out->ids = ids;
    out->count = count;
    stats->visible = count;
    return CULL_OK;
}

/* 对称差：两个升序 id 集合中不一致的元素个数 */
int cull_result_diff(const CullResult *a, const CullResult *b) {
    if (!a || !b) return -1;
    int i = 0, j = 0, diff = 0;
    while (i < a->count && j < b->count) {
        if (a->ids[i] < b->ids[j]) { ++diff; ++i; }
        else if (a->ids[i] > b->ids[j]) { ++diff; ++j; }
        else { ++i; ++j; }
    }
    diff += (a->count - i) + (b->count - j);
    return diff;
}
