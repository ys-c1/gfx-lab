#include "occ.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OCC_EPS 1e-7f

const char *occ_strerror(int code) {
    switch (code) {
    case OCC_OK:     return "ok";
    case OCC_EARG:   return "bad argument";
    case OCC_EZERO:  return "segment count is zero";
    case OCC_EOOB:   return "segment coverage out of bounds";
    case OCC_EEMPTY: return "segment mask is empty";
    case OCC_EMASK:  return "aggregate mask empty";
    default:         return "unknown error";
    }
}

static int in_grid(float x, float y, int w, int h) {
    return x >= 0.0f && y >= 0.0f && x < (float)w && y < (float)h;
}

/* Mark (x,y) plus the neighbor across a grid line the segment itself
 * lies on: a horizontal segment at integer y covers rows y-1 and y,
 * a vertical segment at integer x covers columns x-1 and x. */
static void mark(unsigned char *plane, int w, int h, int x, int y,
                 int on_vline, int on_hline, long *n) {
    int xo = on_vline ? x - 1 : x;
    int yo = on_hline ? y - 1 : y;
    if (x >= 0 && y >= 0 && x < w && y < h) {
        size_t k = (size_t)y * (size_t)w + (size_t)x;
        if (!plane[k]) { plane[k] = 1; ++*n; }
    }
    if (xo != x && xo >= 0 && y >= 0 && xo < w && y < h) {
        size_t k = (size_t)y * (size_t)w + (size_t)xo;
        if (!plane[k]) { plane[k] = 1; ++*n; }
    }
    if (yo != y && x >= 0 && yo >= 0 && x < w && yo < h) {
        size_t k = (size_t)yo * (size_t)w + (size_t)x;
        if (!plane[k]) { plane[k] = 1; ++*n; }
    }
}

/* Amanatides-Woo grid traversal; every cell the open segment passes
 * through is marked, plus the endpoint cell so closed endpoints count.
 * Returns 0 for a zero-length segment (no traversable interval). */
static int rasterize(unsigned char *plane, const OccSeg *s,
                     int w, int h) {
    float dx = s->x1 - s->x0, dy = s->y1 - s->y0;
    if (fabsf(dx) < OCC_EPS && fabsf(dy) < OCC_EPS) return 0;
    int ix = (int)floorf(s->x0), iy = (int)floorf(s->y0);
    int ex = (int)floorf(s->x1), ey = (int)floorf(s->y1);
    int stepx = dx > OCC_EPS ? 1 : (dx < -OCC_EPS ? -1 : 0);
    int stepy = dy > OCC_EPS ? 1 : (dy < -OCC_EPS ? -1 : 0);
    int on_vline = stepy != 0 && fabsf(s->x0 - (float)(int)s->x0) < OCC_EPS;
    int on_hline = stepx != 0 && fabsf(s->y0 - (float)(int)s->y0) < OCC_EPS;

    long n = 0;
    mark(plane, w, h, ix, iy, on_vline, on_hline, &n);

    float tmaxx = INFINITY, tmaxy = INFINITY;
    float tdeltax = INFINITY, tdeltay = INFINITY;
    if (stepx != 0) {
        float boundary = (float)(ix + (stepx > 0 ? 1 : 0));
        tdeltax = fabsf(1.0f / dx);
        tmaxx = fabsf(boundary - s->x0) * tdeltax;
    }
    if (stepy != 0) {
        float boundary = (float)(iy + (stepy > 0 ? 1 : 0));
        tdeltay = fabsf(1.0f / dy);
        tmaxy = fabsf(boundary - s->y0) * tdeltay;
    }

    /* One step per crossed vertical/horizontal boundary until both
     * axes reach the endpoint cell. An exact corner crossing takes a
     * diagonal step: the two side-neighbor cells only touch the segment
     * at the corner point, so neither is marked. Corner crossings are
     * detected with exact integer cross products on the integer cell
     * grid (floating tmax accumulates drift past epsilon). */
    int sx = ix, sy = iy;
    long adx = (long)llabs((long)ex - (long)sx);
    long ady = (long)llabs((long)ey - (long)sy);
    while (ix != ex || iy != ey) {
        int xopen = ix != ex, yopen = iy != ey;
        int corner = 0;
        if (xopen && yopen) {
            /* Counts of vertical/horizontal grid lines from the start
             * cell to the next boundary; equal tmax -- the segment
             * passes exactly through a corner -- iff bx*ady == by*adx. */
            long bx = (long)llabs((long)ix - (long)sx) + 1;
            long by = (long)llabs((long)iy - (long)sy) + 1;
            corner = bx * ady == by * adx;
        }
        if (corner) {
            ix += stepx;
            iy += stepy;
            tmaxx += tdeltax;
            tmaxy += tdeltay;
        } else if ((xopen ? tmaxx : INFINITY) < (yopen ? tmaxy : INFINITY)) {
            ix += stepx;
            tmaxx += tdeltax;
        } else {
            iy += stepy;
            tmaxy += tdeltay;
        }
        mark(plane, w, h, ix, iy, on_vline, on_hline, &n);
    }
    return (int)n;
}

