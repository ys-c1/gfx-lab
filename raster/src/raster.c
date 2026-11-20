#define _POSIX_C_SOURCE 200809L
#include "raster.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Area below which a triangle is not considered to have a usable winding.
 * Matches the threshold used by the original rasterizer for rejecting
 * "flat" triangles. */
#define RT_AREA_EPS 1e-12

int rt_init(RTarget *t, int w, int h) {
    if (!t || w <= 0 || h <= 0) return -1;
    t->w = w; t->h = h;
    t->cull = RT_CULL_NONE;
    t->z = (float*)malloc(sizeof(float) * (size_t)w * (size_t)h);
    t->rgb = (unsigned char*)malloc((size_t)w * (size_t)h * 3);
    if (!t->z || !t->rgb) { rt_free(t); return -1; }
    for (int i = 0; i < w * h; ++i) { t->z[i] = 1e30f; t->rgb[i*3+0] = 0; t->rgb[i*3+1] = 0; t->rgb[i*3+2] = 0; }
    return 0;
}

void rt_free(RTarget *t) {
    if (!t) return;
    free(t->z); free(t->rgb);
    t->z = NULL; t->rgb = NULL; t->w = 0; t->h = 0; t->cull = RT_CULL_NONE;
}

int rt_clear(RTarget *t) {
    if (!t || !t->z || !t->rgb) return -1;
    size_t n = (size_t)t->w * (size_t)t->h;
    for (size_t i = 0; i < n; ++i) {
        t->z[i] = 1e30f;
        t->rgb[i*3+0] = 0; t->rgb[i*3+1] = 0; t->rgb[i*3+2] = 0;
    }
    return 0;
}

void rt_set_cull(RTarget *t, RTCullMode mode) { if (t) t->cull = mode; }
RTCullMode rt_get_cull(const RTarget *t) { return t ? t->cull : RT_CULL_NONE; }

static double edge(const RVert *a, const RVert *b, double px, double py) {
    return (b->x - a->x) * (py - a->y) - (b->y - a->y) * (px - a->x);
}

static int finite_vert(const RVert *v) {
    return isfinite(v->x) && isfinite(v->y) && isfinite(v->z);
}

RTCullReason rt_classify(const RTri *tri, double *signed_area,
                         double *signed_area2, RTWinding *winding) {
    double a2 = 0.0, a = 0.0;
    RTWinding w = RT_WIND_DEGENERATE;
    RTCullReason r = RT_CULL_REASON_NONE;

    if (tri) {
        const RVert *v0 = &tri->v[0], *v1 = &tri->v[1], *v2 = &tri->v[2];
        /* Coincident vertices (same x and y) have no meaningful winding. */
        int coincident =
            (v0->x == v1->x && v0->y == v1->y) ||
            (v1->x == v2->x && v1->y == v2->y) ||
            (v2->x == v0->x && v2->y == v0->y);
        a2 = (v1->x - v0->x) * (v2->y - v0->y) -
             (v1->y - v0->y) * (v2->x - v0->x);
        a = a2 * 0.5;
        if (!finite_vert(v0) || !finite_vert(v1) || !finite_vert(v2) ||
            !isfinite(a2)) {
            r = RT_CULL_REASON_DEGENERATE_WINDING;
        } else if (coincident) {
            r = RT_CULL_REASON_COINCIDENT;
        } else if (a2 == 0.0) {
            r = RT_CULL_REASON_ZERO_AREA;
        } else if (fabs(a2) < RT_AREA_EPS) {
            r = RT_CULL_REASON_DEGENERATE_WINDING;
        } else {
            w = (a2 > 0.0) ? RT_WIND_CCW : RT_WIND_CW;
        }
    } else {
        r = RT_CULL_REASON_DEGENERATE_WINDING;
    }

    if (signed_area) *signed_area = a;
    if (signed_area2) *signed_area2 = a2;
    if (winding) *winding = w;
    return r;
}

/* Decide whether the current target's cull mode rejects this winding.
 * Returns the cull reason, or RT_CULL_REASON_NONE if the triangle must be
 * drawn. */
static RTCullReason cull_reason_for(RTCullMode mode, RTWinding w) {
    switch (mode) {
        case RT_CULL_NONE:
        case RT_CULL_BOTH:
            return RT_CULL_REASON_NONE;
        case RT_CULL_KEEP_CCW:
            return w == RT_WIND_CW ? RT_CULL_REASON_BACKFACE_CW
                                   : RT_CULL_REASON_NONE;
        case RT_CULL_KEEP_CW:
            return w == RT_WIND_CCW ? RT_CULL_REASON_BACKFACE_CCW
                                    : RT_CULL_REASON_NONE;
        default:
            return RT_CULL_REASON_NONE;
    }
}

