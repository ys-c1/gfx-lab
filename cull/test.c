#include "cull.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

static int same_batch(const CullBatchResult *a, const CullBatchResult *b) {
    if (a->views != b->views || a->total != b->total) return 0;
    for (int j = 0; j < a->views; ++j) {
        if (a->counts[j] != b->counts[j]) return 0;
        if (a->offsets[j] != b->offsets[j]) return 0;
        int len = a->counts[j];
        if (memcmp(a->ids + a->offsets[j], b->ids + b->offsets[j],
                   sizeof(int) * (size_t)len) != 0) return 0;
    }
    return a->offsets[a->views] == b->offsets[b->views];
}

int main(void) {
    CullBox b[4] = {{0,0,10,10,0},{100,100,10,10,1},{50,50,20,20,0},{-5,-5,10,10,2}};
    CullView v = {0, 0, 60, 60};
    CullResult r;
    CK(cull_run(b, 4, v, &r) == 0);
    CK(r.count == 3);
    CK(r.ids[0] == 0 && r.ids[1] == 2 && r.ids[2] == 3);
    CK(cull_count_within(b, 4, v) == 3);
    CullView bad = {0, 0, 0, 10};
    CK(cull_run(b, 4, bad, &r) == -2);
    free(r.ids);

    /* --- batch: per-viewport sets, counts and aggregated total --- */
    CullView views[3] = {{0,0,60,60},{95,95,20,20},{-10,-10,30,30}};
    CullBatchOpts opts = {0, 0};
    CullBatchResult batch, seq;
    CK(cull_run_batch(b, 4, views, 3, &opts, &batch) == 0);
    CK(batch.views == 3);
    CK(batch.counts[0] == 3 && batch.counts[1] == 1 && batch.counts[2] == 2);
    CK(batch.total == 6);
    CK(batch.offsets[0] == 0 && batch.offsets[1] == 3 && batch.offsets[2] == 4
       && batch.offsets[3] == 6);
    CK(batch.ids[0] == 0 && batch.ids[1] == 2 && batch.ids[2] == 3);
    CK(batch.ids[3] == 1);
    CK(batch.ids[4] == 0 && batch.ids[5] == 3);

    /* each viewport agrees with an individual cull_run */
    for (int j = 0; j < 3; ++j) {
        CullResult one;
        CK(cull_run(b, 4, views[j], &one) == 0);
        CK(one.count == batch.counts[j]);
        CK(memcmp(one.ids, batch.ids + batch.offsets[j],
                  sizeof(int) * (size_t)one.count) == 0);
        free(one.ids);
    }

    /* switching batching off gives the same result */
    CullBatchOpts off = {1, 0};
    CK(cull_run_batch(b, 4, views, 3, &off, &seq) == 0);
    CK(same_batch(&batch, &seq));
    cull_batch_free(&seq);

    /* empty viewport list is allowed and empty */
    CullBatchResult none;
    CK(cull_run_batch(b, 4, NULL, 0, &opts, &none) == 0);
    CK(none.views == 0 && none.total == 0 && none.counts == NULL && none.ids == NULL);
    cull_batch_free(&none);
    cull_batch_free(&batch);

    /* --- rejection: empty viewport names its index --- */
    CullView empty_views[3] = {{0,0,60,60},{0,0,0,10},{95,95,20,20}};
    CullBatchResult rej;
    CK(cull_run_batch(b, 4, empty_views, 3, &opts, &rej) == -2);
    CK(rej.bad_index == 1);

    /* --- rejection: count above the default limit names the first over-limit index --- */
    enum { TOO_MANY = CULL_MAX_VIEWS + 1 };
    CullView *many = (CullView*)malloc(sizeof(CullView) * TOO_MANY);
    for (int j = 0; j < TOO_MANY; ++j) {
        many[j].x = j * 1000; many[j].y = 0; many[j].w = 10; many[j].h = 10;
    }
    CK(cull_run_batch(b, 4, many, TOO_MANY, &opts, &rej) == -3);
    CK(rej.bad_index == CULL_MAX_VIEWS);
    CullBatchOpts small = {0, 2};
    CK(cull_run_batch(b, 4, many, 3, &small, &rej) == -3);
    CK(rej.bad_index == 2);
    free(many);

    /* --- rejection: duplicate viewport names the later index --- */
    CullView dup_views[4] = {{0,0,60,60},{95,95,20,20},{0,0,60,60},{-10,-10,30,30}};
    CK(cull_run_batch(b, 4, dup_views, 4, &opts, &rej) == -4);
    CK(rej.bad_index == 2);
    /* a view differing only in size is not a duplicate */
    CullView near_views[2] = {{0,0,60,60},{0,0,60,61}};
    CK(cull_run_batch(b, 4, near_views, 2, &opts, &batch) == 0);
    cull_batch_free(&batch);

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