int occ_compute(OccCov *cov, const OccSeg *segs, int nseg, int w, int h,
                int *bad_seg) {
    if (!cov || (nseg != 0 && !segs) || w <= 0 || h <= 0 || nseg < 0)
        return OCC_EARG;
    memset(cov, 0, sizeof(*cov));
    if (nseg == 0) {
        if (bad_seg) *bad_seg = 0;
        return OCC_EZERO;
    }

    size_t cells = (size_t)w * (size_t)h;
    cov->mask = (unsigned char *)calloc((size_t)nseg * cells, 1);
    cov->rate = (float *)calloc((size_t)nseg, sizeof(float));
    cov->covered = (int *)calloc((size_t)nseg, sizeof(int));
    if (!cov->mask || !cov->rate || !cov->covered) {
        occ_cov_free(cov);
        return OCC_EARG;
    }
    cov->w = w; cov->h = h; cov->nseg = nseg;

    for (int i = 0; i < nseg; ++i) {
        const OccSeg *s = &segs[i];
        if (!in_grid(s->x0, s->y0, w, h) || !in_grid(s->x1, s->y1, w, h)) {
            if (bad_seg) *bad_seg = i;
            occ_cov_free(cov);
            return OCC_EOOB;
        }
        unsigned char *plane = cov->mask + (size_t)i * cells;
        int n = rasterize(plane, s, w, h);
        if (n == 0) {
            if (bad_seg) *bad_seg = i;
            occ_cov_free(cov);
            return OCC_EEMPTY;
        }
        cov->covered[i] = n;
        cov->rate[i] = (float)n / (float)cells;
    }
    return OCC_OK;
}

void occ_cov_free(OccCov *cov) {
    if (!cov) return;
    free(cov->mask);
    free(cov->rate);
    free(cov->covered);
    memset(cov, 0, sizeof(*cov));
}

int occ_aggregate(const OccCov *cov, OccAgg *agg, int *bad_seg) {
    if (!cov || !agg) return OCC_EARG;
    memset(agg, 0, sizeof(*agg));
    if (!cov->mask || cov->nseg <= 0 || cov->w <= 0 || cov->h <= 0) {
        if (bad_seg) *bad_seg = cov->nseg > 0 ? cov->nseg : 0;
        return OCC_EMASK;
    }
    size_t cells = (size_t)cov->w * (size_t)cov->h;
    agg->mask = (unsigned char *)calloc(cells, 1);
    agg->added = (int *)calloc((size_t)cov->nseg, sizeof(int));
    if (!agg->mask || !agg->added) {
        occ_agg_free(agg);
        return OCC_EARG;
    }

    long or_ops = 0, union_cells = 0;
    for (int i = 0; i < cov->nseg; ++i) {
        const unsigned char *plane = cov->mask + (size_t)i * cells;
        for (size_t k = 0; k < cells; ++k) {
            if (!plane[k]) continue;
            ++or_ops;
            if (!agg->mask[k]) {
                agg->mask[k] = 1;
                ++agg->added[i];
                ++union_cells;
            }
        }
    }
    if (union_cells == 0) {
        if (bad_seg) *bad_seg = cov->nseg;
        occ_agg_free(agg);
        return OCC_EMASK;
    }
    agg->or_ops = or_ops;
    agg->union_cells = union_cells;
    return OCC_OK;
}

