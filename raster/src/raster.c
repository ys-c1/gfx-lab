#include "raster.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

int rt_init(RTarget *t, int w, int h) {
    if (!t || w <= 0 || h <= 0) return -1;
    t->w = w; t->h = h;
    t->z = (float*)malloc(sizeof(float) * (size_t)w * (size_t)h);
    t->rgb = (unsigned char*)malloc((size_t)w * (size_t)h * 3);
    if (!t->z || !t->rgb) { rt_free(t); return -1; }
    for (int i = 0; i < w * h; ++i) { t->z[i] = 1e30f; t->rgb[i*3+0] = 0; t->rgb[i*3+1] = 0; t->rgb[i*3+2] = 0; }
    return 0;
}

void rt_free(RTarget *t) {
    if (!t) return;
    free(t->z); free(t->rgb);
    t->z = NULL; t->rgb = NULL; t->w = 0; t->h = 0;
}

static double edge(const RVert *a, const RVert *b, double px, double py) {
    return (b->x - a->x) * (py - a->y) - (b->y - a->y) * (px - a->x);
}

/* Scan-convert exactly as the original implementation did. Vertex values are
 * consumed as given, so an unsnapped triangle keeps the legacy result. */
static int scan_triangle(RTarget *t, const RTri *tri) {
    double area = edge(&tri->v[0], &tri->v[1], tri->v[2].x, tri->v[2].y);
    if (fabs(area) < 1e-12) return RT_E_DEGEN;
    int minx = (int)floor(fmin(fmin(tri->v[0].x, tri->v[1].x), tri->v[2].x));
    int maxx = (int)ceil(fmax(fmax(tri->v[0].x, tri->v[1].x), tri->v[2].x));
    int miny = (int)floor(fmin(fmin(tri->v[0].y, tri->v[1].y), tri->v[2].y));
    int maxy = (int)ceil(fmax(fmax(tri->v[0].y, tri->v[1].y), tri->v[2].y));
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx >= t->w) maxx = t->w - 1;
    if (maxy >= t->h) maxy = t->h - 1;
    int written = 0;
    for (int y = miny; y <= maxy; ++y) {
        for (int x = minx; x <= maxx; ++x) {
            double px = x + 0.5, py = y + 0.5;
            double w0 = edge(&tri->v[1], &tri->v[2], px, py);
            double w1 = edge(&tri->v[2], &tri->v[0], px, py);
            double w2 = edge(&tri->v[0], &tri->v[1], px, py);
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            double sum = w0 + w1 + w2;
            double z = (w0 * tri->v[0].z + w1 * tri->v[1].z + w2 * tri->v[2].z) / sum;
            size_t i = (size_t)y * (size_t)t->w + (size_t)x;
            if ((float)z < t->z[i]) {
                t->z[i] = (float)z;
                t->rgb[i*3+0] = tri->rgb[0];
                t->rgb[i*3+1] = tri->rgb[1];
                t->rgb[i*3+2] = tri->rgb[2];
                ++written;
            }
        }
    }
    return written;
}

int rt_triangle(RTarget *t, const RTri *tri) {
    if (!t || !tri) return -1;
    return scan_triangle(t, tri);
}

static void report_clear(RTAlignReport *r) {
    if (!r) return;
    r->code = RT_OK;
    r->vertex = -1;
    r->dx[0] = r->dx[1] = r->dx[2] = 0.0;
    r->dy[0] = r->dy[1] = r->dy[2] = 0.0;
    r->max_displacement = 0.0;
    r->collapsed = 0;
    r->msg[0] = '\0';
}

static int report_vertex_error(RTAlignReport *r, int code, int vertex, const char *what) {
    static const char label[3] = { 'A', 'B', 'C' };
    char tag = (vertex >= 0 && vertex < 3) ? label[vertex] : '?';
    if (r) {
        r->code = code;
        r->vertex = vertex;
        snprintf(r->msg, sizeof(r->msg), "vertex %d (%c): %s", vertex, tag, what);
    }
    return code;
}

