#include "cull.h"
#include <stdlib.h>

/* ================================================================== */
/* 旧接口：保持原语义不变                                              */
/* ================================================================== */

static int overlaps(CullBox b, CullView v) {
    return !(b.x + b.w < v.x || b.x > v.x + v.w || b.y + b.h < v.y || b.y > v.y + v.h);
}

int cull_run(const CullBox *boxes, int n, CullView view, CullResult *out) {
    if (!boxes || !out || n < 0) return -1;
    if (view.w <= 0 || view.h <= 0) return -2;
    out->ids = (int*)malloc(sizeof(int) * (size_t)(n > 0 ? n : 1));
    if (!out->ids) return -1;
    out->count = 0;
    for (int i = 0; i < n; ++i)
        if (overlaps(boxes[i], view)) out->ids[out->count++] = i;
    return 0;
}

int cull_count_within(const CullBox *boxes, int n, CullView view) {
    if (!boxes || n < 0) return -1;
    int c = 0;
    for (int i = 0; i < n; ++i) if (overlaps(boxes[i], view)) ++c;
    return c;
}

/* ================================================================== */
/* 分层两段剔除                                                        */
/* ================================================================== */

static int cmp_int_asc(const void *a, const void *b) {
    int x = *(const int*)a, y = *(const int*)b;
    return (x > y) - (x < y);
}

static void err_clear(CullError *e) {
    if (!e) return;
    e->code = CULL_OK;
    e->index = -1;
    e->layer = -1;
    e->found = 0;
    e->limit = CULL_MAX_LAYERS;
    e->field = NULL;
}

static int err_set(CullError *e, int code) {
    if (e) e->code = code;
    return code;
}

void cull_grouping_free(CullGrouping *g) {
    if (!g) return;
    free(g->members);
    free(g->groups);
    g->members = NULL;
    g->groups = NULL;
    g->n = 0;
    g->group_count = 0;
}

