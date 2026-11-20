#include "cull.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

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

/* ---- 权重表 ---- */

static int cmp_weight_id(const void *pa, const void *pb) {
    const CullWeight *a = (const CullWeight*)pa, *b = (const CullWeight*)pb;
    return (a->id > b->id) - (a->id < b->id);
}

int cull_weights_validate(const CullWeightTable *t, int *bad_id) {
    if (bad_id) *bad_id = -1;
    if (!t) return 0;
    if (t->count < 0 || (t->count > 0 && !t->entries)) return -1;
    for (int i = 0; i < t->count; ++i) {
        if (t->entries[i].weight < 0) {
            if (bad_id) *bad_id = t->entries[i].id;
            return -3;
        }
    }
    if (t->count > 1) {
        CullWeight *tmp = (CullWeight*)malloc(sizeof(CullWeight) * (size_t)t->count);
        if (!tmp) return -1;
        memcpy(tmp, t->entries, sizeof(CullWeight) * (size_t)t->count);
        qsort(tmp, (size_t)t->count, sizeof(CullWeight), cmp_weight_id);
        for (int i = 1; i < t->count; ++i) {
            if (tmp[i].id == tmp[i - 1].id && tmp[i].weight != tmp[i - 1].weight) {
                if (bad_id) *bad_id = tmp[i].id;
                free(tmp);
                return -4;
            }
        }
        free(tmp);
    }
    return 0;
}

double cull_weight_of(const CullWeightTable *t, int id) {
    if (!t) return 0.0;
    for (int i = 0; i < t->count; ++i)
        if (t->entries[i].id == id) return t->entries[i].weight;
    return 0.0;
}

/* 在按 id 升序的条目里二分查权重，未登记为 0 */
static double weight_lookup(const CullWeight *e, int n, int id) {
    int lo = 0, hi = n;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (e[mid].id < id) lo = mid + 1; else hi = mid;
    }
    if (lo < n && e[lo].id == id) return e[lo].weight;
    return 0.0;
}

/* 稳定归并：键相等取左段，保持原相对顺序 */
static void merge_sort_idx(int *idx, int *tmp, const int *layer, const double *w,
                           int lo, int hi, long *cmp) {
    if (hi - lo < 2) return;
    int mid = lo + (hi - lo) / 2;
    merge_sort_idx(idx, tmp, layer, w, lo, mid, cmp);
    merge_sort_idx(idx, tmp, layer, w, mid, hi, cmp);
    int i = lo, j = mid, k = lo;
    while (i < mid && j < hi) {
        ++*cmp;
        int li = layer[idx[i]], lj = layer[idx[j]];
        double wi = w[idx[i]], wj = w[idx[j]];
        if (li < lj || (li == lj && wi <= wj)) tmp[k++] = idx[i++];
        else tmp[k++] = idx[j++];
    }
    while (i < mid) tmp[k++] = idx[i++];
    while (j < hi) tmp[k++] = idx[j++];
    for (i = lo; i < hi; ++i) idx[i] = tmp[i];
}

int cull_order(const CullBox *boxes, const int *ids, int n,
               const CullWeightTable *t, int sort, int *out,
               long *cmp_count, int *bad_id) {
    if (!ids || !out || n < 0) return -1;
    if (n > 0 && !boxes) return -1;
    if (cmp_count) *cmp_count = 0;
    if (bad_id) *bad_id = -1;
    int rc = cull_weights_validate(t, bad_id);
    if (rc != 0) return rc;
    for (int i = 0; i < n; ++i) out[i] = ids[i];
    if (!sort || n < 2) return 0;

    int wn = t ? t->count : 0;
    CullWeight *ws = NULL;
    int *layer = NULL, *tmp = NULL;
    double *w = NULL;
    if (wn > 0) {
        ws = (CullWeight*)malloc(sizeof(CullWeight) * (size_t)wn);
        if (!ws) return -1;
        memcpy(ws, t->entries, sizeof(CullWeight) * (size_t)wn);
        qsort(ws, (size_t)wn, sizeof(CullWeight), cmp_weight_id);
    }
    layer = (int*)malloc(sizeof(int) * (size_t)n);
    w = (double*)malloc(sizeof(double) * (size_t)n);
    tmp = (int*)malloc(sizeof(int) * (size_t)n);
    if (!layer || !w || !tmp) {
        free(ws); free(layer); free(w); free(tmp);
        return -1;
    }
    for (int i = 0; i < n; ++i) {
        layer[i] = boxes[ids[i]].layer;
        w[i] = weight_lookup(ws, wn, ids[i]);
    }
    /* 第一段：稳定排序（排位置下标，键按下标索引） */
    long cmp = 0;
    {
        int *idx = (int*)malloc(sizeof(int) * (size_t)n);
        if (!idx) { free(ws); free(layer); free(w); free(tmp); return -1; }
        for (int i = 0; i < n; ++i) idx[i] = i;
        merge_sort_idx(idx, tmp, layer, w, 0, n, &cmp);
        for (int i = 0; i < n; ++i) out[i] = ids[idx[i]];
        free(idx);
    }
    /* 第二段：out 已按序写出 */
    if (cmp_count) *cmp_count = cmp;
    free(ws); free(layer); free(w); free(tmp);
    return 0;
}

int cull_weights_export(const CullWeightTable *t, const char *path) {
    if (!t || !path || t->count < 0 || (t->count > 0 && !t->entries)) return -1;
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    for (int i = 0; i < t->count; ++i)
        fprintf(f, "%d %.17g\n", t->entries[i].id, t->entries[i].weight);
    if (fclose(f) != 0) return -1;
    return 0;
}
