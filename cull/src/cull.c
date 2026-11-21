#include "cull.h"
#include <stdlib.h>

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

/* ------------------------------------------------------------------ */
/* 对象号 -> 对象表下标 映射                                            */
/* ------------------------------------------------------------------ */

typedef struct { int id; int at; } MapPair;

static int cmp_pair(const void *a, const void *b) {
    int ia = ((const MapPair*)a)->id, ib = ((const MapPair*)b)->id;
    return (ia > ib) - (ia < ib);
}

static unsigned hash_id(int id) {
    unsigned x = (unsigned)id;
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
    return x;
}

int cull_map_build(CullMap *m, const CullObject *objs, int n, int *err_index) {
    if (err_index) *err_index = -1;
    if (!m) return CULL_E_ARG;
    m->ids = m->at = m->hkey = m->hval = NULL; m->n = 0; m->hcap = 0;
    if (n < 0 || (!objs && n > 0)) return CULL_E_ARG;
    if (n == 0) return CULL_E_EMPTY;

    MapPair *p = (MapPair*)malloc(sizeof(MapPair) * (size_t)n);
    m->ids = (int*)malloc(sizeof(int) * (size_t)n);
    m->at = (int*)malloc(sizeof(int) * (size_t)n);
    if (!p || !m->ids || !m->at) { free(p); cull_map_free(m); return CULL_E_ARG; }
    for (int i = 0; i < n; ++i) { p[i].id = objs[i].id; p[i].at = i; }
    qsort(p, (size_t)n, sizeof(MapPair), cmp_pair);

    /* 重复：排序后相邻同号；报二者在原表中下标较大（即后出现）的那个。 */
    for (int i = 1; i < n; ++i) {
        if (p[i].id == p[i-1].id) {
            int idx = p[i].at > p[i-1].at ? p[i].at : p[i-1].at;
            if (err_index) *err_index = idx;
            free(p);
            cull_map_free(m);
            return CULL_E_DUP_ID;
        }
    }

    /* 哈希：容量取 2 的幂且 >= 2n，装载因子 <= 0.5。 */
    unsigned cap = 16;
    while (cap < (unsigned)n * 2u) cap <<= 1;
    m->hkey = (int*)malloc(sizeof(int) * (size_t)cap);
    m->hval = (int*)malloc(sizeof(int) * (size_t)cap);
    if (!m->hkey || !m->hval) { free(p); cull_map_free(m); return CULL_E_ARG; }
    for (unsigned i = 0; i < cap; ++i) m->hval[i] = -1;
    m->hcap = cap;

    for (int i = 0; i < n; ++i) {
        m->ids[i] = p[i].id;
        m->at[i] = p[i].at;
        unsigned h = hash_id(p[i].id) & (cap - 1);
        while (m->hval[h] >= 0) h = (h + 1) & (cap - 1);
        m->hkey[h] = p[i].id;
        m->hval[h] = p[i].at;
    }
    m->n = n;
    free(p);
    return CULL_OK;
}

void cull_map_free(CullMap *m) {
    if (!m) return;
    free(m->ids); free(m->at);
    free(m->hkey); free(m->hval);
    m->ids = m->at = m->hkey = m->hval = NULL;
    m->n = 0; m->hcap = 0;
}

