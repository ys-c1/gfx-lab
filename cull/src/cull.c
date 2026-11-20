#include "cull.h"
#include <stdlib.h>
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

/* ---- 分层稳定输出 ---- */

static unsigned long long g_cmp; /* qsort 比较计数, 非线程安全 */

typedef struct { int seq; int idx; } SeqIdx;
static int cmp_seq_idx(const void *pa, const void *pb) {
    const SeqIdx *a = (const SeqIdx*)pa, *b = (const SeqIdx*)pb;
    ++g_cmp;
    if (a->seq != b->seq) return a->seq < b->seq ? -1 : 1;
    return a->idx < b->idx ? -1 : (a->idx > b->idx);
}

typedef struct { int rank; int seq; int idx; } VisItem;
static int cmp_vis(const void *pa, const void *pb) {
    const VisItem *a = (const VisItem*)pa, *b = (const VisItem*)pb;
    ++g_cmp;
    if (a->rank != b->rank) return a->rank < b->rank ? -1 : 1;
    if (a->seq != b->seq) return a->seq < b->seq ? -1 : 1;
    return 0; /* seq 已校验唯一 */
}

static int fail(CullStatus *st, int code, int index, const char *field) {
    if (st) { st->code = code; st->index = index; st->field = field; }
    return code;
}

int cull_run_layered(const CullBox *boxes, int n, CullView view,
                     int layered, CullLayered *out, CullStatus *st) {
    if (st) { st->code = CULL_OK; st->index = -1; st->field = 0; }
    if (out) memset(out, 0, sizeof(*out));
    if (!boxes || !out || n < 0) return fail(st, CULL_ERR_NULL, -1, 0);
    if (view.w <= 0 || view.h <= 0) return fail(st, CULL_ERR_VIEW, -1, 0);

    g_cmp = 0;

    /* 阶段一: 校验 + 分桶 */

    /* layer 非负 */
    for (int i = 0; i < n; ++i)
        if (boxes[i].layer < 0)
            return fail(st, CULL_ERR_LAYER_NEGATIVE, i, "layer");

    /* seq 全局唯一: 排序后查相邻重复 */
    SeqIdx *tmp = (SeqIdx*)malloc(sizeof(SeqIdx) * (size_t)(n > 0 ? n : 1));
    if (!tmp) return fail(st, CULL_ERR_NULL, -1, 0);
    for (int i = 0; i < n; ++i) { tmp[i].seq = boxes[i].seq; tmp[i].idx = i; }
    qsort(tmp, (size_t)n, sizeof(SeqIdx), cmp_seq_idx);
    for (int i = 1; i < n; ++i)
        if (tmp[i].seq == tmp[i - 1].seq) {
            int bad = tmp[i].idx; /* 后插入的重复者 */
            free(tmp);
            return fail(st, CULL_ERR_SEQ_DUP, bad, "seq");
        }
    free(tmp);

    /* 收集不同层号, 超上限拒绝 */
    int layers[CULL_MAX_BUCKETS];
    int nl = 0;
    for (int i = 0; i < n; ++i) {
        int L = boxes[i].layer, found = 0;
        for (int j = 0; j < nl; ++j)
            if (layers[j] == L) { found = 1; break; }
        if (!found) {
            if (nl == CULL_MAX_BUCKETS)
                return fail(st, CULL_ERR_BUCKETS_OVERFLOW, i, "layer");
            layers[nl++] = L;
        }
    }
    /* 桶按层号升序 (插入排序, nl <= 64) */
    for (int i = 1; i < nl; ++i) {
        int L = layers[i], j = i;
        while (j > 0 && layers[j - 1] > L) { layers[j] = layers[j - 1]; --j; }
        layers[j] = L;
    }

    /* 每盒桶号 + 每桶总数 */
    int *rank = (int*)malloc(sizeof(int) * (size_t)(n > 0 ? n : 1));
    int *total = (int*)calloc((size_t)(nl > 0 ? nl : 1), sizeof(int));
    int *viscount = (int*)calloc((size_t)(nl > 0 ? nl : 1), sizeof(int));
    VisItem *vis = (VisItem*)malloc(sizeof(VisItem) * (size_t)(n > 0 ? n : 1));
    out->ids = (int*)malloc(sizeof(int) * (size_t)(n > 0 ? n : 1));
    out->buckets = (CullBucket*)malloc(sizeof(CullBucket) * (size_t)(nl > 0 ? nl : 1));
    if (!rank || !total || !viscount || !vis || !out->ids || !out->buckets) {
        free(rank); free(total); free(viscount); free(vis);
        free(out->ids); free(out->buckets);
        memset(out, 0, sizeof(*out));
        return fail(st, CULL_ERR_NULL, -1, 0);
    }
    for (int i = 0; i < n; ++i) {
        int r = 0;
        while (layers[r] != boxes[i].layer) ++r;
        rank[i] = r;
        ++total[r];
    }

    /* 阶段二: 可见性判定, 桶内按 seq 稳定输出, 每桶统计 */

    int m = 0;
    for (int i = 0; i < n; ++i)
        if (overlaps(boxes[i], view)) {
            vis[m].rank = rank[i];
            vis[m].seq = boxes[i].seq;
            vis[m].idx = i;
            ++m;
            ++viscount[rank[i]];
        }
    if (layered) qsort(vis, (size_t)m, sizeof(VisItem), cmp_vis);
    /* 否则 vis 已是插入序, 与 cull_run 一致 */
    for (int k = 0; k < m; ++k) out->ids[k] = vis[k].idx;
    out->count = m;

    for (int j = 0; j < nl; ++j) {
        out->buckets[j].layer = layers[j];
        out->buckets[j].visible = viscount[j];
        out->buckets[j].clipped = total[j] - viscount[j];
    }
    out->bucket_count = nl;
    out->comparisons = g_cmp;

    free(rank); free(total); free(viscount); free(vis);
    return CULL_OK;
}

void cull_layered_free(CullLayered *out) {
    if (!out) return;
    free(out->ids);
    free(out->buckets);
    memset(out, 0, sizeof(*out));
}
