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
    t->near_z = 0.0;
    t->clip = 0;
    return 0;
}

void rt_free(RTarget *t) {
    if (!t) return;
    free(t->z); free(t->rgb);
    t->z = NULL; t->rgb = NULL; t->w = 0; t->h = 0;
    t->near_z = 0.0; t->clip = 0;
}

static double edge(const RVert *a, const RVert *b, double px, double py) {
    return (b->x - a->x) * (py - a->y) - (b->y - a->y) * (px - a->x);
}

static int coincident(const RVert *a, const RVert *b) {
    double dx = a->x - b->x, dy = a->y - b->y, dz = a->z - b->z;
    return dx * dx + dy * dy + dz * dz < 1e-24;
}

/* Rasterize one triangle; returns pixel count or RE_DEGEN. Shared by both paths. */
static int rasterize(RTarget *t, const RTri *tri) {
    double area = edge(&tri->v[0], &tri->v[1], tri->v[2].x, tri->v[2].y);
    if (fabs(area) < 1e-12) return RE_DEGEN;
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
    if (!t || !tri) return RE_ARG;
    double area = edge(&tri->v[0], &tri->v[1], tri->v[2].x, tri->v[2].y);
    if (fabs(area) < 1e-12) return RE_DEGEN;
    return rasterize(t, tri);
}

/* Intersection of edge a->b with plane z = near; caller guarantees it crosses. */
static RVert plane_hit(const RVert *a, const RVert *b, double near) {
    double t = (near - a->z) / (b->z - a->z);
    RVert p;
    p.x = a->x + (b->x - a->x) * t;
    p.y = a->y + (b->y - a->y) * t;
    p.z = near;
    return p;
}

/*
 * Stage 1: clip one triangle against the near plane (keep half-space z >= near)
 * with Sutherland-Hodgman, then fan-triangulate the surviving polygon.
 * Returns number of triangles written, or RE_DEGEN / RE_EMPTY on failure.
 * *new_verts counts vertices that did not exist in the input triangle.
 */
int rt_clip_triangle(const RTri *in, double near, RTri *out, int maxout, int *new_verts) {
    if (!in || !out || maxout <= 0) return RE_ARG;
    if (new_verts) *new_verts = 0;
    if (coincident(&in->v[0], &in->v[1]) ||
        coincident(&in->v[1], &in->v[2]) ||
        coincident(&in->v[2], &in->v[0]))
        return RE_DEGEN;

    RVert poly[8];
    int np = 0;
    for (int e = 0; e < 3; ++e) {
        const RVert *a = &in->v[e];
        const RVert *b = &in->v[(e + 1) % 3];
        int ain = a->z >= near, bin = b->z >= near;
        if (ain && bin) {
            poly[np++] = *b;
        } else if (ain && !bin) {
            poly[np++] = plane_hit(a, b, near);
        } else if (!ain && bin) {
            poly[np++] = plane_hit(a, b, near);
            poly[np++] = *b;
        }
    }
    if (np == 0) return RE_EMPTY;

    /* Drop duplicate vertices produced when an input vertex sits on the plane. */
    RVert uniq[8];
    int nu = 0;
    for (int i = 0; i < np; ++i) {
        int dup = 0;
        for (int j = 0; j < nu; ++j)
            if (coincident(&poly[i], &uniq[j])) { dup = 1; break; }
        if (!dup) uniq[nu++] = poly[i];
    }
    if (nu < 3) return RE_EMPTY;
    if (nu - 2 > maxout) return RE_ARG;

    int nt = 0;
    for (int i = 1; i + 1 < nu; ++i) {
        double area = edge(&uniq[0], &uniq[i], uniq[i + 1].x, uniq[i + 1].y);
        if (fabs(area) < 1e-12) continue; /* degenerate fan slice, skip, not an error */
        out[nt].v[0] = uniq[0];
        out[nt].v[1] = uniq[i];
        out[nt].v[2] = uniq[i + 1];
        out[nt].rgb[0] = in->rgb[0];
        out[nt].rgb[1] = in->rgb[1];
        out[nt].rgb[2] = in->rgb[2];
        ++nt;
    }
    if (nt == 0) return RE_EMPTY;
    if (new_verts) {
        int made = 0;
        for (int i = 0; i < nu; ++i) {
            int frominput = 0;
            for (int k = 0; k < 3; ++k)
                if (coincident(&uniq[i], &in->v[k])) { frominput = 1; break; }
            if (!frominput) ++made;
        }
        *new_verts = made;
    }
    return nt;
}

