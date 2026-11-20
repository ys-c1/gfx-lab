#include "cull.h"
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* View culling (unchanged)                                            */
/* ------------------------------------------------------------------ */
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
/* Two-stage occlusion culling                                         */
/* ------------------------------------------------------------------ */
static int sprite_ok(const CullSprite *s) {
    return isfinite(s->x) && isfinite(s->y) && isfinite(s->w) && isfinite(s->h) &&
           isfinite(s->depth);
}

int cull_depth_init(CullDepthMap *m, int w, int h,
                    double x0, double y0, double x1, double y1) {
    if (!m) return CULL_ERR_ARG;
    m->depth = NULL;
    m->cover = NULL;
    m->w = m->h = 0;
    m->covered_cells = 0;
    m->rendered = 0;
    if (w <= 0 || h <= 0) return CULL_ERR_ARG;
    if (!isfinite(x0) || !isfinite(y0) || !isfinite(x1) || !isfinite(y1)) return CULL_ERR_ARG;
    double rw = x1 - x0, rh = y1 - y0;
    if (!(rw > 0.0) || !(rh > 0.0)) return CULL_ERR_ARG;
    size_t cells = (size_t)w * (size_t)h;
    m->depth = (float*)malloc(sizeof(float) * cells);
    m->cover = (unsigned char*)calloc(cells, 1);
    if (!m->depth || !m->cover) { cull_depth_free(m); return CULL_ERR_ARG; }
    for (size_t i = 0; i < cells; ++i) m->depth[i] = FLT_MAX;
    m->w = w; m->h = h;
    m->x0 = x0; m->y0 = y0; m->x1 = x1; m->y1 = y1;
    m->cell_w = rw / (double)w;
    m->cell_h = rh / (double)h;
    return CULL_OK;
}

void cull_depth_free(CullDepthMap *m) {
    if (!m) return;
    free(m->depth);
    free(m->cover);
    m->depth = NULL;
    m->cover = NULL;
    m->w = m->h = 0;
    m->covered_cells = 0;
    m->rendered = 0;
}

/* Cells completely inside [lo, hi].  Used when rasterizing occluders: a cell
 * only receives a depth when the occluder covers the whole cell, so a
 * candidate that is culled really is blocked over its entire footprint. */
static int cells_fully_inside(double lo, double hi, double origin, double cell,
                              int count, int *first, int *last) {
    double a = (lo - origin) / cell;
    double b = (hi - origin) / cell;
    if (!isfinite(a) || !isfinite(b) || !(b > a)) return 0;
    double eps = 1e-12 * (fabs(a) + fabs(b) + 1.0);
    double f = ceil(a - eps);
    double l = floor(b - 1.0 + eps); /* cell k: k >= a and k+1 <= b */
    if (f < 0.0) f = 0.0;
    if (l > (double)(count - 1)) l = (double)(count - 1);
    if (f > l || l < 0.0 || f > (double)(count - 1)) return 0;
    *first = (int)f;
    *last = (int)l;
    return 1;
}

/* Cells whose interior the rectangle intersects with positive area.  Used for
 * candidate footprints; expanding by an epsilon only ever keeps more
 * candidates visible, never culls one wrongly. */
static int cells_touched(double lo, double hi, double origin, double cell,
                         int count, int *first, int *last) {
    double a = (lo - origin) / cell;
    double b = (hi - origin) / cell;
    if (!isfinite(a) || !isfinite(b) || !(b > a)) return 0;
    double eps = 1e-12 * (fabs(a) + fabs(b) + 1.0);
    double f = floor(a + eps);
    double l = ceil(b - eps) - 1.0; /* cell k: k < b and k+1 > a */
    if (f < 0.0) f = 0.0;
    if (l > (double)(count - 1)) l = (double)(count - 1);
    if (f > l || l < 0.0 || f > (double)(count - 1)) return 0;
    *first = (int)f;
    *last = (int)l;
    return 1;
}

