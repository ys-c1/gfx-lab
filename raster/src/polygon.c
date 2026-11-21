#define _POSIX_C_SOURCE 199309L
#include "rpf.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* parameter validation: every failure names the offending field      */
/* ------------------------------------------------------------------ */

static char g_err[160];

static int verr(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_err, sizeof(g_err), fmt, ap);
    va_end(ap);
    return -1;
}

const char *rpf_last_error(char *buf, int cap) {
    if (buf && cap > 0) {
        snprintf(buf, (size_t)cap, "%s", g_err);
        return buf;
    }
    return g_err;
}

static int finite_v(const RVert *v) {
    return isfinite(v->x) && isfinite(v->y) && isfinite(v->z);
}

static int validate(const RTarget *t, const RPoly *p, const RPFillOpts *o) {
    g_err[0] = '\0';
    if (!t) return verr("field 'target' is NULL");
    if (t->w <= 0 || t->h <= 0 || !t->z || !t->rgb)
        return verr("field 'target' is not initialized (w/h/buffer invalid)");
    if (!p) return verr("field 'poly' is NULL");
    if (!p->verts) return verr("field 'poly.verts' is NULL");
    if (p->n < 3) return verr("field 'poly.n' out of range: %d (min 3)", p->n);
    if (p->n > RPF_MAX_VERTS)
        return verr("field 'poly.n' out of range: %d (max %d)", p->n, RPF_MAX_VERTS);
    for (int i = 0; i < p->n; ++i)
        if (!finite_v(&p->verts[i]))
            return verr("field 'poly.verts[%d]' contains non-finite coordinate", i);
    if (!o) return verr("field 'opts' is NULL");
    if (o->rule != RPF_RULE_EVEN_ODD && o->rule != RPF_RULE_NONZERO)
        return verr("field 'opts.rule' out of range: %d (0=even-odd,1=nonzero)", o->rule);
    if (o->mode != RPF_MODE_OFF && o->mode != RPF_MODE_ENABLED)
        return verr("field 'opts.mode' out of range: %d (0=off,1=on)", o->mode);
    if (!isfinite(o->z)) return verr("field 'opts.z' is non-finite");
    if (!o->eo_color) return verr("field 'opts.eo_color' is NULL");
    if (!o->pos_color) return verr("field 'opts.pos_color' is NULL");
    if (!o->neg_color) return verr("field 'opts.neg_color' is NULL");
    return 0;
}

void rpf_stats_free(RPFStats *st) {
    if (!st) return;
    free(st->cover);
    st->cover = NULL;
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec * 1e-6;
}

static double edge_fn(const RVert *a, const RVert *b, double px, double py) {
    return (b->x - a->x) * (py - a->y) - (b->y - a->y) * (px - a->x);
}

/*
 * Analytic containment at one sample point, two standard half-open rules
 * (both valid for self-intersecting polygons, where the signed-fan winding
 * identity fails). The fan triangulation is used only for triangle ids.
 *
 *  crossings : even-odd ray casting to +x. An edge spans the ray's y under
 *              a.y <= py < b.y (up) or b.y <= py < a.y (down), so a vertex
 *              shared by two edges is counted exactly once and horizontal
 *              edges never span. Instead of computing the intersection (a
 *              division that double-counts collinear vertical edges at a
 *              vertex), the isLeft sign gives the same side test directly.
 *
 *  winding   : Sunday's winding-number rule with the same strict isLeft
 *              sign tests; a point on an edge gives isleft == 0 and is
 *              counted by neither branch.
 */
static void ray_point(const RPoly *poly, double px, double py,
                      int *winding, int *crossings) {
    int wn = 0, cr = 0;
    for (int k = 0; k < poly->n; ++k) {
        const RVert *a = &poly->verts[k];
        const RVert *b = &poly->verts[(k + 1) % poly->n];
        double isleft = (b->x - a->x) * (py - a->y)
                      - (b->y - a->y) * (px - a->x);

        if (a->y <= py) {
            if (b->y > py) {                              /* upward edge */
                if (isleft > 0) { ++wn; ++cr; }           /* P strictly left */
            }
        } else {
            if (b->y <= py) {                             /* downward edge */
                if (isleft < 0) { --wn; ++cr; }           /* P strictly right */
            }
        }
    }
    *winding = wn;
    *crossings = cr;
}

/* ------------------------------------------------------------------ */
/* phase 1: coverage. Signed fan triangles -> parity + winding + ids  */
/* ------------------------------------------------------------------ */