void occ_agg_free(OccAgg *agg) {
    if (!agg) return;
    free(agg->mask);
    free(agg->added);
    memset(agg, 0, sizeof(*agg));
}

/* Liang-Barsky clipping of segment s against cell (cx, cy); a cell is
 * hit only when the clipped interval has positive length. A segment
 * lying on a grid line passes the parallel test for both adjacent rows
 * or columns; a corner-only contact clips to a point and is rejected. */
static int cell_hit(const OccSeg *s, int cx, int cy) {
    float dx = s->x1 - s->x0, dy = s->y1 - s->y0;
    float p[4] = {-dx, dx, -dy, dy};
    float q[4] = {s->x0 - (float)cx, (float)(cx + 1) - s->x0,
                  s->y0 - (float)cy, (float)(cy + 1) - s->y0};
    float u0 = 0.0f, u1 = 1.0f;
    for (int k = 0; k < 4; ++k) {
        if (fabsf(p[k]) < OCC_EPS) {
            if (q[k] < 0.0f) return 0;       /* parallel and outside */
        } else {
            float r = q[k] / p[k];
            if (p[k] < 0.0f) { if (r > u1) return 0; if (r > u0) u0 = r; }
            else             { if (r < u0) return 0; if (r < u1) u1 = r; }
        }
    }
    return u1 - u0 > OCC_EPS;
}

int occ_reference_mask(unsigned char *out, const OccSeg *segs, int nseg,
                       int w, int h, int *bad_seg) {
    if (!out || (nseg != 0 && !segs) || w <= 0 || h <= 0 || nseg < 0)
        return OCC_EARG;
    if (nseg == 0) {
        if (bad_seg) *bad_seg = 0;
        return OCC_EZERO;
    }
    size_t cells = (size_t)w * (size_t)h;
    memset(out, 0, cells);
    for (int i = 0; i < nseg; ++i) {
        const OccSeg *s = &segs[i];
        if (!in_grid(s->x0, s->y0, w, h) || !in_grid(s->x1, s->y1, w, h)) {
            if (bad_seg) *bad_seg = i;
            return OCC_EOOB;
        }
        int xmin = (int)floorf(fminf(s->x0, s->x1));
        int xmax = (int)floorf(fmaxf(s->x0, s->x1));
        int ymin = (int)floorf(fminf(s->y0, s->y1));
        int ymax = (int)floorf(fmaxf(s->y0, s->y1));
        /* A segment lying on a grid line also covers the cells on its
         * other side; expand the scan box accordingly. */
        int on_vline = fabsf(s->x0 - s->x1) < OCC_EPS &&
                       fabsf(s->x0 - (float)(int)s->x0) < OCC_EPS;
        int on_hline = fabsf(s->y0 - s->y1) < OCC_EPS &&
                       fabsf(s->y0 - (float)(int)s->y0) < OCC_EPS;
        if (on_vline) xmin -= 1;
        if (on_hline) ymin -= 1;
        if (xmin < 0) xmin = 0;
        if (ymin < 0) ymin = 0;
        if (xmax >= w) xmax = w - 1;
        if (ymax >= h) ymax = h - 1;
        int hits = 0;
        for (int cy = ymin; cy <= ymax; ++cy)
            for (int cx = xmin; cx <= xmax; ++cx)
                if (cell_hit(s, cx, cy)) {
                    out[(size_t)cy * w + cx] = 1;
                    ++hits;
                }
        if (hits == 0) {
            if (bad_seg) *bad_seg = i;
            return OCC_EEMPTY;
        }
    }
    return OCC_OK;
}

int occ_write_pgm(const unsigned char *mask, int w, int h, const char *path) {
    if (!mask || !path || w <= 0 || h <= 0) return OCC_EARG;
    FILE *f = fopen(path, "wb");
    if (!f) return OCC_EARG;
    fprintf(f, "P5\n%d %d\n255\n", w, h);
    for (size_t i = 0; i < (size_t)w * (size_t)h; ++i) {
        unsigned char b = mask[i] ? (unsigned char)0 : (unsigned char)255;
        fwrite(&b, 1, 1, f);
    }
    fclose(f);
    return OCC_OK;
}