int cull_map_find(const CullMap *m, int id) {
    if (!m || !m->ids || m->n <= 0) return -1;
    unsigned h = hash_id(id) & (m->hcap - 1u);
    for (unsigned step = 0; step < m->hcap; ++step) {
        if (m->hval[h] < 0) return -1;
        if (m->hkey[h] == id) return m->hval[h];
        h = (h + 1) & (m->hcap - 1u);
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* 阶段 1：按对象号合并                                                 */
/* ------------------------------------------------------------------ */

/* 16 位两趟 LSD 基数排序：稳定、O(n)，比 qsort 的间接比较快一个量级。
 * int 先翻最高位再按无符号排，负数也保持升序。 */
static void radix_sort_i32(const int *src, int n, int *dst) {
    unsigned *a = (unsigned*)malloc(sizeof(unsigned) * (size_t)n);
    unsigned *b = (unsigned*)malloc(sizeof(unsigned) * (size_t)n);
    int *cnt = (int*)malloc(sizeof(int) * 65536);
    if (!a || !b || !cnt) { /* 退路：内存不足时仍保证正确 */
        for (int i = 0; i < n; ++i) dst[i] = src[i];
        for (int i = 1; i < n; ++i) {
            int v = dst[i], j = i - 1;
            while (j >= 0 && dst[j] > v) { dst[j+1] = dst[j]; --j; }
            dst[j+1] = v;
        }
        free(a); free(b); free(cnt);
        return;
    }
    for (int i = 0; i < n; ++i) a[i] = (unsigned)src[i] ^ 0x80000000u;
    for (int shift = 0; shift < 32; shift += 16) {
        for (int i = 0; i < 65536; ++i) cnt[i] = 0;
        for (int i = 0; i < n; ++i) ++cnt[(a[i] >> shift) & 0xffffu];
        int sum = 0;
        for (int i = 0; i < 65536; ++i) { int c = cnt[i]; cnt[i] = sum; sum += c; }
        for (int i = 0; i < n; ++i) b[cnt[(a[i] >> shift) & 0xffffu]++] = a[i];
        unsigned *t = a; a = b; b = t;
    }
    /* 两趟后结果回到 a（起始缓冲区）。 */
    for (int i = 0; i < n; ++i) dst[i] = (int)(a[i] ^ 0x80000000u);
    free(a); free(b); free(cnt);
}

int cull_merge_ids(const int *cand, int n, int *uniq, int *uniq_count, int *err_index) {
    if (err_index) *err_index = -1;
    if (!cand || !uniq || !uniq_count || n < 0) return CULL_E_ARG;
    *uniq_count = 0;
    if (n == 0) return CULL_E_EMPTY;

    int *sorted = (int*)malloc(sizeof(int) * (size_t)n);
    if (!sorted) return CULL_E_ARG;
    radix_sort_i32(cand, n, sorted);

    int u = 0;
    uniq[u++] = sorted[0];
    for (int i = 1; i < n; ++i)
        if (sorted[i] != uniq[u-1]) uniq[u++] = sorted[i];
    *uniq_count = u;
    free(sorted);
    return CULL_OK;
}

/* ------------------------------------------------------------------ */
/* 阶段 2：按原顺序输出                                                 */
/* ------------------------------------------------------------------ */

/* uniq 升序，建开放寻址哈希：uniq 值 -> 槽位，候选查表 O(1)。 */
typedef struct { int *key; int *slot; unsigned cap; } SlotTable;

static int slot_build(SlotTable *t, const int *uniq, int u) {
    unsigned cap = 16;
    while (cap < (unsigned)u * 2u) cap <<= 1;
    t->key = (int*)malloc(sizeof(int) * (size_t)cap);
    t->slot = (int*)malloc(sizeof(int) * (size_t)cap);
    t->cap = cap;
    if (!t->key || !t->slot) { free(t->key); free(t->slot); t->key = t->slot = NULL; return -1; }
    for (unsigned i = 0; i < cap; ++i) t->slot[i] = -1;
    for (int i = 0; i < u; ++i) {
        unsigned h = hash_id(uniq[i]) & (cap - 1u);
        while (t->slot[h] >= 0) h = (h + 1) & (cap - 1u);
        t->slot[h] = i;
        t->key[h] = uniq[i];
    }
    return 0;
}

static void slot_free(SlotTable *t) { free(t->key); free(t->slot); t->key = t->slot = NULL; t->cap = 0; }

static int slot_find(const SlotTable *t, int id) {
    unsigned h = hash_id(id) & (t->cap - 1u);
    for (unsigned step = 0; step < t->cap; ++step) {
        if (t->slot[h] < 0) return -1;
        if (t->key[h] == id) return t->slot[h];
        h = (h + 1) & (t->cap - 1u);
    }
    return -1;
}

int cull_order_original(const int *cand, int n, const int *uniq, int uniq_count,
                        int *out_ids, int *out_first, int *err_index) {
    if (err_index) *err_index = -1;
    if (!cand || !uniq || !out_ids || n < 0 || uniq_count < 0) return CULL_E_ARG;
    if (n == 0) return CULL_E_EMPTY;

    SlotTable t; t.key = t.slot = NULL; t.cap = 0;
    int *first = (int*)malloc(sizeof(int) * (size_t)(uniq_count > 0 ? uniq_count : 1));
    if (!first || slot_build(&t, uniq, uniq_count) < 0) {
        free(first); slot_free(&t);
        return CULL_E_ARG;
    }
    for (int i = 0; i < uniq_count; ++i) first[i] = -1;

    /* 第一遍：记录每个对象号首次出现的下标；缺项立即报候选下标。 */
    for (int i = 0; i < n; ++i) {
        int j = slot_find(&t, cand[i]);
        if (j < 0) { if (err_index) *err_index = i; free(first); slot_free(&t); return CULL_E_MAP_MISS; }
        if (first[j] < 0) first[j] = i;
    }
    /* 第二遍：按首次出现下标升序输出。 */
    int written = 0;
    for (int i = 0; i < n; ++i) {
        int j = slot_find(&t, cand[i]);
        if (first[j] == i) {
            out_ids[written] = cand[i];
            if (out_first) out_first[written] = i;
            ++written;
        }
    }
    free(first);
    slot_free(&t);
    return CULL_OK;
}

/* ------------------------------------------------------------------ */
/* 完整流程                                                             */
/* ------------------------------------------------------------------ */

void cull_draw_list_free(CullDrawList *d) {
    if (!d) return;
    free(d->ids); free(d->first);
    d->ids = d->first = NULL; d->count = 0;
}

int cull_draw_list(const int *cand, int ncand, const CullMap *map, int merge,
                   CullDrawList *out, int *err_index) {
    if (err_index) *err_index = -1;
    if (!out) return CULL_E_ARG;
    out->ids = out->first = NULL; out->count = 0;
    if (!cand || ncand < 0) return CULL_E_ARG;
    if (ncand == 0) return CULL_E_EMPTY;

    /* 映射缺项：先于合并校验，报候选下标。 */
    if (map) {
        for (int i = 0; i < ncand; ++i) {
            if (cull_map_find(map, cand[i]) < 0) {
                if (err_index) *err_index = i;
                return CULL_E_MAP_MISS;
            }
        }
    }

    if (!merge) {
        /* 关闭合并：原样输出，绘制调用数与候选逐项相同（画面照旧）。 */
        out->ids = (int*)malloc(sizeof(int) * (size_t)(ncand > 0 ? ncand : 1));
        out->first = (int*)malloc(sizeof(int) * (size_t)(ncand > 0 ? ncand : 1));
        if (!out->ids || !out->first) { cull_draw_list_free(out); return CULL_E_ARG; }
        for (int i = 0; i < ncand; ++i) { out->ids[i] = cand[i]; out->first[i] = i; }
        out->count = ncand;
        return CULL_OK;
    }

    int ucount = 0;
    int *uniq = (int*)malloc(sizeof(int) * (size_t)(ncand > 0 ? ncand : 1));
    if (!uniq) return CULL_E_ARG;
    int rc = cull_merge_ids(cand, ncand, uniq, &ucount, err_index);
    if (rc != CULL_OK) { free(uniq); return rc; }

    out->ids = (int*)malloc(sizeof(int) * (size_t)ucount);
    out->first = (int*)malloc(sizeof(int) * (size_t)ucount);
    if (!out->ids || !out->first) { free(uniq); cull_draw_list_free(out); return CULL_E_ARG; }

    rc = cull_order_original(cand, ncand, uniq, ucount, out->ids, out->first, err_index);
    free(uniq);
    if (rc != CULL_OK) { cull_draw_list_free(out); return rc; }
    out->count = ucount;
    return CULL_OK;
}