const char *rt_strerror(int code) {
    switch (code) {
    case RE_OK:     return "ok";
    case RE_ARG:   return "invalid argument";
    case RE_DEGEN: return "degenerate triangle (coincident vertices or zero area)";
    case RE_NEAR:  return "vertex depth below near plane";
    case RE_EMPTY: return "triangle fully removed by near-plane clipping";
    default:       return "unknown error";
    }
}

/*
 * Two-stage draw: every triangle is either near-plane clipped then rasterized,
 * or (clip == 0) rejected with RE_NEAR when a vertex lies behind the near plane.
 */
int rt_draw(RTarget *t, const RTri *tris, int n, RStats *st) {
    if (!t || (!tris && n > 0) || n < 0) return RE_ARG;
    RStats local;
    if (!st) st = &local;
    st->tris_in = n; st->tris_out = 0; st->new_verts = 0; st->pixels = 0;
    st->err_tri = -1;
    int first = RE_OK;

    for (int i = 0; i < n; ++i) {
        const RTri *s = &tris[i];
        if (coincident(&s->v[0], &s->v[1]) ||
            coincident(&s->v[1], &s->v[2]) ||
            coincident(&s->v[2], &s->v[0])) {
            fprintf(stderr, "triangle %d: %s\n", i, rt_strerror(RE_DEGEN));
            if (first == RE_OK) { first = RE_DEGEN; st->err_tri = i; }
            continue;
        }

        if (t->clip) {
            RTri clipped[4];
            int made = 0;
            int rc = rt_clip_triangle(s, t->near_z, clipped, 4, &made);
            if (rc == RE_EMPTY) {
                fprintf(stderr, "triangle %d: %s\n", i, rt_strerror(RE_EMPTY));
                if (first == RE_OK) { first = RE_EMPTY; st->err_tri = i; }
                continue;
            }
            if (rc < 0) {
                fprintf(stderr, "triangle %d: %s\n", i, rt_strerror(rc));
                if (first == RE_OK) { first = rc; st->err_tri = i; }
                continue;
            }
            st->new_verts += made;
            for (int k = 0; k < rc; ++k) {
                int px = rasterize(t, &clipped[k]);
                if (px < 0) {
                    fprintf(stderr, "triangle %d: %s\n", i, rt_strerror(px));
                    if (first == RE_OK) { first = px; st->err_tri = i; }
                    continue;
                }
                st->pixels += px;
                ++st->tris_out;
            }
        } else {
            if (s->v[0].z < t->near_z || s->v[1].z < t->near_z || s->v[2].z < t->near_z) {
                fprintf(stderr, "triangle %d: %s (near=%g)\n", i, rt_strerror(RE_NEAR), t->near_z);
                if (first == RE_OK) { first = RE_NEAR; st->err_tri = i; }
                continue;
            }
            double area = edge(&s->v[0], &s->v[1], s->v[2].x, s->v[2].y);
            if (fabs(area) < 1e-12) {
                fprintf(stderr, "triangle %d: %s\n", i, rt_strerror(RE_DEGEN));
                if (first == RE_OK) { first = RE_DEGEN; st->err_tri = i; }
                continue;
            }
            int px = rasterize(t, s);
            if (px < 0) {
                fprintf(stderr, "triangle %d: %s\n", i, rt_strerror(px));
                if (first == RE_OK) { first = px; st->err_tri = i; }
                continue;
            }
            st->pixels += px;
            ++st->tris_out;
        }
    }
    return first;
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
