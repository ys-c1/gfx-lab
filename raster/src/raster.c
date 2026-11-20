#include "raster.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int rt_init(RTarget *t, int w, int h) {
    if (!t || w <= 0 || h <= 0) return -1;
    t->w = w; t->h = h;
    t->z = (float*)malloc(sizeof(float) * (size_t)w * (size_t)h);
    t->rgb = (unsigned char*)malloc((size_t)w * (size_t)h * 3);
    t->wire = (unsigned char*)calloc((size_t)w * (size_t)h, 1);
    if (!t->z || !t->rgb || !t->wire) { rt_free(t); return -1; }
    for (int i = 0; i < w * h; ++i) { t->z[i] = 1e30f; t->rgb[i*3+0] = 0; t->rgb[i*3+1] = 0; t->rgb[i*3+2] = 0; }
    return 0;
}

void rt_free(RTarget *t) {
    if (!t) return;
    free(t->z); free(t->rgb); free(t->wire);
    t->z = NULL; t->rgb = NULL; t->wire = NULL;
    t->w = 0; t->h = 0;
}

static double edge(const RVert *a, const RVert *b, double px, double py) {
    return (b->x - a->x) * (py - a->y) - (b->y - a->y) * (px - a->x);
}

int rt_triangle(RTarget *t, const RTri *tri) {
    if (!t || !tri) return -1;
    double area = edge(&tri->v[0], &tri->v[1], tri->v[2].x, tri->v[2].y);
    if (fabs(area) < 1e-12) return -2;
    int minx = (int)floor(fmin(fmin(tri->v[0].x, tri->v[1].x), tri->v[2].x));
    int maxx = (int)ceil(fmax(fmax(tri->v[0].x, tri->v[1].x), tri->v[2].x));
    int miny = (int)floor(fmin(fmin(tri->v[0].y, tri->v[1].y), tri->v[2].y));
    int maxy = (int)ceil(fmax(fmax(tri->v[0].y, tri->v[1].y), tri->v[2].y));
    if (minx < 0) minx = 0; if (miny < 0) miny = 0;
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

/* ---------------- wireframe ---------------- */

/* Stage 1, per pixel: distance from (x+0.5,y+0.5) to segment a-b.  The
 * projection is clamped to [0,1] so the closest point never leaves the edge;
 * this is what makes "vertex coincidence" and "short edge" detectable rather
 * than silently measuring distance to an infinite line. */
int rt_edge_dist(const RVert *a, const RVert *b, int x, int y, double *dist) {
    if (!a || !b || !dist || x < 0 || y < 0) return RT_WIRE_ERR_ARG;
    double px = (double)x + 0.5, py = (double)y + 0.5;
    double dx = b->x - a->x, dy = b->y - a->y;
    double len2 = dx * dx + dy * dy;
    if (!(len2 > 0.0)) return RT_WIRE_ERR_DUP_VERTEX; /* also catches NaN */
    double t = ((px - a->x) * dx + (py - a->y) * dy) / len2;
    if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;
    double cx = a->x + t * dx, cy = a->y + t * dy;
    double ex = px - cx, ey = py - cy;
    *dist = sqrt(ex * ex + ey * ey);
    return RT_WIRE_OK;
}

const char *rt_wire_strerror(int code) {
    switch (code) {
    case RT_WIRE_OK: return "ok";
    case RT_WIRE_ERR_ARG: return "null or out-of-range argument";
    case RT_WIRE_ERR_THRESHOLD: return "wire threshold must be >= 0";
    case RT_WIRE_ERR_SHORT_EDGE: return "edge shorter than RT_WIRE_MIN_EDGE";
    case RT_WIRE_ERR_DUP_VERTEX: return "coincident vertices";
    default: return "unknown wireframe error";
    }
}

double rt_wire_nominal_width(const RWire *cfg) {
    return cfg ? 2.0 * cfg->threshold : 0.0;
}

int rt_wire_errfmt(const RWireErr *err, char *buf, size_t cap) {
    if (!err || !buf || cap == 0) return -1;
    int n;
    if (err->code == RT_WIRE_ERR_SHORT_EDGE || err->code == RT_WIRE_ERR_DUP_VERTEX)
        n = snprintf(buf, cap, "edge %d (v%d-v%d): %s", err->edge, err->vertex,
                     err->vertex2, rt_wire_strerror(err->code));
    else if (err->code == RT_WIRE_ERR_THRESHOLD)
        n = snprintf(buf, cap, "%s", rt_wire_strerror(err->code));
    else
        n = snprintf(buf, cap, "%s", rt_wire_strerror(err->code));
    return (n < 0) ? -1 : (n >= (int)cap ? -2 : 0);
}

/* Edge i always runs from vertex i to vertex (i+1)%3, in the caller's order. */
static int wire_check(const RTri *tri, const RWire *cfg, RWireErr *err) {
    if (!tri || !cfg) {
        if (err) err->code = RT_WIRE_ERR_ARG;
        return RT_WIRE_ERR_ARG;
    }
    if (!(cfg->threshold >= 0.0) || !isfinite(cfg->threshold)) {
        if (err) err->code = RT_WIRE_ERR_THRESHOLD;
        return RT_WIRE_ERR_THRESHOLD;
    }
    if (!isfinite(tri->v[0].x) || !isfinite(tri->v[0].y) ||
        !isfinite(tri->v[1].x) || !isfinite(tri->v[1].y) ||
        !isfinite(tri->v[2].x) || !isfinite(tri->v[2].y)) {
        if (err) err->code = RT_WIRE_ERR_ARG;
        return RT_WIRE_ERR_ARG;
    }

    for (int i = 0; i < 3; ++i) {
        int j = (i + 1) % 3;
        double dx = tri->v[j].x - tri->v[i].x;
        double dy = tri->v[j].y - tri->v[i].y;
        double len = sqrt(dx * dx + dy * dy);
        if (len == 0.0) {
            if (err) { err->code = RT_WIRE_ERR_DUP_VERTEX; err->edge = i;
                       err->vertex = i; err->vertex2 = j; }
            return RT_WIRE_ERR_DUP_VERTEX;
        }
        if (len < RT_WIRE_MIN_EDGE) {
            if (err) { err->code = RT_WIRE_ERR_SHORT_EDGE; err->edge = i;
                       err->vertex = i; err->vertex2 = j; }
            return RT_WIRE_ERR_SHORT_EDGE;
        }
    }
    return RT_WIRE_OK;
}

/* Stage 2.  Walks the same integer bounding box the fill pass would use, plus
 * a margin of ceil(threshold)+1 so edge pixels just outside the triangle still
 * get their centres measured.  Pixel (x,y) is covered by edge i when
 * rt_edge_dist() <= threshold; each hit writes cfg->rgb and marks t->wire. */
int rt_wire(RTarget *t, const RTri *tri, const RWire *cfg,
            RWireStats *stats, RWireErr *err) {
    if (err) { err->code = RT_WIRE_OK; err->edge = -1; err->vertex = -1; err->vertex2 = -1; }
    if (!t || !cfg) {
        if (err) err->code = RT_WIRE_ERR_ARG;
        return RT_WIRE_ERR_ARG;
    }
    /* Line rendering off: do not validate, do not touch a single byte. */
    if (!cfg->enabled) {
        if (stats) { stats->edge[0] = stats->edge[1] = stats->edge[2] = 0; stats->covered = 0; }
        return RT_WIRE_OK;
    }
    int rc = wire_check(tri, cfg, err);
    if (rc != RT_WIRE_OK) return rc;

    double margin = ceil(cfg->threshold) + 1.0;
    /* Clamp in double before the int cast: a huge threshold must not overflow. */
    double dminx = fmin(fmin(tri->v[0].x, tri->v[1].x), tri->v[2].x) - margin;
    double dmaxx = fmax(fmax(tri->v[0].x, tri->v[1].x), tri->v[2].x) + margin;
    double dminy = fmin(fmin(tri->v[0].y, tri->v[1].y), tri->v[2].y) - margin;
    double dmaxy = fmax(fmax(tri->v[0].y, tri->v[1].y), tri->v[2].y) + margin;
    int per_edge[3] = {0, 0, 0}, covered = 0;
    if (dmaxx < 0.0 || dmaxy < 0.0 || dminx > (double)(t->w - 1) ||
        dminy > (double)(t->h - 1)) {
        if (stats) { stats->edge[0] = stats->edge[1] = stats->edge[2] = 0; stats->covered = 0; }
        return 0;
    }
    int minx = dminx < 0.0 ? 0 : (int)floor(dminx);
    int maxx = dmaxx > (double)(t->w - 1) ? t->w - 1 : (int)floor(dmaxx);
    int miny = dminy < 0.0 ? 0 : (int)floor(dminy);
    int maxy = dmaxy > (double)(t->h - 1) ? t->h - 1 : (int)floor(dmaxy);

    for (int y = miny; y <= maxy; ++y) {
        for (int x = minx; x <= maxx; ++x) {
            int hit = 0;
            for (int i = 0; i < 3; ++i) {
                const RVert *a = &tri->v[i], *b = &tri->v[(i + 1) % 3];
                double d;
                /* Validation already proved no degenerate edge. */
                rt_edge_dist(a, b, x, y, &d);
                if (d <= cfg->threshold) { ++per_edge[i]; hit = 1; }
            }
            if (hit) {
                size_t i = (size_t)y * (size_t)t->w + (size_t)x;
                t->rgb[i*3+0] = cfg->rgb[0];
                t->rgb[i*3+1] = cfg->rgb[1];
                t->rgb[i*3+2] = cfg->rgb[2];
                t->wire[i] = 1;
                ++covered;
            }
        }
    }
    if (stats) {
        stats->edge[0] = per_edge[0]; stats->edge[1] = per_edge[1];
        stats->edge[2] = per_edge[2]; stats->covered = covered;
    }
    return covered;
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
