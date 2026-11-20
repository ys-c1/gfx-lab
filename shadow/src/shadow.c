#include "shadow.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ util */

static int fail(ShError *err, int code, int index, const char *msg) {
    if (err) { err->code = code; err->index = index; err->msg = msg; }
    return code;
}

static int fin(float v) { return isfinite(v) != 0; }

static int seg_finite(const ShSeg *s) {
    return fin(s->x0) && fin(s->y0) && fin(s->x1) && fin(s->y1);
}

/* --------------------------------------------------------------- basics */

int sh_init(ShadowMap *m, int w, int h) {
    if (!m || w <= 0 || h <= 0) return SH_E_ARG;
    m->buf = (float*)calloc((size_t)w * (size_t)h, sizeof(float));
    if (!m->buf) return SH_E_NOMEM;
    m->w = w; m->h = h;
    return SH_OK;
}

void sh_free(ShadowMap *m) {
    if (m) { free(m->buf); m->buf = NULL; m->w = 0; m->h = 0; }
}

int sh_clear(ShadowMap *m) {
    if (!m || !m->buf) return SH_E_ARG;
    for (int i = 0; i < m->w * m->h; ++i) m->buf[i] = 0.0f;
    return SH_OK;
}

/* -------------------------------------------------------------- frustum */

/* Sign of the turn o->a->b: > 0 means b lies to the left of o->a. */
static double cross3(double ox, double oy, double ax, double ay,
                     double bx, double by) {
    return (ax - ox) * (by - oy) - (ay - oy) * (bx - ox);
}

/* Squared distance from p to segment ab. */
static double dist2_seg(double px, double py,
                        double ax, double ay, double bx, double by) {
    double dx = bx - ax, dy = by - ay;
    double l2 = dx * dx + dy * dy;
    double t = 0.0;
    if (l2 > 0.0) {
        t = ((px - ax) * dx + (py - ay) * dy) / l2;
        if (t < 0.0) t = 0.0;
        if (t > 1.0) t = 1.0;
    }
    double qx = ax + t * dx - px, qy = ay + t * dy - py;
    return qx * qx + qy * qy;
}

static int cmp_pt(const void *pa, const void *pb) {
    const ShPoint *a = (const ShPoint*)pa, *b = (const ShPoint*)pb;
    if (a->x < b->x) return -1;
    if (a->x > b->x) return 1;
    if (a->y < b->y) return -1;
    if (a->y > b->y) return 1;
    return 0;
}

/* Monotone-chain convex hull, CCW.  Returns the new count, 0 if degenerate. */
static int convex_hull(ShPoint *p, int n) {
    if (n < 3) return 0;
    qsort(p, (size_t)n, sizeof(*p), cmp_pt);

    int k = 0;                       /* drop exact duplicates */
    for (int i = 0; i < n; ++i)
        if (k == 0 || p[i].x != p[k - 1].x || p[i].y != p[k - 1].y) p[k++] = p[i];
    n = k;
    if (n < 3) return 0;

    ShPoint hull[2 * SH_FRUSTUM_MAX];
    int h = 0;
    for (int i = 0; i < n; ++i) {
        while (h >= 2 &&
               cross3(hull[h-2].x, hull[h-2].y, hull[h-1].x, hull[h-1].y,
                      p[i].x, p[i].y) <= 0.0) --h;
        hull[h++] = p[i];
    }
    for (int i = n - 2, lower = h + 1; i >= 0; --i) {
        while (h >= lower &&
               cross3(hull[h-2].x, hull[h-2].y, hull[h-1].x, hull[h-1].y,
                      p[i].x, p[i].y) <= 0.0) --h;
        hull[h++] = p[i];
    }
    --h;                             /* closing point repeats the first */
    if (h < 3 || h > SH_FRUSTUM_MAX) return 0;
    for (int i = 0; i < h; ++i) p[i] = hull[i];
    return h;
}