int cull_depth_render(CullDepthMap *m, const CullSprite *occluders, int n,
                      int *err_index) {
    if (err_index) *err_index = -1;
    if (!m || !m->depth || !m->cover || m->w <= 0 || m->h <= 0) return CULL_ERR_ARG;
    if (n < 0 || (n > 0 && !occluders)) return CULL_ERR_ARG;

    m->rendered = 0;
    m->covered_cells = 0;
    size_t cells = (size_t)m->w * (size_t)m->h;
    for (size_t i = 0; i < cells; ++i) { m->depth[i] = FLT_MAX; m->cover[i] = 0; }

    for (int i = 0; i < n; ++i) {
        const CullSprite *s = &occluders[i];
        if (!sprite_ok(s)) {
            if (err_index) *err_index = i;
            return CULL_ERR_NONFINITE;
        }
        if (s->w < 0.0 || s->h < 0.0) {
            if (err_index) *err_index = i;
            return CULL_ERR_RECT;
        }
        if (s->w == 0.0 || s->h == 0.0) continue;
        int fx, lx, fy, ly;
        if (!cells_fully_inside(s->x, s->x + s->w, m->x0, m->cell_w, m->w, &fx, &lx)) continue;
        if (!cells_fully_inside(s->y, s->y + s->h, m->y0, m->cell_h, m->h, &fy, &ly)) continue;
        float d = (float)s->depth;
        for (int y = fy; y <= ly; ++y) {
            size_t row = (size_t)y * (size_t)m->w;
            for (int x = fx; x <= lx; ++x) {
                size_t k = row + (size_t)x;
                if (!m->cover[k]) { m->cover[k] = 1; ++m->covered_cells; }
                if (d < m->depth[k]) m->depth[k] = d;
            }
        }
    }
    m->rendered = 1;
    return CULL_OK;
}

/* A candidate is occluded only if its footprint lies inside the depth map and
 * every cell it touches has a strictly nearer occluder covering that whole
 * cell.  Cells without such coverage keep the candidate visible, so the
 * depth-map discretisation can only under-cull, never over-cull. */
static int sprite_occluded(const CullDepthMap *m, const CullSprite *s) {
    if (s->x < m->x0 || s->y < m->y0 ||
        s->x + s->w > m->x1 || s->y + s->h > m->y1) return 0;
    int fx, lx, fy, ly;
    if (!cells_touched(s->x, s->x + s->w, m->x0, m->cell_w, m->w, &fx, &lx)) return 0;
    if (!cells_touched(s->y, s->y + s->h, m->y0, m->cell_h, m->h, &fy, &ly)) return 0;
    float d = (float)s->depth;
    for (int y = fy; y <= ly; ++y) {
        size_t row = (size_t)y * (size_t)m->w;
        for (int x = fx; x <= lx; ++x) {
            size_t k = row + (size_t)x;
            if (!m->cover[k]) return 0;
            if (!(m->depth[k] < d)) return 0;
        }
    }
    return 1;
}

int cull_occlusion(const CullDepthMap *m, const CullSprite *candidates, int n,
                   int enabled, CullOcclusionResult *out, int *err_index) {
    if (err_index) *err_index = -1;
    if (!out || n < 0 || (n > 0 && !candidates)) return CULL_ERR_ARG;
    out->ids = NULL;
    out->count = 0;
    out->culled = 0;
    for (int i = 0; i < n; ++i) {
        if (!sprite_ok(&candidates[i])) {
            if (err_index) *err_index = i;
            return CULL_ERR_NONFINITE;
        }
        if (candidates[i].w < 0.0 || candidates[i].h < 0.0) {
            if (err_index) *err_index = i;
            return CULL_ERR_RECT;
        }
    }
    if (enabled) {
        if (!m || !m->depth || !m->cover || m->w <= 0 || m->h <= 0)
            return CULL_ERR_EMPTY_DEPTH;
        if (!m->rendered || m->covered_cells <= 0) return CULL_ERR_EMPTY_DEPTH;
    }
    out->ids = (int*)malloc(sizeof(int) * (size_t)(n > 0 ? n : 1));
    if (!out->ids) return CULL_ERR_ARG;
    for (int i = 0; i < n; ++i) {
        /* A degenerate (zero-area) candidate has no blockable footprint, so it
         * stays visible; this keeps the occlusion-off set identical to input. */
        if (!enabled || !sprite_occluded(m, &candidates[i])) out->ids[out->count++] = i;
    }
    out->culled = n - out->count;
    return CULL_OK;
}

void cull_occlusion_free(CullOcclusionResult *r) {
    if (!r) return;
    free(r->ids);
    r->ids = NULL;
    r->count = 0;
    r->culled = 0;
}

/* ------------------------------------------------------------------ */
/* Analytic reference                                                  */
/* ------------------------------------------------------------------ */
static int cmp_double(const void *a, const void *b) {
    double x = *(const double*)a, y = *(const double*)b;
    return (x > y) - (x < y);
}

static int ranges_overlap_2d(const CullSprite *a, const CullSprite *b) {
    return a->x < b->x + b->w && b->x < a->x + a->w &&
           a->y < b->y + b->h && b->y < a->y + a->h;
}

