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
    t->clip_on = 0; t->cx = t->cy = t->cw = t->ch = 0;
    rt_clear(t);
    return 0;
}

void rt_free(RTarget *t) {
    if (!t) return;
    free(t->z); free(t->rgb);
    t->z = NULL; t->rgb = NULL; t->w = 0; t->h = 0;
    t->clip_on = 0; t->cx = t->cy = t->cw = t->ch = 0;
}

void rt_clear(RTarget *t) {
    if (!t || !t->z || !t->rgb) return;
    for (int i = 0; i < t->w * t->h; ++i) {
        t->z[i] = 1e30f;
        t->rgb[i*3+0] = 0; t->rgb[i*3+1] = 0; t->rgb[i*3+2] = 0;
    }
}

int rt_set_clip(RTarget *t, int x, int y, int w, int h) {
    if (!t) return -1;
    if (w == 0 || h == 0) return -2;        /* empty rect */
    if (w < 0 || h < 0) return -3;          /* negative extent */
    if (x < 0 || y < 0 || x > t->w - w || y > t->h - h) return -4; /* outside target */
    t->clip_on = 1;
    t->cx = x; t->cy = y; t->cw = w; t->ch = h;
    return 0;
}

void rt_clear_clip(RTarget *t) {
    if (!t) return;
    t->clip_on = 0;
}

const char *rt_clip_strerror(int rc) {
    switch (rc) {
    case 0:  return "ok";
    case -1: return "null target";
    case -2: return "clip rect is empty (zero width or height)";
    case -3: return "clip rect has negative width or height";
    case -4: return "clip rect exceeds target bounds";
    default: return "unknown error";
    }
}

static double edge(const RVert *a, const RVert *b, double px, double py) {
    return (b->x - a->x) * (py - a->y) - (b->y - a->y) * (px - a->x);
}

/* Shared inner loop. Degenerate input writes nothing and returns 0. */
static int raster_span(RTarget *t, const RVert v[3], const unsigned char rgb[3],
                       int minx, int miny, int maxx, int maxy) {
    double area = edge(&v[0], &v[1], v[2].x, v[2].y);
    if (fabs(area) < 1e-12) return 0;
    int written = 0;
    for (int y = miny; y <= maxy; ++y) {
        for (int x = minx; x <= maxx; ++x) {
            double px = x + 0.5, py = y + 0.5;
            double w0 = edge(&v[1], &v[2], px, py);
            double w1 = edge(&v[2], &v[0], px, py);
            double w2 = edge(&v[0], &v[1], px, py);
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            double sum = w0 + w1 + w2;
            double z = (w0 * v[0].z + w1 * v[1].z + w2 * v[2].z) / sum;
            size_t i = (size_t)y * (size_t)t->w + (size_t)x;
            if ((float)z < t->z[i]) {
                t->z[i] = (float)z;
                t->rgb[i*3+0] = rgb[0];
                t->rgb[i*3+1] = rgb[1];
                t->rgb[i*3+2] = rgb[2];
                ++written;
            }
        }
    }
    return written;
}

/* ---- step 1: clip triangle to the clip rect (Sutherland-Hodgman) ---- */

typedef struct { double x, y, z; } CVert;

/* Clip polygon `in` (n verts) against one half-plane, append to `out`.
 * axis 0 -> x, 1 -> y; keep_le: keep v <= bound, else keep v >= bound. */
static int clip_half(const CVert *in, int n, CVert *out, int axis,
                     double bound, int keep_le) {
    int m = 0;
    for (int i = 0; i < n; ++i) {
        const CVert *a = &in[i];
        const CVert *b = &in[(i + 1) % n];
        double va = axis ? a->y : a->x;
        double vb = axis ? b->y : b->x;
        int ina = keep_le ? (va <= bound) : (va >= bound);
        int inb = keep_le ? (vb <= bound) : (vb >= bound);
        if (ina) out[m++] = *a;
        if (ina != inb) {
            double tt = (bound - va) / (vb - va);
            CVert p;
            p.x = a->x + tt * (b->x - a->x);
            p.y = a->y + tt * (b->y - a->y);
            p.z = a->z + tt * (b->z - a->z);   /* z stays on the triangle plane */
            out[m++] = p;
        }
    }
    return m;
}