/* Validate a user-supplied frustum; index identifies the offending vertex. */
static int frustum_check(const ShFrustum *f, ShError *err) {
    if (f->n < 3 || f->n > SH_FRUSTUM_MAX)
        return fail(err, SH_E_EMPTY_FRUSTUM, -1, "frustum needs 3..8 vertices");
    double area2 = 0.0;
    for (int i = 0; i < f->n; ++i) {
        if (!fin(f->v[i].x) || !fin(f->v[i].y))
            return fail(err, SH_E_NONFINITE, i, "frustum vertex is not finite");
        int j = (i + 1) % f->n;
        area2 += (double)f->v[i].x * f->v[j].y - (double)f->v[j].x * f->v[i].y;
    }
    if (fabs(area2) < 1e-12)
        return fail(err, SH_E_EMPTY_FRUSTUM, -1, "frustum has zero area");
    /* Every test below assumes convexity, so reject a reflex vertex. */
    {
        int sign = area2 > 0.0 ? 1 : -1;
        for (int i = 0; i < f->n; ++i) {
            int j = (i + 1) % f->n, k = (i + 2) % f->n;
            double c = cross3(f->v[i].x, f->v[i].y, f->v[j].x, f->v[j].y,
                              f->v[k].x, f->v[k].y) * sign;
            if (c < -1e-9)
                return fail(err, SH_E_EMPTY_FRUSTUM, j,
                            "frustum is not convex");
        }
    }
    return SH_OK;
}

int sh_frustum_from_light(const ShadowMap *m, const ShLight *light,
                          ShFrustum *out, ShError *err) {
    if (!m || !light || !out || !m->buf)
        return fail(err, SH_E_ARG, -1, "null shadow map, light or output");
    if (!fin(light->x) || !fin(light->y))
        return fail(err, SH_E_NONFINITE, -1, "light position is not finite");

    ShPoint p[1 + 4];
    p[0].x = light->x;   p[0].y = light->y;
    p[1].x = 0.0f;       p[1].y = 0.0f;
    p[2].x = (float)m->w; p[2].y = 0.0f;
    p[3].x = (float)m->w; p[3].y = (float)m->h;
    p[4].x = 0.0f;       p[4].y = (float)m->h;

    int n = convex_hull(p, 5);
    if (n < 3)
        return fail(err, SH_E_EMPTY_FRUSTUM, -1,
                    "light and map corners collapse to a degenerate hull");
    out->n = n;
    for (int i = 0; i < n; ++i) out->v[i] = p[i];
    if (err) { err->code = SH_OK; err->index = -1; err->msg = "ok"; }
    return SH_OK;
}

/* Inclusive point-in-convex-polygon test (either winding). */
static int point_in_convex(const ShFrustum *f, double px, double py) {
    int sign = 0;
    for (int i = 0; i < f->n; ++i) {
        int j = (i + 1) % f->n;
        double c = cross3(f->v[i].x, f->v[i].y, f->v[j].x, f->v[j].y, px, py);
        if (c > 1e-9)      { if (sign < 0) return 0; sign = 1;  }
        else if (c < -1e-9) { if (sign > 0) return 0; sign = -1; }
    }
    return 1;
}

/* Does the segment touch the convex frustum at all (endpoint inside or
 * crossing an edge)?  Conservative: true means "keep". */
static int seg_touches_convex(const ShFrustum *f, const ShSeg *s) {
    if (point_in_convex(f, s->x0, s->y0) || point_in_convex(f, s->x1, s->y1))
        return 1;
    double ax = s->x0, ay = s->y0, bx = s->x1, by = s->y1;
    if (ax == bx && ay == by) return 0;
    for (int i = 0; i < f->n; ++i) {
        int j = (i + 1) % f->n;
        double cx = f->v[i].x, cy = f->v[i].y;
        double dx = f->v[j].x, dy = f->v[j].y;
        double d1 = cross3(ax, ay, bx, by, cx, cy);
        double d2 = cross3(ax, ay, bx, by, dx, dy);
        double d3 = cross3(cx, cy, dx, dy, ax, ay);
        double d4 = cross3(cx, cy, dx, dy, bx, by);
        if (((d1 > 0.0 && d2 < 0.0) || (d1 < 0.0 && d2 > 0.0)) &&
            ((d3 > 0.0 && d4 < 0.0) || (d3 < 0.0 && d4 > 0.0)))
            return 1;
        /* Collinear touch counts as keeping the segment. */
        if (d1 == 0.0 && dist2_seg(cx, cy, ax, ay, bx, by) < 1e-18) return 1;
        if (d2 == 0.0 && dist2_seg(dx, dy, ax, ay, bx, by) < 1e-18) return 1;
        if (d3 == 0.0 && dist2_seg(ax, ay, cx, cy, dx, dy) < 1e-18) return 1;
        if (d4 == 0.0 && dist2_seg(bx, by, cx, cy, dx, dy) < 1e-18) return 1;
    }
    return 0;
}

