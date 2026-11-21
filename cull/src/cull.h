#ifndef CULL_H
#define CULL_H

#include <stddef.h>

/* Axis-aligned object box.
   vx, vy: velocity in pixels per second, used for adaptive tolerance.
   tol:    base tolerance in pixels added on top of the speed-derived one. */
typedef struct {
    double x, y, w, h;
    int layer;
    double vx, vy;
    double tol;
} CullBox;

typedef struct { double x, y, w, h; } CullView;
typedef struct { int *ids; int count; } CullResult;

/* Parameters of the two-stage adaptive cull.
   dt:     frame interval in seconds (must be finite and >= 0).
   margin: global viewport expansion in pixels applied in stage 2
           (must be finite and >= 0). 0 means no global expansion. */
typedef struct {
    double dt;
    double margin;
} CullParams;

/* Error record. When a function returns a negative code and err is
   non-NULL, field names the offending struct field (a static string)
   and index names the object, or -1 for a global parameter. */
typedef struct {
    int code;
    int index;
    const char *field;
    const char *reason;
} CullError;

/* Legacy API: exact overlap against the unexpanded viewport. */
int cull_run(const CullBox *boxes, int n, CullView view, CullResult *out);
int cull_count_within(const CullBox *boxes, int n, CullView view);

/* Stage 1: compute per-object adaptive tolerance.
   tols is caller-owned storage for n doubles.
   tolerance_i = boxes[i].tol + hypot(vx_i, vy_i) * params.dt */
int cull_apply_tolerance(const CullBox *boxes, int n, CullParams params,
                         double *tols, CullError *err);

/* Stage 2: filter boxes against the viewport expanded by params.margin,
   with each box additionally grown by its stage-1 tolerance.
   With every tolerance and margin at 0 the result equals cull_run. */
int cull_run_adaptive(const CullBox *boxes, int n, CullView view,
                      CullParams params, const double *tols,
                      CullResult *out, CullError *err);

/* Export candidate ids, one integer per line. 0 on success, -1 on I/O error. */
int cull_export(const char *path, const CullResult *r);

#endif
