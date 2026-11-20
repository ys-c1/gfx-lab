#include "cull.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* One frame's worth of boxes: a dense field with heavy overlap. */
enum { N = 512, REPS = 2000 };

static void make_boxes(CullBox *b, int n) {
    unsigned s = 12345u;
    for (int i = 0; i < n; ++i) {
        s = s * 1664525u + 1013904223u;
        b[i].x = (double)(s % 1000u);
        s = s * 1664525u + 1013904223u;
        b[i].y = (double)(s % 1000u);
        b[i].w = 8 + (double)(s % 60u);
        b[i].h = 8 + (double)(s % 60u);
        b[i].layer = i % 4;
    }
}

/* Grid of viewports tiling the box field. */
static void make_views(CullView *v, int m) {
    int cols = 1;
    while (cols * cols < m) ++cols;
    for (int j = 0; j < m; ++j) {
        v[j].x = (double)((j % cols) * (1000 / cols));
        v[j].y = (double)((j / cols) * (1000 / cols));
        v[j].w = 1000.0 / cols;
        v[j].h = 1000.0 / cols;
    }
}

static double ms(clock_t from, clock_t to) {
    return (double)(to - from) * 1e3 / (double)CLOCKS_PER_SEC;
}

static void bench(int m, const CullBox *boxes, const CullView *views) {
    CullBatchOpts on = {0, 0}, off = {1, 0};
    CullBatchResult batch, seq;
    CullResult one;

    if (cull_run_batch(boxes, N, views, m, &on, &batch) != 0) {
        printf("%2d views: batch failed\n", m);
        return;
    }
    if (cull_run_batch(boxes, N, views, m, &off, &seq) != 0) {
        printf("%2d views: sequential batch failed\n", m);
        cull_batch_free(&batch);
        return;
    }

    /* Candidate counts vs. calling cull_run once per viewport: exact match,
     * both for the aggregate and for each individual viewport's id list. */
    int seq_total = 0, count_err = 0, set_mismatch = 0;
    for (int j = 0; j < m; ++j) {
        if (cull_run(boxes, N, views[j], &one) != 0) {
            printf("%2d views: single call failed\n", m);
            cull_batch_free(&batch);
            cull_batch_free(&seq);
            return;
        }
        int d = one.count - batch.counts[j];
        if (d < 0) d = -d;
        if (d > count_err) count_err = d;
        if (one.count != batch.counts[j] ||
            memcmp(one.ids, batch.ids + batch.offsets[j],
                   sizeof(int) * (size_t)one.count) != 0)
            ++set_mismatch;
        seq_total += one.count;
        free(one.ids);
    }
    int same_as_off = (seq.total == batch.total &&
                       seq.offsets[m] == batch.offsets[m] &&
                       memcmp(seq.counts, batch.counts, sizeof(int) * (size_t)m) == 0);

    clock_t b0 = clock();
    CullBatchResult timed;
    for (int r = 0; r < REPS; ++r) {
        if (cull_run_batch(boxes, N, views, m, &on, &timed) != 0) return;
        cull_batch_free(&timed);
    }
    clock_t b1 = clock();

    clock_t q0 = clock();
    for (int r = 0; r < REPS; ++r)
        for (int j = 0; j < m; ++j) {
            if (cull_run(boxes, N, views[j], &one) != 0) return;
            free(one.ids);
        }
    clock_t q1 = clock();

    double tb = ms(b0, b1) / REPS, tq = ms(q0, q1) / REPS;
    printf("%2d views: total %d (per-viewport calls: %d), count error %d, "
           "set mismatches %d, batch-off identical %s\n",
           m, seq.total, seq_total, count_err, set_mismatch,
           same_as_off ? "yes" : "no");
    printf("         batch %.3f ms/frame, one-at-a-time %.3f ms/frame, "
           "speedup %.2fx\n", tb, tq, (tb > 0.0 && tq > 0.0) ? tq / tb : 0.0);
    cull_batch_free(&batch);
    cull_batch_free(&seq);
}

int main(void) {
    CullBox *boxes = (CullBox*)malloc(sizeof(CullBox) * N);
    CullView *views = (CullView*)malloc(sizeof(CullView) * 32);
    if (!boxes || !views) return 1;
    make_boxes(boxes, N);
    make_views(views, 32);
    bench(4, boxes, views);
    bench(32, boxes, views);
    free(views);
    free(boxes);
    return 0;
}
