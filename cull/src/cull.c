#include "cull.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

static int overlaps(CullBox b, CullView v) {
    return !(b.x + b.w < v.x || b.x > v.x + v.w || b.y + b.h < v.y || b.y > v.y + v.h);
}

/* ---------------- legacy API (unchanged) ---------------- */

int cull_run(const CullBox *boxes, int n, CullView view, CullResult *out) {
    if (!boxes || !out || n < 0) return CULL_E_ARG;
    if (view.w <= 0 || view.h <= 0) return CULL_E_VIEW;
    out->ids = (int*)malloc(sizeof(int) * (size_t)(n > 0 ? n : 1));
    if (!out->ids) return CULL_E_NOMEM;
    out->count = 0;
    for (int i = 0; i < n; ++i)
        if (overlaps(boxes[i], view)) out->ids[out->count++] = i;
    return CULL_OK;
}

int cull_count_within(const CullBox *boxes, int n, CullView view) {
    if (!boxes || n < 0) return CULL_E_ARG;
    int c = 0;
    for (int i = 0; i < n; ++i) if (overlaps(boxes[i], view)) ++c;
    return c;
}

/* ---------------- animated culling ---------------- */

const char *cull_strerror(int code) {
    switch (code) {
    case CULL_OK:       return "ok";
    case CULL_E_ARG:    return "invalid argument: null pointer or n < 0";
    case CULL_E_VIEW:   return "invalid view: width and height must be > 0";
    case CULL_E_EMPTY:  return "empty input: n == 0, there is nothing to cull";
    case CULL_E_T:      return "invalid coefficient: t must be within [0, 1]";
    case CULL_E_STATE:  return "invalid state: cull_anim_init was not called";
    case CULL_E_NOMEM:  return "allocation failed";
    case CULL_E_IO:     return "csv export failed: cannot open output file";
    default:            return "unknown cull error";
    }
}

CullView cull_view_lerp(CullView a, CullView b, double t) {
    CullView r;
    r.x = a.x + (b.x - a.x) * t;
    r.y = a.y + (b.y - a.y) * t;
    r.w = a.w + (b.w - a.w) * t;
    r.h = a.h + (b.h - a.h) * t;
    return r;
}

static CullView union_view(CullView a, CullView b) {
    CullView r;
    r.x = fmin(a.x, b.x);
    r.y = fmin(a.y, b.y);
    double ax = fmax(a.x + a.w, b.x + b.w);
    double ay = fmax(a.y + a.h, b.y + b.h);
    r.w = ax - r.x;
    r.h = ay - r.y;
    return r;
}

/* counts of elements that appear only in a (removed when moving a->b)
   and only in b (added); common elements are counted by caller */
static void diff_sorted(const int *a, int na, const int *b, int nb,
                        int *only_a, int *only_b) {
    int i = 0, j = 0;
    *only_a = 0; *only_b = 0;
    while (i < na && j < nb) {
        if (a[i] < b[j]) { ++*only_a; ++i; }
        else if (a[i] > b[j]) { ++*only_b; ++j; }
        else { ++i; ++j; }
    }
    *only_a += na - i;
    *only_b += nb - j;
}

static int push_record(CullAnim *a, const CullStepInfo *info) {
    if (a->nhist == a->hcap) {
        int ncap = a->hcap ? a->hcap * 2 : 16;
        CullFrameRecord *p = (CullFrameRecord*)realloc(
            a->hist, (size_t)ncap * sizeof(*p));
        if (!p) return CULL_E_NOMEM;
        a->hist = p; a->hcap = ncap;
    }
    CullFrameRecord *rec = &a->hist[a->nhist++];
    rec->frame = info->frame;
    rec->t = info->t;
    rec->vx = info->view.x; rec->vy = info->view.y;
    rec->vw = info->view.w; rec->vh = info->view.h;
    rec->candidates = info->candidates;
    rec->rebuilt = info->rebuilt;
    rec->added = info->added; rec->removed = info->removed;
    rec->unchanged = info->unchanged;
    rec->visible_count = info->visible_count;
    rec->visible_ids = (int*)malloc(sizeof(int) * (size_t)(info->visible_count > 0 ? info->visible_count : 1));
    if (!rec->visible_ids) { --a->nhist; return CULL_E_NOMEM; }
    memcpy(rec->visible_ids, info->visible,
           (size_t)info->visible_count * sizeof(int));
    return CULL_OK;
}

static void fill_info(CullAnim *a, CullStepInfo *info, double t, int rebuilt,
                      int added, int removed, int unchanged) {
    if (!info) return;
    info->frame = a->frame;
    info->t = t;
    info->view = a->cur;
    info->candidates = a->ncand;
    info->rebuilt = rebuilt;
    info->added = added;
    info->removed = removed;
    info->unchanged = unchanged;
    info->visible_count = a->nvis;
    info->visible = a->visible;
}

