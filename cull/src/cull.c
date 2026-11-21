#include "cull.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int overlaps(const CullBox *b, CullView v) {
    return !(b->x + b->w < v.x || b->x > v.x + v.w ||
             b->y + b->h < v.y || b->y > v.y + v.h);
}

static void set_error(CullError *err, int code, int index, const char *field,
                      const char *reason) {
    if (!err) return;
    err->code = code;
    err->index = index;
    err->field = field;
    err->reason = reason;
}

/* Returns 0 when inputs are acceptable, a negative code otherwise. */
static int validate_inputs(const CullBox *boxes, int n, CullView view,
                           const double *tols, CullError *err) {
    if (!boxes || n < 0) {
        set_error(err, -1, -1, "boxes", "null pointer or negative count");
        return -1;
    }
    if (view.w <= 0 || view.h <= 0) {
        set_error(err, -2, -1, "view.w/view.h", "viewport size must be positive");
        return -2;
    }
    if (!tols) {
        set_error(err, -1, -1, "tols", "stage-1 tolerances are required");
        return -1;
    }
    for (int i = 0; i < n; ++i) {
        if (!isfinite(boxes[i].vx) || !isfinite(boxes[i].vy)) {
            set_error(err, -3, i, "vx/vy",
                      "velocity components must be finite");
            return -3;
        }
        if (!isfinite(boxes[i].tol) || boxes[i].tol < 0.0) {
            set_error(err, -3, i, "tol",
                      "base tolerance must be finite and non-negative");
            return -3;
        }
        if (!isfinite(tols[i]) || tols[i] < 0.0) {
            set_error(err, -3, i, "tolerance",
                      "adaptive tolerance must be finite and non-negative");
            return -3;
        }
    }
    return 0;
}

int cull_run(const CullBox *boxes, int n, CullView view, CullResult *out) {
    if (!boxes || !out || n < 0) return -1;
    if (view.w <= 0 || view.h <= 0) return -2;
    out->ids = (int *)malloc(sizeof(int) * (size_t)(n > 0 ? n : 1));
    if (!out->ids) return -1;
    out->count = 0;
    for (int i = 0; i < n; ++i)
        if (overlaps(&boxes[i], view)) out->ids[out->count++] = i;
    return 0;
}

int cull_count_within(const CullBox *boxes, int n, CullView view) {
    if (!boxes || n < 0) return -1;
    int c = 0;
    for (int i = 0; i < n; ++i)
        if (overlaps(&boxes[i], view)) ++c;
    return c;
}

int cull_apply_tolerance(const CullBox *boxes, int n, CullParams params,
                         double *tols, CullError *err) {
    if (!boxes || n < 0 || !tols) {
        set_error(err, -1, -1, "boxes/tols", "null pointer or negative count");
        return -1;
    }
    if (!isfinite(params.dt) || params.dt < 0.0) {
        set_error(err, -3, -1, "dt",
                  "frame interval must be finite and non-negative");
        return -3;
    }
    if (!isfinite(params.margin) || params.margin < 0.0) {
        set_error(err, -3, -1, "margin",
                  "viewport margin must be finite and non-negative");
        return -3;
    }
    for (int i = 0; i < n; ++i) {
        if (!isfinite(boxes[i].vx) || !isfinite(boxes[i].vy)) {
            set_error(err, -3, i, "vx/vy",
                      "velocity components must be finite");
            return -3;
        }
        if (!isfinite(boxes[i].tol) || boxes[i].tol < 0.0) {
            set_error(err, -3, i, "tol",
                      "base tolerance must be finite and non-negative");
            return -3;
        }
        tols[i] = boxes[i].tol + hypot(boxes[i].vx, boxes[i].vy) * params.dt;
    }
    return 0;
}

/* Box grown by tol vs viewport grown by margin: identical overlap test as
   overlaps() once both rectangles are expanded, so an all-zero expansion
   reproduces the legacy result exactly. */
static int grown_overlaps(const CullBox *b, double tol, CullView v,
                          double margin) {
    double bx0 = b->x - tol, bx1 = b->x + b->w + tol;
    double by0 = b->y - tol, by1 = b->y + b->h + tol;
    double vx0 = v.x - margin, vx1 = v.x + v.w + margin;
    double vy0 = v.y - margin, vy1 = v.y + v.h + margin;
    return !(bx1 < vx0 || bx0 > vx1 || by1 < vy0 || by0 > vy1);
}

int cull_run_adaptive(const CullBox *boxes, int n, CullView view,
                      CullParams params, const double *tols,
                      CullResult *out, CullError *err) {
    if (!out) {
        set_error(err, -1, -1, "out", "result pointer is null");
        return -1;
    }
    if (!isfinite(params.dt) || params.dt < 0.0) {
        set_error(err, -3, -1, "dt",
                  "frame interval must be finite and non-negative");
        return -3;
    }
    if (!isfinite(params.margin) || params.margin < 0.0) {
        set_error(err, -3, -1, "margin",
                  "viewport margin must be finite and non-negative");
        return -3;
    }
    int rc = validate_inputs(boxes, n, view, tols, err);
    if (rc != 0) return rc;

    out->ids = (int *)malloc(sizeof(int) * (size_t)(n > 0 ? n : 1));
    if (!out->ids) {
        set_error(err, -1, -1, "out.ids", "allocation failed");
        return -1;
    }
    out->count = 0;
    for (int i = 0; i < n; ++i)
        if (grown_overlaps(&boxes[i], tols[i], view, params.margin))
            out->ids[out->count++] = i;
    return 0;
}

int cull_export(const char *path, const CullResult *r) {
    if (!path || !r) return -1;
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    for (int i = 0; i < r->count; ++i)
        if (fprintf(f, "%d\n", r->ids[i]) < 0) {
            fclose(f);
            return -1;
        }
    if (fclose(f) != 0) return -1;
    return 0;
}