/* 在升序 keys[0..kn) 中查找 layer，返回组号；找不到返回 -1。 */
static int find_key(const int *keys, int kn, int layer) {
    int lo = 0, hi = kn - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (keys[mid] == layer) return mid;
        if (keys[mid] < layer) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

/* 第一段：只按层号分组。不读取视口，不做剔除。 */
int cull_group_by_layer(const CullBox *boxes, int n, CullGrouping *g,
                        CullError *err) {
    err_clear(err);
    if (!boxes || !g || n < 0) return err_set(err, CULL_ERR_ARG);

    g->n = 0;
    g->members = NULL;
    g->groups = NULL;
    g->group_count = 0;

    /* 校验层号：负数直接拒绝。 */
    for (int i = 0; i < n; ++i) {
        if (boxes[i].layer < 0) {
            if (err) { err->index = i; err->layer = boxes[i].layer; err->field = "layer"; }
            return err_set(err, CULL_ERR_LAYER);
        }
    }

    /* 取出全部层号并升序去重，得到确定性的分组顺序。 */
    int *keys = NULL, kn = 0;
    if (n > 0) {
        keys = (int*)malloc(sizeof(int) * (size_t)n);
        if (!keys) return err_set(err, CULL_ERR_ARG);
        for (int i = 0; i < n; ++i) keys[i] = boxes[i].layer;
        qsort(keys, (size_t)n, sizeof(int), cmp_int_asc);
        for (int i = 0; i < n; ++i)
            if (kn == 0 || keys[kn - 1] != keys[i]) keys[kn++] = keys[i];
    }

    /* 超限检查：分组数（不同层号数）不得超过上限；恰好等于上限合法。 */
    if (kn > CULL_MAX_LAYERS) {
        int cut = keys[CULL_MAX_LAYERS]; /* 升序第 limit+1 个，即越界层号 */
        int first_idx = -1;
        for (int i = 0; i < n; ++i)
            if (boxes[i].layer == cut) { first_idx = i; break; }
        if (err) {
            err->index = first_idx;
            err->layer = cut;
            err->found = kn;
            err->field = "layer";
        }
        free(keys);
        return err_set(err, CULL_ERR_LAYER_LIMIT);
    }

    CullLayerGroup *groups = NULL;
    int *members = (int*)malloc(sizeof(int) * (size_t)(n > 0 ? n : 1));
    if (kn > 0) {
        groups = (CullLayerGroup*)malloc(sizeof(CullLayerGroup) * (size_t)kn);
    }
    if (!members || (kn > 0 && !groups)) {
        free(keys); free(members); free(groups);
        return err_set(err, CULL_ERR_ARG);
    }

    /* 统计每组候选数并标出边界；members 中组内顺序 = 输入下标升序。 */
    int *fill = kn > 0 ? (int*)calloc((size_t)kn, sizeof(int)) : NULL;
    if (kn > 0 && !fill) { free(keys); free(members); free(groups); return err_set(err, CULL_ERR_ARG); }

    for (int k = 0; k < kn; ++k) {
        groups[k].layer = keys[k];
        groups[k].count = 0;
        groups[k].first = 0;
    }
    /* 分组是为第二段提供候选；每组成员数 = 该层对象数。 */
    for (int i = 0; i < n; ++i) {
        int gk = find_key(keys, kn, boxes[i].layer);
        if (gk < 0) { free(fill); free(keys); free(members); free(groups); return err_set(err, CULL_ERR_ARG); }
        ++groups[gk].count;
    }
    for (int k = 0; k < kn; ++k)
        groups[k].first = (k == 0) ? 0 : groups[k - 1].first + groups[k - 1].count;
    for (int i = 0; i < n; ++i) {
        int gk = find_key(keys, kn, boxes[i].layer);
        members[groups[gk].first + fill[gk]++] = i;
    }
    free(fill);
    free(keys);

    g->n = n;
    g->members = members;
    g->groups = groups;
    g->group_count = kn;
    return CULL_OK;
}

/* 校验第一段的输出结构，防止越界读取。 */
static int grouping_valid(const CullGrouping *g) {
    if (g->n < 0 || g->group_count < 0) return 0;
    if (g->group_count == 0) return g->n == 0;
    if (!g->groups || !g->members) return 0;
    int expect = 0, prev_layer = -1;
    for (int k = 0; k < g->group_count; ++k) {
        if (g->groups[k].count < 0) return 0;
        if (g->groups[k].first != expect) return 0;       /* 边界首尾相接 */
        if (k == 0) { if (g->groups[k].layer < 0) return 0; }
        else if (g->groups[k].layer <= prev_layer) return 0; /* 层号严格升序 */
        prev_layer = g->groups[k].layer;
        expect += g->groups[k].count;
    }
    if (expect != g->n) return 0;
    for (int i = 0; i < g->n; ++i)
        if (g->members[i] < 0 || g->members[i] >= g->n) return 0;
    return 1;
}

/* 第二段：对每个组单独、独立地剔除。组之间只共享只读的输入与视口。 */
int cull_run_grouped(const CullBox *boxes, const CullGrouping *g, CullView view,
                     CullLayered *out, CullError *err) {
    err_clear(err);
    if (!boxes || !g || !out) return err_set(err, CULL_ERR_ARG);
    out->ids = NULL;
    out->count = 0;
    out->groups = NULL;
    out->group_count = 0;
    out->layer_limit = CULL_MAX_LAYERS;

    if (!grouping_valid(g)) return err_set(err, CULL_ERR_ARG);
    if (view.w <= 0 || view.h <= 0) return err_set(err, CULL_ERR_VIEW);

    /* 防御性检查：调用者传入的分组也不能超过上限。 */
    if (g->group_count > CULL_MAX_LAYERS) {
        if (err) {
            err->found = g->group_count;
            err->layer = g->groups[CULL_MAX_LAYERS].layer;
            err->index = -1;
            err->field = "layer";
        }
        return err_set(err, CULL_ERR_LAYER_LIMIT);
    }

    int *ids = (int*)malloc(sizeof(int) * (size_t)(g->n > 0 ? g->n : 1));
    CullGroup *og = NULL;
    if (g->group_count > 0)
        og = (CullGroup*)malloc(sizeof(CullGroup) * (size_t)g->group_count);
    if (!ids || (g->group_count > 0 && !og)) {
        free(ids); free(og);
        return err_set(err, CULL_ERR_ARG);
    }

    int total = 0;
    for (int k = 0; k < g->group_count; ++k) {
        const CullLayerGroup *grp = &g->groups[k];
        int first = total;
        /* 只在本组成员上遍历，不触碰其他层：组间串扰为 0。 */
        for (int m = 0; m < grp->count; ++m) {
            int id = g->members[grp->first + m];
            if (overlaps(boxes[id], view)) ids[total++] = id;
        }
        og[k].layer = grp->layer;
        og[k].first = first;
        og[k].count = total - first;
        og[k].candidates = grp->count;
    }

    out->ids = ids;
    out->count = total;
    out->groups = og;
    out->group_count = g->group_count;
    return CULL_OK;
}

int cull_run_layered(const CullBox *boxes, int n, CullView view, int layered,
                     CullLayered *out, CullError *err) {
    err_clear(err);
    if (!out) return err_set(err, CULL_ERR_ARG);
    out->ids = NULL;
    out->count = 0;
    out->groups = NULL;
    out->group_count = 0;
    out->layer_limit = CULL_MAX_LAYERS;
    if (!boxes || n < 0) return err_set(err, CULL_ERR_ARG);

    if (!layered) {
        /* 关闭新分支：与 cull_run 走完全相同的路径，输出照旧。 */
        if (view.w <= 0 || view.h <= 0) return err_set(err, CULL_ERR_VIEW);
        int *ids = (int*)malloc(sizeof(int) * (size_t)(n > 0 ? n : 1));
        if (!ids) return err_set(err, CULL_ERR_ARG);
        int c = 0;
        for (int i = 0; i < n; ++i)
            if (overlaps(boxes[i], view)) ids[c++] = i;
        out->ids = ids;
        out->count = c;
        return CULL_OK;
    }

    CullGrouping g;
    int rc = cull_group_by_layer(boxes, n, &g, err);
    if (rc != CULL_OK) return rc;
    rc = cull_run_grouped(boxes, &g, view, out, err);
    cull_grouping_free(&g);
    return rc;
}

void cull_layered_free(CullLayered *out) {
    if (!out) return;
    free(out->ids);
    free(out->groups);
    out->ids = NULL;
    out->groups = NULL;
    out->count = 0;
    out->group_count = 0;
}