int cull_anim_init(CullAnim *a, const CullBox *boxes, int n,
                   CullView v0, CullView v1, int flags,
                   CullStepInfo *first) {
    if (!a) return CULL_E_ARG;
    memset(a, 0, sizeof(*a));
    if (!boxes || n < 0) return CULL_E_ARG;
    if (n == 0) return CULL_E_EMPTY;
    if (v0.w <= 0 || v0.h <= 0 || v1.w <= 0 || v1.h <= 0) return CULL_E_VIEW;

    a->boxes = boxes; a->n = n; a->v0 = v0; a->v1 = v1; a->flags = flags;

    a->cand = (int*)malloc(sizeof(int) * (size_t)n);
    a->visible = (int*)malloc(sizeof(int) * (size_t)n);
    a->prev = (int*)malloc(sizeof(int) * (size_t)n);
    if (!a->cand || !a->visible || !a->prev) { cull_anim_free(a); return CULL_E_NOMEM; }

    /* candidate set: every box overlapping the union of the two keyframes.
       every interpolated view lies inside that union, so any box outside
       the union is invisible for the whole trajectory and never changes. */
    CullView uni = union_view(v0, v1);
    for (int i = 0; i < n; ++i)
        if (overlaps(boxes[i], uni)) a->cand[a->ncand++] = i;

    /* frame 0 at t = 0. rebuilt == 1 means "all n boxes were tested"; the
       incremental path only tests candidates, so frame 0 is an initial
       build (added == visible_count) rather than a full rebuild. */
    a->cur = v0;
    int rebuilt = (flags & CULL_ANIM_FULL_REBUILD) ? 1 : 0;
    a->nvis = 0;
    if (rebuilt) {
        for (int i = 0; i < n; ++i)
            if (overlaps(boxes[i], a->cur)) a->visible[a->nvis++] = i;
        a->rebuilds = 1;
    } else {
        for (int k = 0; k < a->ncand; ++k) {
            int id = a->cand[k];
            if (overlaps(boxes[id], a->cur)) a->visible[a->nvis++] = id;
        }
    }
    CullStepInfo tmp;
    fill_info(a, &tmp, 0.0, rebuilt, a->nvis, 0, 0);
    int rc = push_record(a, &tmp);
    if (rc) { cull_anim_free(a); return rc; }
    memcpy(a->prev, a->visible, (size_t)a->nvis * sizeof(int));
    a->nprev = a->nvis;
    fill_info(a, first, 0.0, rebuilt, a->nvis, 0, 0);
    return CULL_OK;
}

int cull_anim_step(CullAnim *a, double t, CullStepInfo *info) {
    if (!a) return CULL_E_ARG;
    if (!a->boxes) return CULL_E_STATE;
    if (!(t >= 0.0 && t <= 1.0)) return CULL_E_T;

    /* phase 1: interpolate the intermediate viewport */
    a->cur = cull_view_lerp(a->v0, a->v1, t);
    ++a->frame;

    int *tmp = a->prev;          /* swap: prev becomes scratch buffer */
    a->prev = a->visible;
    a->visible = tmp;
    int oldn = a->nprev;
    a->nvis = 0;

    int rebuilt = (a->flags & CULL_ANIM_FULL_REBUILD) ? 1 : 0;

    /* phase 2: full rebuild, or only re-test the candidate subset */
    if (rebuilt) {
        for (int i = 0; i < a->n; ++i)
            if (overlaps(a->boxes[i], a->cur)) a->visible[a->nvis++] = i;
        ++a->rebuilds;
    } else {
        for (int k = 0; k < a->ncand; ++k) {
            int id = a->cand[k];
            if (overlaps(a->boxes[id], a->cur)) a->visible[a->nvis++] = id;
        }
    }

    int removed, added;
    diff_sorted(a->prev, oldn, a->visible, a->nvis, &removed, &added);
    int unchanged = a->nvis - added;

    CullStepInfo local;
    fill_info(a, &local, t, rebuilt, added, removed, unchanged);
    int rc = push_record(a, &local);
    if (rc) return rc;

    memcpy(a->prev, a->visible, (size_t)a->nvis * sizeof(int));
    a->nprev = a->nvis;
    fill_info(a, info, t, rebuilt, added, removed, unchanged);
    return CULL_OK;
}

void cull_anim_free(CullAnim *a) {
    if (!a) return;
    free(a->cand);
    free(a->visible);
    free(a->prev);
    if (a->hist) {
        for (int i = 0; i < a->nhist; ++i) free(a->hist[i].visible_ids);
        free(a->hist);
    }
    memset(a, 0, sizeof(*a));
}

int cull_anim_export_csv(const CullAnim *a, const char *path) {
    if (!a || !path || !a->boxes) return CULL_E_ARG;
    FILE *f = fopen(path, "w");
    if (!f) return CULL_E_IO;
    fprintf(f, "frame,t,vx,vy,vw,vh,candidates,rebuilt,added,removed,unchanged,visible_count,visible_ids\n");
    for (int i = 0; i < a->nhist; ++i) {
        const CullFrameRecord *r = &a->hist[i];
        fprintf(f, "%d,%.6f,%.6f,%.6f,%.6f,%.6f,%d,%d,%d,%d,%d,%d,",
                r->frame, r->t, r->vx, r->vy, r->vw, r->vh,
                r->candidates, r->rebuilt, r->added, r->removed,
                r->unchanged, r->visible_count);
        for (int j = 0; j < r->visible_count; ++j)
            fprintf(f, "%s%d", j ? "|" : "", r->visible_ids[j]);
        fputc('\n', f);
    }
    fclose(f);
    return CULL_OK;
}

int cull_anim_export_boxes(const CullBox *boxes, int n, const char *path) {
    if (!boxes || n < 0 || !path) return CULL_E_ARG;
    FILE *f = fopen(path, "w");
    if (!f) return CULL_E_IO;
    fprintf(f, "id,x,y,w,h,layer\n");
    for (int i = 0; i < n; ++i)
        fprintf(f, "%d,%.6f,%.6f,%.6f,%.6f,%d\n",
                i, boxes[i].x, boxes[i].y, boxes[i].w, boxes[i].h, boxes[i].layer);
    fclose(f);
    return CULL_OK;
}
