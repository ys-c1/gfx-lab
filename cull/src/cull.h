#ifndef CULL_H
#define CULL_H

/* Largest number of viewports accepted by one cull_run_batch call. */
#define CULL_MAX_VIEWS 256

typedef struct { double x, y, w, h; int layer; } CullBox;
typedef struct { double x, y, w, h; } CullView;
typedef struct { int *ids; int count; } CullResult;

int cull_run(const CullBox *boxes, int n, CullView view, CullResult *out);
int cull_count_within(const CullBox *boxes, int n, CullView view);

/*
 * Batch viewport culling.
 *
 * A single traversal over the boxes tests each box against every viewport, so
 * a frame with many viewports does not walk the box list once per viewport.
 * The overlap predicate is exact, therefore counts[j] is both the candidate
 * count and the exact visible count for viewport j, and matches what a
 * separate cull_run call for that viewport returns.
 */
typedef struct {
    int disabled;  /* non-zero: evaluate every viewport through cull_run */
    int max_views; /* 0: use CULL_MAX_VIEWS */
} CullBatchOpts;

typedef struct {
    int views;     /* number of viewports supplied */
    int total;     /* sum of counts[j] over all viewports */
    int *counts;   /* counts[j]: candidates for viewport j (exact visible count) */
    int *offsets;  /* offsets[j]..offsets[j + 1] - 1 index into ids */
    int *ids;      /* concatenated box ids, ascending within each viewport */
    int bad_index; /* offending viewport index on rejection, else -1 */
} CullBatchResult;

/*
 * Validate viewports before a batch run.
 * Returns 0 on success, -1 for a null/invalid argument, -2 for an empty
 * viewport (w or h <= 0), -3 when m exceeds max_views, or -4 for a duplicate
 * viewport (same x, y, w and h as an earlier one). On failure *bad_index
 * receives the earliest offending index; for -3 that is max_views, the first
 * index beyond the limit.
 */
int cull_validate_views(const CullView *views, int m, int max_views, int *bad_index);

/*
 * Run a batch and populate out with one visible set and candidate count per
 * viewport plus the aggregated total. Returns 0, or -1/-2/-3/-4 as above;
 * out->bad_index is set on rejection. Release a successful result with
 * cull_batch_free.
 */
int cull_run_batch(const CullBox *boxes, int n, const CullView *views, int m,
                   const CullBatchOpts *opts, CullBatchResult *out);
void cull_batch_free(CullBatchResult *r);

#endif