/* Sweep the candidate's x extent.  Inside each slab the set of occluders that
 * span the whole slab is fixed, so the candidate is visible iff in at least
 * one slab the union of their y intervals misses part of the candidate.
 * This is exact - no sampling, no resolution. */
int cull_reference_visible(const CullSprite *occluders, int nocc,
                           const CullSprite *candidates, int n,
                           unsigned char *visible, int *err_index) {
    if (err_index) *err_index = -1;
    if (!visible || n < 0 || (n > 0 && !candidates)) return CULL_ERR_ARG;
    if (nocc < 0 || (nocc > 0 && !occluders)) return CULL_ERR_ARG;
    for (int i = 0; i < nocc; ++i) {
        if (!sprite_ok(&occluders[i])) { if (err_index) *err_index = i; return CULL_ERR_NONFINITE; }
        if (occluders[i].w < 0.0 || occluders[i].h < 0.0) { if (err_index) *err_index = i; return CULL_ERR_RECT; }
    }
    for (int i = 0; i < n; ++i) {
        if (!sprite_ok(&candidates[i])) { if (err_index) *err_index = i; return CULL_ERR_NONFINITE; }
        if (candidates[i].w < 0.0 || candidates[i].h < 0.0) { if (err_index) *err_index = i; return CULL_ERR_RECT; }
    }
    if (nocc > 512) return CULL_ERR_ARG; /* fixed-size scratch below */

    enum { CAP = 1024 };
    double xs[CAP], iv[CAP];
    int cx[512], ncx;

    for (int i = 0; i < n; ++i) {
        const CullSprite *c = &candidates[i];
        if (c->w == 0.0 || c->h == 0.0) { visible[i] = 1; continue; } /* no footprint, nothing hides it */
        double cx0 = c->x, cx1 = c->x + c->w, cy0 = c->y, cy1 = c->y + c->h;

        int nx = 0;
        xs[nx++] = cx0;
        xs[nx++] = cx1;
        ncx = 0;
        for (int o = 0; o < nocc; ++o) {
            const CullSprite *s = &occluders[o];
            if (!(s->depth < c->depth) || !(s->w > 0.0 && s->h > 0.0)) continue;
            if (!ranges_overlap_2d(s, c)) continue;
            if (ncx >= (int)(sizeof(cx) / sizeof(cx[0]))) return CULL_ERR_ARG;
            cx[ncx++] = o;
            double a = s->x > cx0 ? s->x : cx0;
            double b = s->x + s->w < cx1 ? s->x + s->w : cx1;
            if (a > cx0 && a < cx1) { if (nx >= CAP) return CULL_ERR_ARG; xs[nx++] = a; }
            if (b > cx0 && b < cx1) { if (nx >= CAP) return CULL_ERR_ARG; xs[nx++] = b; }
        }
        if (nx > CAP) return CULL_ERR_ARG;
        qsort(xs, (size_t)nx, sizeof(double), cmp_double);

        int vis = 0;
        for (int k = 0; k + 1 < nx && !vis; ++k) {
            if (!(xs[k + 1] > xs[k])) continue;
            double mid = 0.5 * (xs[k] + xs[k + 1]);
            int cnt = 0;
            for (int j = 0; j < ncx; ++j) {
                const CullSprite *s = &occluders[cx[j]];
                if (!(s->x < mid && mid < s->x + s->w)) continue;
                double lo = s->y > cy0 ? s->y : cy0;
                double hi = s->y + s->h < cy1 ? s->y + s->h : cy1;
                if (hi > lo) { iv[2 * cnt] = lo; iv[2 * cnt + 1] = hi; ++cnt; }
            }
            /* insertion sort the intervals by lower edge (small counts) */
            for (int a = 1; a < cnt; ++a) {
                double l = iv[2 * a], h = iv[2 * a + 1];
                int b = a - 1;
                while (b >= 0 && iv[2 * b] > l) {
                    iv[2 * b + 2] = iv[2 * b];
                    iv[2 * b + 3] = iv[2 * b + 1];
                    --b;
                }
                iv[2 * b + 2] = l;
                iv[2 * b + 3] = h;
            }
            double reach = cy0;
            int gap = 0;
            for (int a = 0; a < cnt; ++a) {
                if (iv[2 * a] > reach) { gap = 1; break; }
                if (iv[2 * a + 1] > reach) reach = iv[2 * a + 1];
                if (reach >= cy1) break;
            }
            if (reach < cy1) gap = 1;
            if (gap) { vis = 1; break; }
        }
        visible[i] = (unsigned char)vis;
    }
    return CULL_OK;
}