int rt_triangle_info(RTarget *t, const RTri *tri, RTTriInfo *info) {
    if (!t || !t->z || !t->rgb || !tri) return -1;
    double a2;
    RTCullReason reason = rt_classify(tri, NULL, &a2, NULL);
    RTTriInfo local;
    memset(&local, 0, sizeof local);
    local.signed_area2 = a2;
    local.signed_area = a2 * 0.5;

    if (reason != RT_CULL_REASON_NONE) {
        local.winding = RT_WIND_DEGENERATE;
        local.reason = reason;
        local.covered = local.filled = 0;
        if (info) *info = local;
        return -2;
    }

    RTWinding w = (a2 > 0.0) ? RT_WIND_CCW : RT_WIND_CW;
    local.winding = w;

    RTCullMode mode = t->cull;
    RTCullReason cr = cull_reason_for(mode, w);
    if (cr != RT_CULL_REASON_NONE) {
        local.reason = cr;
        local.covered = local.filled = 0;
        if (info) *info = local;
        return 0;
    }

    /* Legacy (no-cull) path: positive winding only, using the exact edge
     * tests and z interpolation of the original rasterizer.  RT_CULL_NONE
     * keeps this path so that its output is bit-for-bit unchanged.  In that
     * mode a negative-winding triangle has no pixels, exactly as before. */
    int neg = (w == RT_WIND_CW);
    if (mode == RT_CULL_NONE && neg) {
        local.reason = RT_CULL_REASON_NONE;
        local.covered = local.filled = 0;
        if (info) *info = local;
        return 0;
    }
    /* The legacy fill rule accepts only w0,w1,w2 >= 0, i.e. the positive
     * (CCW) side.  RT_CULL_BOTH on a negative (CW) triangle is the mirror
     * image: negate all three edge functions, which accepts exactly the
     * pixels the legacy path would accept if the winding were positive. */
    int two_sided = neg;
    int minx = (int)floor(fmin(fmin(tri->v[0].x, tri->v[1].x), tri->v[2].x));
    int maxx = (int)ceil(fmax(fmax(tri->v[0].x, tri->v[1].x), tri->v[2].x));
    int miny = (int)floor(fmin(fmin(tri->v[0].y, tri->v[1].y), tri->v[2].y));
    int maxy = (int)ceil(fmax(fmax(tri->v[0].y, tri->v[1].y), tri->v[2].y));
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx >= t->w) maxx = t->w - 1;
    if (maxy >= t->h) maxy = t->h - 1;
    int covered = 0, written = 0;
    for (int y = miny; y <= maxy; ++y) {
        for (int x = minx; x <= maxx; ++x) {
            double px = x + 0.5, py = y + 0.5;
            double w0 = edge(&tri->v[1], &tri->v[2], px, py);
            double w1 = edge(&tri->v[2], &tri->v[0], px, py);
            double w2 = edge(&tri->v[0], &tri->v[1], px, py);
            if (!two_sided) {
                if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            } else {
                if (w0 > 0 || w1 > 0 || w2 > 0) continue;
            }
            double sum = w0 + w1 + w2;
            double z = (w0 * tri->v[0].z + w1 * tri->v[1].z + w2 * tri->v[2].z) / sum;
            size_t i = (size_t)y * (size_t)t->w + (size_t)x;
            ++covered;
            if ((float)z < t->z[i]) {
                t->z[i] = (float)z;
                t->rgb[i*3+0] = tri->rgb[0];
                t->rgb[i*3+1] = tri->rgb[1];
                t->rgb[i*3+2] = tri->rgb[2];
                ++written;
            }
        }
    }
    local.reason = RT_CULL_REASON_NONE;
    local.covered = covered;
    local.filled = written;
    if (info) *info = local;
    return written;
}

int rt_triangle(RTarget *t, const RTri *tri) {
    if (!t || !tri) return -1;
    return rt_triangle_info(t, tri, NULL);
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

void rt_stats_free(RTBatchStats *stats) {
    if (!stats) return;
    free(stats->culled_idx);
    free(stats->rejected_idx);
    free(stats->info);
    memset(stats, 0, sizeof *stats);
}

int rt_batch(RTarget *t, const RTri *tris, int n, RTBatchStats *stats) {
    if (!t || !stats) return -1;
    if (n < 0 || (n > 0 && !tris)) return -1;
    memset(stats, 0, sizeof *stats);
    stats->triangles = n;
    stats->culled_idx = (int*)malloc(sizeof(int) * (size_t)(n > 0 ? n : 1));
    stats->rejected_idx = (int*)malloc(sizeof(int) * (size_t)(n > 0 ? n : 1));
    stats->info = (RTTriInfo*)calloc((size_t)(n > 0 ? n : 1), sizeof(RTTriInfo));
    if (!stats->culled_idx || !stats->rejected_idx || !stats->info) {
        rt_stats_free(stats);
        return -1;
    }

    double t0 = now_ms();
    for (int i = 0; i < n; ++i) {
        RTTriInfo info;
        int rc = rt_triangle_info(t, &tris[i], &info);
        if (rc < 0) {
            stats->rejected_idx[stats->rejected_n++] = i;
            ++stats->rejected;
        } else if (info.reason != RT_CULL_REASON_NONE) {
            stats->culled_idx[stats->culled_n++] = i;
            ++stats->culled;
        } else {
            ++stats->drawn;
        }
        ++stats->reason_counts[info.reason];
        stats->covered += info.covered;
        stats->filled += info.filled;
        stats->info[i] = info;
    }
    stats->elapsed_ms = now_ms() - t0;
    return 0;
}

const char *rt_winding_name(RTWinding w) {
    switch (w) {
        case RT_WIND_CCW: return "ccw";
        case RT_WIND_CW: return "cw";
        default: return "degenerate";
    }
}

const char *rt_cull_reason_name(RTCullReason r) {
    switch (r) {
        case RT_CULL_REASON_NONE: return "drawn";
        case RT_CULL_REASON_BACKFACE_CW: return "backface_cw";
        case RT_CULL_REASON_BACKFACE_CCW: return "backface_ccw";
        case RT_CULL_REASON_COINCIDENT: return "coincident_vertices";
        case RT_CULL_REASON_ZERO_AREA: return "zero_area";
        case RT_CULL_REASON_DEGENERATE_WINDING: return "degenerate_winding";
        default: return "unknown";
    }
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