int rpf_polygon(RTarget *t, const RPoly *poly, const RPFillOpts *opts,
                RPFStats *stats) {
    if (validate(t, poly, opts) != 0) return -1;

    const int w = t->w, h = t->h, n = poly->n;
    RPFCover *cover = (RPFCover*)calloc((size_t)w * (size_t)h, sizeof(*cover));
    if (!cover) { verr("field 'target': out of memory for coverage buffer"); return -1; }

    double t0 = now_ms();
    int ntris = 0;
    int tri_cover[RPF_MAX_TRIS];
    memset(tri_cover, 0, sizeof(tri_cover));

    for (int k = 0; k < n - 2; ++k) {
        const RVert *a = &poly->verts[0];
        const RVert *b = &poly->verts[k + 1];
        const RVert *c = &poly->verts[k + 2];
        double area = edge_fn(a, b, c->x, c->y);
        if (fabs(area) < 1e-12) continue;          /* degenerate fan triangle */
        int sgn = area > 0 ? 1 : -1;
        int tid = ntris;
        ntris++;

        int minx = (int)floor(fmin(fmin(a->x, b->x), c->x));
        int maxx = (int)ceil(fmax(fmax(a->x, b->x), c->x));
        int miny = (int)floor(fmin(fmin(a->y, b->y), c->y));
        int maxy = (int)ceil(fmax(fmax(a->y, b->y), c->y));
        if (minx < 0) minx = 0;
        if (miny < 0) miny = 0;
        if (maxx >= w) maxx = w - 1;
        if (maxy >= h) maxy = h - 1;

        for (int y = miny; y <= maxy; ++y) {
            for (int x = minx; x <= maxx; ++x) {
                double px = x + 0.5, py = y + 0.5;
                double e0 = edge_fn(b, c, px, py);
                double e1 = edge_fn(c, a, px, py);
                double e2 = edge_fn(a, b, px, py);
                int inside = sgn > 0 ? (e0 >= 0 && e1 >= 0 && e2 >= 0)
                                     : (e0 <= 0 && e1 <= 0 && e2 <= 0);
                if (!inside) continue;
                size_t i = (size_t)y * (size_t)w + (size_t)x;
                RPFCover *cv = &cover[i];
                if (cv->ntris < RPF_MAX_TRIS) {
                    cv->tri_id[cv->ntris] = (unsigned char)tid;
                } else {
                    cv->overflow = 1;
                }
                cv->ntris++;
                tri_cover[tid]++;
            }
        }
    }

    /* phase 1b: analytic containment at every pixel centre. A horizontal
     * ray gives the geometrically correct crossing count (parity) and signed
     * winding number for ANY polygon, including self-intersecting ones,
     * where fan-triangle overlap is a topological artifact rather than real
     * double coverage. The fan pass above only supplies triangle ids. */
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int wn = 0, cr = 0;
            ray_point(poly, x + 0.5, y + 0.5, &wn, &cr);
            size_t i = (size_t)y * (size_t)w + (size_t)x;
            cover[i].parity = (unsigned char)cr;
            cover[i].winding = (signed char)wn;
        }
    }
    double t1 = now_ms();

    int covered = 0, eo_fill = 0, nz_fill = 0, overflow = 0;
    for (size_t i = 0; i < (size_t)w * (size_t)h; ++i) {
        RPFCover *cv = &cover[i];
        int eo_in = cv->parity & 1;
        int nz_in = cv->winding != 0;
        if (eo_in || nz_in) covered++;   /* union of the two rules' interiors */
        if (cv->overflow) overflow++;
        if (eo_in) eo_fill++;
        if (nz_in) nz_fill++;
    }

    /* phase 2: shading, driven strictly by the phase-1 record + rule table */
    int fill = 0;
    if (opts->mode == RPF_MODE_ENABLED) {
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                size_t i = (size_t)y * (size_t)w + (size_t)x;
                RPFCover *cv = &cover[i];
                int inside = opts->rule == RPF_RULE_EVEN_ODD
                                 ? (cv->parity & 1)
                                 : (cv->winding != 0);
                if (!inside) continue;
                const unsigned char *col;
                if (opts->rule == RPF_RULE_EVEN_ODD) {
                    col = opts->eo_color;
                } else {
                    col = cv->winding > 0 ? opts->pos_color : opts->neg_color;
                }
                if ((float)opts->z < t->z[i]) {
                    t->z[i] = (float)opts->z;
                    t->rgb[i * 3 + 0] = col[0];
                    t->rgb[i * 3 + 1] = col[1];
                    t->rgb[i * 3 + 2] = col[2];
                    ++fill;
                }
            }
        }
    }
    double t2 = now_ms();

    if (stats) {
        memset(stats, 0, sizeof(*stats));
        stats->grid_w = w;
        stats->grid_h = h;
        stats->cover = cover;
        stats->ntris = ntris;
        stats->covered_pixels = covered;
        stats->eo_fill_pixels = eo_fill;
        stats->nz_fill_pixels = nz_fill;
        stats->overflow_pixels = overflow;
        for (int k = 0; k < ntris; ++k) stats->tri_cover[k] = tri_cover[k];
        stats->phase1_ms = t1 - t0;
        stats->phase2_ms = t2 - t1;
        stats->total_ms = t2 - t0;
        /* distinct inside (parity, winding) states = the rule table rows */
        int nstates = 0;
        unsigned char seen[128];
        memset(seen, 0, sizeof(seen));
        for (size_t i = 0; i < (size_t)w * (size_t)h; ++i) {
            RPFCover *cv = &cover[i];
            if (!(cv->parity & 1) && cv->winding == 0) continue;
            int key = (cv->parity << 3) + (cv->winding + 8);
            if (key < 0 || key >= 128 || seen[key]) continue;
            seen[key] = 1;
            nstates++;
        }
        stats->nstates = nstates;
    } else {
        free(cover);
    }
    return fill;
}

