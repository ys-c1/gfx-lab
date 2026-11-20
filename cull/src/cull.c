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

static int same_view(CullView a, CullView b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

int cull_validate_views(const CullView *views, int m, int max_views, int *bad_index) {
    if (bad_index) *bad_index = -1;
    if (!views) {
        if (m > 0) return -1;
        return 0;
    }
    if (m < 0 || m > max_views) {
        if (bad_index) *bad_index = m > max_views ? max_views : 0;
        return -3;
    }
    for (int j = 0; j < m; ++j) {
        /* Written as a positive test so a NaN size is rejected too. */
        if (!(views[j].w > 0.0) || !(views[j].h > 0.0)) {
            if (bad_index) *bad_index = j;
            return -2;
        }
        for (int k = 0; k < j; ++k) {
            if (same_view(views[j], views[k])) {
                if (bad_index) *bad_index = j;
                return -4;
            }
        }
    }
    return 0;
}

void cull_batch_free(CullBatchResult *r) {
    if (!r) return;
    free(r->counts);
    free(r->offsets);
    free(r->ids);
    r->counts = NULL;
    r->offsets = NULL;
    r->ids = NULL;
    r->views = r->total = 0;
    r->bad_index = -1;
}

/* Concatenate one cull_run result per viewport: the correctness reference
 * path used when batching is switched off. */
static int run_sequential(const CullBox *boxes, int n, const CullView *views, int m,
                          CullBatchResult *out) {
    int cap = n > 0 ? n : 1;
    if (m > 0 && cap > (int)(0x7fffffff / (long)m)) return -1;
    out->ids = (int*)malloc(sizeof(int) * (size_t)m * (size_t)cap);
    if (!out->ids) return -1;
    out->counts = (int*)calloc((size_t)m + 1, sizeof(int));
    out->offsets = (int*)malloc(sizeof(int) * ((size_t)m + 1));
    if (!out->counts || !out->offsets) {
        cull_batch_free(out);
        return -1;
    }
    out->offsets[0] = 0;
    for (int j = 0; j < m; ++j) {
        CullResult r;
        if (cull_run(boxes, n, views[j], &r) != 0) {
            cull_batch_free(out);
            return -1;
        }
        memcpy(out->ids + out->offsets[j], r.ids, sizeof(int) * (size_t)r.count);
        free(r.ids);
        out->counts[j] = r.count;
        out->offsets[j + 1] = out->offsets[j] + r.count;
    }
    return 0;
}

/* One traversal of the box list, testing every box against every viewport.
 * Hits land in per-viewport slots of n ids each; hits within a viewport stay
 * ascending because boxes are visited in index order. A final compaction
 * packs those slots into the concatenated output described by offsets. */
static int run_shared(const CullBox *boxes, int n, const CullView *views, int m,
                      CullBatchResult *out) {
    int cap = n > 0 ? n : 1;
    if (cap > (int)(0x7fffffff / (long)m)) return -1;
    size_t cap_total = (size_t)m * (size_t)cap;
    out->counts = (int*)calloc((size_t)m + 1, sizeof(int));
    out->offsets = (int*)malloc(sizeof(int) * ((size_t)m + 1));
    out->ids = (int*)malloc(sizeof(int) * cap_total);
    if (!out->counts || !out->offsets || !out->ids) {
        cull_batch_free(out);
        return -1;
    }
    if (n == 0) {
        out->offsets[0] = 0;
        for (int j = 0; j < m; ++j) out->offsets[j + 1] = 0;
        return 0;
    }

    int *slot = out->ids;   /* slot[j * cap] holds viewport j's hits */
    for (int i = 0; i < n; ++i) {
        CullBox b = boxes[i];
        for (int j = 0; j < m; ++j)
            if (overlaps(b, views[j])) slot[(size_t)j * (size_t)cap + (size_t)out->counts[j]++] = i;
    }

    /* Compact in place, lowest viewport slot first, so every source range is
     * still ahead of the write cursor and memmove moves it left or not at all. */
    int write = 0;
    for (int j = 0; j < m; ++j) {
        out->offsets[j] = write;
        if (out->counts[j] > 0)
            memmove(out->ids + write, out->ids + (size_t)j * (size_t)cap,
                    sizeof(int) * (size_t)out->counts[j]);
        write += out->counts[j];
    }
    out->offsets[m] = write;
    return 0;
}

int cull_run_batch(const CullBox *boxes, int n, const CullView *views, int m,
                   const CullBatchOpts *opts, CullBatchResult *out) {
    if (!boxes || !out || n < 0 || m < 0) return -1;
    if (m > 0 && !views) return -1;

    int max_views = opts && opts->max_views > 0 ? opts->max_views : CULL_MAX_VIEWS;
    if (m > max_views) {
        out->bad_index = max_views;
        return -3;
    }

    out->views = m;
    out->total = 0;
    out->counts = NULL;
    out->offsets = NULL;
    out->ids = NULL;
    out->bad_index = -1;
    if (m == 0) return 0;

    int bad = -1;
    int rc = cull_validate_views(views, m, max_views, &bad);
    if (rc != 0) {
        out->bad_index = bad;
        return rc;
    }

    rc = opts && opts->disabled ? run_sequential(boxes, n, views, m, out)
                                : run_shared(boxes, n, views, m, out);
    if (rc != 0) return rc;

    int total = 0;
    for (int j = 0; j < m; ++j) total += out->counts[j];
    out->total = total;
    return 0;
}