static int report_error(RTAlignReport *r, int code, const char *what) {
    if (r) {
        r->code = code;
        r->vertex = -1;
        snprintf(r->msg, sizeof(r->msg), "%s", what);
    }
    return code;
}

/* Post-snap collapse: the vertices landed on one lattice line, so the
 * triangle covers no pixel. Not an error; the report says why. */
static int report_collapsed(RTAlignReport *r) {
    if (r) {
        r->collapsed = 1;
        r->vertex = -1;
        snprintf(r->msg, sizeof(r->msg), "no area after snapping: vertices share one lattice line");
    }
    return RT_OK;
}

int rt_triangle_aligned(RTarget *t, const RTri *tri, const RTAlign *align, RTAlignReport *report) {
    if (!t) return report_error(report, RT_E_ARG, "null target");
    RTAlignReport local;
    RTAlignReport *rep = report ? report : &local;
    RTri snapped;
    int rc = rt_align_vertices(tri, align, &snapped, rep);
    if (rc != RT_OK) return rc;
    if (!align || !align->enabled) return scan_triangle(t, tri);
    if (rep->collapsed) return 0;   /* snapped sliver covers no pixel */
    return scan_triangle(t, &snapped);
}

int rt_align_vertices(const RTri *tri, const RTAlign *align, RTri *out, RTAlignReport *report) {
    report_clear(report);
    if (!tri || !out) return report_error(report, RT_E_ARG, "null triangle or output");

    if (!align || !align->enabled) {
        *out = *tri;
        return RT_OK;
    }

    double grid = align->grid;
    if (!isfinite(grid) || !(grid > 0.0))
        return report_error(report, RT_E_GRID, "alignment grid must be finite and > 0");

    /* Validate every vertex before writing *out, so a rejected call leaves the
     * caller's output and the render target untouched. A missing vertex is a
     * NaN coordinate; an out-of-range or infinite one is an overflow. */
    for (int i = 0; i < 3; ++i) {
        const RVert *v = &tri->v[i];
        if (isnan(v->x) || isnan(v->y) || isnan(v->z))
            return report_vertex_error(report, RT_E_MISSING, i, "coordinate is missing (NaN)");
        if (isinf(v->x) || isinf(v->y) || isinf(v->z) ||
            fabs(v->x) > RT_MAX_COORD || fabs(v->y) > RT_MAX_COORD)
            return report_vertex_error(report, RT_E_OVERFLOW, i, "coordinate outside +/-1e9");
    }

    if (fabs(edge(&tri->v[0], &tri->v[1], tri->v[2].x, tri->v[2].y)) < 1e-12)
        return report_error(report, RT_E_DEGEN, "triangle has zero area");

    RTri snapped = *tri;
    for (int i = 0; i < 3; ++i) {
        double sx = round(tri->v[i].x / grid) * grid;
        double sy = round(tri->v[i].y / grid) * grid;
        /* A grid so small that x/grid or the product leaves the double range
         * is an overflow of that vertex, not a silent infinity downstream. */
        if (!isfinite(sx) || !isfinite(sy))
            return report_vertex_error(report, RT_E_OVERFLOW, i, "offset overflows the grid");
        if (report) {
            report->dx[i] = sx - tri->v[i].x;
            report->dy[i] = sy - tri->v[i].y;
            double ax = fabs(report->dx[i]), ay = fabs(report->dy[i]);
            if (ax > report->max_displacement) report->max_displacement = ax;
            if (ay > report->max_displacement) report->max_displacement = ay;
        }
        snapped.v[i].x = sx;
        snapped.v[i].y = sy;
    }

    if (fabs(edge(&snapped.v[0], &snapped.v[1], snapped.v[2].x, snapped.v[2].y)) < 1e-12)
        report_collapsed(report);

    *out = snapped;
    return RT_OK;
}

int rt_write_ppm(const RTarget *t, const char *path) {
    if (!t || !path) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) return -2;
    fprintf(f, "P6\n%d %d\n255\n", t->w, t->h);
    fwrite(t->rgb, 1, (size_t)t->w * (size_t)t->h * 3, f);
    fclose(f);
    return 0;
}