/* Returns convex polygon (3..7 verts) of `tri` clipped to t's clip rect. */
static int clip_tri_to_rect(const RTarget *t, const RTri *tri, CVert *out) {
    CVert bufA[8], bufB[8];
    for (int i = 0; i < 3; ++i) {
        bufA[i].x = tri->v[i].x;
        bufA[i].y = tri->v[i].y;
        bufA[i].z = tri->v[i].z;
    }
    int n = 3;
    n = clip_half(bufA, n, bufB, 0, (double)t->cx, 0);            /* x >= cx      */
    n = clip_half(bufB, n, bufA, 0, (double)(t->cx + t->cw), 1);  /* x <= cx+cw   */
    n = clip_half(bufA, n, bufB, 1, (double)t->cy, 0);            /* y >= cy      */
    n = clip_half(bufB, n, out,  1, (double)(t->cy + t->ch), 1);  /* y <= cy+ch   */
    return n;
}

int rt_triangle_ex(RTarget *t, const RTri *tri, RTriStat *st) {
    if (!t || !tri) return -1;
    double area = edge(&tri->v[0], &tri->v[1], tri->v[2].x, tri->v[2].y);
    if (fabs(area) < 1e-12) return -2;
    int minx = (int)floor(fmin(fmin(tri->v[0].x, tri->v[1].x), tri->v[2].x));
    int maxx = (int)ceil(fmax(fmax(tri->v[0].x, tri->v[1].x), tri->v[2].x));
    int miny = (int)floor(fmin(fmin(tri->v[0].y, tri->v[1].y), tri->v[2].y));
    int maxy = (int)ceil(fmax(fmax(tri->v[0].y, tri->v[1].y), tri->v[2].y));
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx >= t->w) maxx = t->w - 1;
    if (maxy >= t->h) maxy = t->h - 1;

    if (!t->clip_on) {
        /* identical code path (and output) as the unclipped rasterizer */
        int written = raster_span(t, tri->v, tri->rgb, minx, miny, maxx, maxy);
        if (st) { st->filled = written; st->clipped = 0; }
        return written;
    }

    /* step 1: clip the triangle to the clip rect */
    CVert poly[8];
    int n = clip_tri_to_rect(t, tri, poly);

    /* step 2: rasterize the clipped polygon as a triangle fan */
    int written = 0;
    int rx0 = t->cx, rx1 = t->cx + t->cw - 1;
    int ry0 = t->cy, ry1 = t->cy + t->ch - 1;
    for (int i = 1; i + 1 < n; ++i) {
        RVert fv[3] = {
            { poly[0].x,   poly[0].y,   poly[0].z   },
            { poly[i].x,   poly[i].y,   poly[i].z   },
            { poly[i+1].x, poly[i+1].y, poly[i+1].z },
        };
        int fminx = (int)floor(fmin(fmin(fv[0].x, fv[1].x), fv[2].x));
        int fmaxx = (int)ceil(fmax(fmax(fv[0].x, fv[1].x), fv[2].x));
        int fminy = (int)floor(fmin(fmin(fv[0].y, fv[1].y), fv[2].y));
        int fmaxy = (int)ceil(fmax(fmax(fv[0].y, fv[1].y), fv[2].y));
        if (fminx < rx0) fminx = rx0;
        if (fmaxx > rx1) fmaxx = rx1;
        if (fminy < ry0) fminy = ry0;
        if (fmaxy > ry1) fmaxy = ry1;
        written += raster_span(t, fv, tri->rgb, fminx, fminy, fmaxx, fmaxy);
    }

    /* count samples the original triangle covers in-target but outside the rect */
    int clipped = 0;
    if (st) {
        for (int y = miny; y <= maxy; ++y) {
            for (int x = minx; x <= maxx; ++x) {
                double px = x + 0.5, py = y + 0.5;
                double w0 = edge(&tri->v[1], &tri->v[2], px, py);
                double w1 = edge(&tri->v[2], &tri->v[0], px, py);
                double w2 = edge(&tri->v[0], &tri->v[1], px, py);
                if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                if (x < rx0 || x > rx1 || y < ry0 || y > ry1) ++clipped;
            }
        }
        st->filled = written;
        st->clipped = clipped;
    }
    return written;
}

int rt_triangle(RTarget *t, const RTri *tri) {
    return rt_triangle_ex(t, tri, NULL);
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