int sh_cull_segments(const ShFrustum *f, const ShSeg *segs, int nseg,
                     int *cand, int cand_cap, ShCullStats *stats, ShError *err) {
    if (!f || !segs)
        return fail(err, SH_E_ARG, -1, "null frustum or segment array");
    if (nseg <= 0)
        return fail(err, SH_E_NO_SEGMENTS, -1, "segment count must be positive");
    if (cand_cap > 0 && !cand)
        return fail(err, SH_E_ARG, -1, "null candidate table");

    int rc = frustum_check(f, err);
    if (rc != SH_OK) return rc;

    if (stats) { stats->input = nseg; stats->culled = 0; stats->candidates = 0; }

    int ncand = 0;
    for (int i = 0; i < nseg; ++i) {
        if (!seg_finite(&segs[i]))
            return fail(err, SH_E_NONFINITE, i,
                        "segment coordinate is not finite");
        if (!seg_touches_convex(f, &segs[i])) continue;
        if (ncand >= cand_cap)
            return fail(err, SH_E_CAPACITY, i, "candidate table is full");
        cand[ncand++] = i;
    }
    if (stats) { stats->candidates = ncand; stats->culled = nseg - ncand; }
    if (err) { err->code = SH_OK; err->index = -1; err->msg = "ok"; }
    return SH_OK;
}

/* ------------------------------------------------------------- rendering */

static int blocked(const ShSeg *s, float lx, float ly, float px, float py) {
    float rx = px - lx, ry = py - ly;
    float sx = s->x1 - s->x0, sy = s->y1 - s->y0;
    float den = rx * sy - ry * sx;
    if (fabsf(den) < 1e-9f) return 0;
    float t = ((s->x0 - lx) * sy - (s->y0 - ly) * sx) / den;
    float u = ((s->x0 - lx) * ry - (s->y0 - ly) * rx) / den;
    return t > 0.0f && t < 1.0f && u >= 0.0f && u <= 1.0f;
}

/* Shared pixel loop.  cand == NULL means "test all nseg in original order". */
static void render_impl(ShadowMap *m, const ShLight *light, const ShSeg *segs,
                        int nseg, const int *cand, int ncand) {
    int count = cand ? ncand : nseg;
    for (int y = 0; y < m->h; ++y) {
        for (int x = 0; x < m->w; ++x) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            int shadowed = 0;
            for (int k = 0; k < count; ++k) {
                int i = cand ? cand[k] : k;
                if (blocked(&segs[i], light->x, light->y, px, py)) {
                    shadowed = 1;
                    break;
                }
            }
            float v = 0.0f;
            if (!shadowed) {
                float dx = px - light->x, dy = py - light->y;
                float d = sqrtf(dx * dx + dy * dy);
                float fall = 1.0f / (1.0f + d / light->radius);
                v = light->intensity * fall;
            }
            m->buf[(size_t)y * m->w + x] = v;
        }
    }
}

int sh_render_candidates(ShadowMap *m, const ShLight *light,
                         const ShSeg *segs, const int *cand, int ncand) {
    if (!m || !light || !m->buf) return SH_E_ARG;
    if (ncand < 0) return SH_E_ARG;
    if (ncand > 0 && (!segs || !cand)) return SH_E_ARG;
    if (light->radius <= 0.0f || light->intensity < 0.0f) return SH_E_LIGHT;
    if (!fin(light->x) || !fin(light->y)) return SH_E_NONFINITE;
    render_impl(m, light, segs, 0, cand, ncand);
    return SH_OK;
}