/* ------------------------------------------------------------------ */
/* queries                                                            */
/* ------------------------------------------------------------------ */

int rpf_pixel_get(const RTarget *t, const RPFStats *st, int x, int y,
                  RPFCover *out) {
    g_err[0] = '\0';
    if (!t) return verr("field 'target' is NULL");
    if (!st || !st->cover) return verr("field 'stats' is NULL or has no coverage buffer");
    if (!out) return verr("field 'out' is NULL");
    if (x < 0 || x >= st->grid_w)
        return verr("field 'x' out of range: %d (grid width %d)", x, st->grid_w);
    if (y < 0 || y >= st->grid_h)
        return verr("field 'y' out of range: %d (grid height %d)", y, st->grid_h);
    *out = st->cover[(size_t)y * (size_t)st->grid_w + (size_t)x];
    return 0;
}

int rpf_rule_table(const RTarget *t, const RPFStats *st,
                   const RPFillOpts *opts, RPFState *out, int maxn) {
    g_err[0] = '\0';
    if (!t) return verr("field 'target' is NULL");
    if (!st || !st->cover) return verr("field 'stats' is NULL or has no coverage buffer");
    if (!opts) return verr("field 'opts' is NULL");
    if (!out) return verr("field 'out' is NULL");
    if (maxn <= 0) return verr("field 'maxn' out of range: %d (min 1)", maxn);

    RPFState tmp[RPF_MAX_STATES];
    int nst = 0;
    for (size_t i = 0; i < (size_t)st->grid_w * (size_t)st->grid_h; ++i) {
        RPFCover *cv = &st->cover[i];
        if (!(cv->parity & 1) && cv->winding == 0) continue;
        int found = -1;
        for (int k = 0; k < nst; ++k)
            if (tmp[k].parity == cv->parity && tmp[k].winding == cv->winding)
                found = k;
        if (found < 0) {
            if (nst >= RPF_MAX_STATES) { verr("rule table exceeds RPF_MAX_STATES"); return -1; }
            found = nst++;
            memset(&tmp[found], 0, sizeof(tmp[found]));
            tmp[found].parity = cv->parity;
            tmp[found].winding = cv->winding;
            tmp[found].eo_inside = cv->parity & 1;
            tmp[found].nz_inside = cv->winding != 0;
            tmp[found].selected = opts->rule == RPF_RULE_EVEN_ODD
                                      ? tmp[found].eo_inside
                                      : tmp[found].nz_inside;
            const unsigned char *col;
            if (opts->rule == RPF_RULE_EVEN_ODD) {
                col = opts->eo_color;
            } else {
                col = cv->winding > 0 ? opts->pos_color : opts->neg_color;
            }
            if (tmp[found].selected)
                memcpy(tmp[found].rgb, col, 3);
        }
        tmp[found].count++;
    }
    if (nst > maxn) return verr("field 'maxn' too small: need %d, got %d", nst, maxn);
    memcpy(out, tmp, (size_t)nst * sizeof(RPFState));
    return nst;
}