/* Map corners in the order (0,0), (w,0), (w,h), (0,h). */
static void map_corner(const ShadowMap *m, int i, ShPoint *p) {
    p->x = (i == 0 || i == 3) ? 0.0f : (float)m->w;
    p->y = (i < 2) ? 0.0f : (float)m->h;
}

/* Culling is loss-free only if the frustum contains the light and every map
 * pixel.  Both are corners of the hull we build, so test those five points. */
static int frustum_covers(const ShFrustum *f, const ShadowMap *m,
                          const ShLight *l, int *bad) {
    if (!point_in_convex(f, l->x, l->y)) { if (bad) *bad = -1; return 0; }
    for (int i = 0; i < 4; ++i) {
        ShPoint c;
        map_corner(m, i, &c);
        if (!point_in_convex(f, c.x, c.y)) { if (bad) *bad = i; return 0; }
    }
    return 1;
}

int sh_render_ex(ShadowMap *m, const ShLight *light, const ShSeg *segs, int nseg,
                 const ShCullOpts *opts, ShCullStats *stats, ShError *err) {
    if (!m || !light || !m->buf)
        return fail(err, SH_E_ARG, -1, "null shadow map or light");
    if (light->radius <= 0.0f || light->intensity < 0.0f)
        return fail(err, SH_E_LIGHT, -1,
                    "light radius must be > 0 and intensity >= 0");
    if (!fin(light->x) || !fin(light->y))
        return fail(err, SH_E_NONFINITE, -1, "light position is not finite");
    if (nseg <= 0 || !segs)
        return fail(err, SH_E_NO_SEGMENTS, -1, "segment count must be positive");

    int cull = opts && opts->enabled != 0;
    if (stats) { stats->input = nseg; stats->culled = 0; stats->candidates = nseg; }

    if (!cull) {
        /* Legacy path: every segment in original order.  Validate first so the
         * reported index is deterministic. */
        for (int i = 0; i < nseg; ++i)
            if (!seg_finite(&segs[i]))
                return fail(err, SH_E_NONFINITE, i,
                            "segment coordinate is not finite");
        render_impl(m, light, segs, nseg, NULL, 0);
        if (err) { err->code = SH_OK; err->index = -1; err->msg = "ok"; }
        return SH_OK;
    }

    int rc = frustum_check(&opts->frustum, err);
    if (rc != SH_OK) return rc;
    {
        int bad = -1;
        if (!frustum_covers(&opts->frustum, m, light, &bad))
            return fail(err, SH_E_FRUSTUM_COVER, bad,
                        "frustum does not contain the light and the whole map");
    }

    /* Layer 1: build the candidate table once. */
    int *cand = (int*)malloc(sizeof(int) * (size_t)nseg);
    if (!cand) return fail(err, SH_E_NOMEM, -1, "candidate allocation failed");
    ShCullStats local;
    rc = sh_cull_segments(&opts->frustum, segs, nseg, cand, nseg, &local, err);
    if (rc != SH_OK) { free(cand); return rc; }
    if (stats) *stats = local;

    /* Layer 2: test only candidates, in original order. */
    render_impl(m, light, segs, 0, cand, local.candidates);
    free(cand);
    if (err) { err->code = SH_OK; err->index = -1; err->msg = "ok"; }
    return SH_OK;
}

int sh_render(ShadowMap *m, const ShLight *light, const ShSeg *segs, int nseg) {
    return sh_render_ex(m, light, segs, nseg, NULL, NULL, NULL);
}

int sh_write_pgm(const ShadowMap *m, const char *path) {
    if (!m || !m->buf || !path) return SH_E_ARG;
    FILE *f = fopen(path, "wb");
    if (!f) return SH_E_IO;
    fprintf(f, "P5\n%d %d\n255\n", m->w, m->h);
    for (int i = 0; i < m->w * m->h; ++i) {
        float v = m->buf[i];
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        unsigned char b = (unsigned char)(v * 255.0f + 0.5f);
        fwrite(&b, 1, 1, f);
    }
    fclose(f);
    return SH_OK;
}