/* ------------------------------------------------------------------ */
/* debug maps / report                                                */
/* ------------------------------------------------------------------ */

static int write_pgm(const char *path, int w, int h, const unsigned char *g) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "P5\n%d %d\n255\n", w, h);
    if (fwrite(g, 1, (size_t)w * (size_t)h, f) != (size_t)w * (size_t)h) {
        fclose(f);
        return -2;
    }
    fclose(f);
    return 0;
}

int rpf_write_parity_pgm(const RTarget *t, const RPFStats *st, const char *path) {
    g_err[0] = '\0';
    if (!st || !st->cover) return verr("field 'stats' is NULL or has no coverage buffer");
    if (!path) return verr("field 'path' is NULL");
    int n = st->grid_w * st->grid_h;
    unsigned char *g = (unsigned char*)malloc((size_t)n);
    if (!g) return verr("out of memory");
    for (int i = 0; i < n; ++i)
        g[i] = (st->cover[i].parity & 1) ? 255 : 0;
    int rc = write_pgm(path, st->grid_w, st->grid_h, g);
    free(g);
    (void)t;
    return rc;
}

int rpf_write_winding_pgm(const RTarget *t, const RPFStats *st, const char *path) {
    g_err[0] = '\0';
    if (!st || !st->cover) return verr("field 'stats' is NULL or has no coverage buffer");
    if (!path) return verr("field 'path' is NULL");
    int n = st->grid_w * st->grid_h;
    unsigned char *g = (unsigned char*)malloc((size_t)n);
    if (!g) return verr("out of memory");
    /* +winding -> bright (255-|w|), -winding -> dark (|w|), zero -> 128 */
    for (int i = 0; i < n; ++i) {
        signed char wd = st->cover[i].winding;
        g[i] = wd > 0 ? (unsigned char)(255 - wd)
             : wd < 0 ? (unsigned char)(-wd) : 128;
    }
    int rc = write_pgm(path, st->grid_w, st->grid_h, g);
    free(g);
    (void)t;
    return rc;
}

int rpf_write_report(const RTarget *t, const RPFStats *st,
                     const RPoly *poly, const RPFillOpts *opts, const char *path) {
    g_err[0] = '\0';
    if (!st || !st->cover) return verr("field 'stats' is NULL or has no coverage buffer");
    if (!poly) return verr("field 'poly' is NULL");
    if (!opts) return verr("field 'opts' is NULL");
    if (!path) return verr("field 'path' is NULL");
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "raster polygon fill report\n");
    fprintf(f, "grid: %d x %d\n", st->grid_w, st->grid_h);
    fprintf(f, "verts: %d  fan triangles: %d\n", poly->n, st->ntris);
    fprintf(f, "mode: %s  rule: %s  z: %.3f\n",
            opts->mode == RPF_MODE_OFF ? "OFF" : "ON",
            opts->rule == RPF_RULE_EVEN_ODD ? "even-odd" : "nonzero", opts->z);
    fprintf(f, "phase1 coverage ms: %.4f\n", st->phase1_ms);
    fprintf(f, "phase2 shading  ms: %.4f\n", st->phase2_ms);
    fprintf(f, "total           ms: %.4f\n", st->total_ms);
    fprintf(f, "covered pixels (EO union NZ): %d\n", st->covered_pixels);
    fprintf(f, "even-odd fill pixels     : %d\n", st->eo_fill_pixels);
    fprintf(f, "nonzero fill pixels      : %d\n", st->nz_fill_pixels);
    fprintf(f, "overflow pixels          : %d\n", st->overflow_pixels);
    fprintf(f, "\nper fan-triangle coverage:\n");
    for (int k = 0; k < st->ntris; ++k)
        fprintf(f, "  tri %2d (verts 0,%d,%d): %d px\n",
                k, k + 1, k + 2, st->tri_cover[k]);
    RPFState rows[RPF_MAX_STATES];
    int nst = rpf_rule_table(t, st, opts, rows, RPF_MAX_STATES);
    fprintf(f, "\nrule table (parity, winding) -> [even-odd, nonzero, selected] color count:\n");
    for (int k = 0; k < nst; ++k) {
        RPFState *r = &rows[k];
        fprintf(f, "  parity=%u winding=%d  EO=%d NZ=%d sel=%d rgb=(%u,%u,%u) count=%d\n",
                r->parity, (int)r->winding, r->eo_inside, r->nz_inside, r->selected,
                r->rgb[0], r->rgb[1], r->rgb[2], r->count);
    }
    fclose(f);
    return 0;
}
