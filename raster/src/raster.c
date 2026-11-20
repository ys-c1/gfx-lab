#include "raster.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* The rasteriser is split into two passes so that a blended frame is built in
 * two distinct stages:
 *
 *   pass 1  coverage + depth test + colour resolution -> staging list
 *           (blend with alpha)
 *   pass 2  write-back of the staged fragments
 *
 * Only fragments that survive the depth test enter the staging list, so pass 2
 * never touches a destination that pass 1 did not already accept.  With
 * blending disabled pass 2 stores exactly what the original one-pass
 * rasteriser stored, byte for byte.
 */

static void record_error(RTarget *t, int code, int tri_index) {
    t->err_code = code;
    t->err_tri = (code == RT_OK) ? -1 : tri_index;
}

int rt_init(RTarget *t, int w, int h) {
    if (!t || w <= 0 || h <= 0) return -1;
    t->w = w; t->h = h;
    t->z = (float*)malloc(sizeof(float) * (size_t)w * (size_t)h);
    t->rgb = (unsigned char*)malloc((size_t)w * (size_t)h * 3);
    t->blend = 0;
    t->tris = 0;
    t->err_code = RT_OK;
    t->err_tri = -1;
    t->last_stats.written = 0;
    t->last_stats.blended = 0;
    t->frag = NULL;
    t->frag_cap = 0;
    if (!t->z || !t->rgb) { rt_free(t); return -1; }
    for (int i = 0; i < w * h; ++i) { t->z[i] = 1e30f; t->rgb[i*3+0] = 0; t->rgb[i*3+1] = 0; t->rgb[i*3+2] = 0; }
    return 0;
}

void rt_free(RTarget *t) {
    if (!t) return;
    free(t->z); free(t->rgb); free(t->frag);
    t->z = NULL; t->rgb = NULL; t->frag = NULL;
    t->frag_cap = 0;
    t->w = 0; t->h = 0;
}

int rt_set_blend(RTarget *t, int enabled) {
    if (!t) return RT_ERR_ARG;
    t->blend = enabled ? 1 : 0;
    return RT_OK;
}

const char *rt_error_string(int code) {
    switch (code) {
    case RT_OK: return "ok";
    case RT_ERR_ARG: return "invalid argument";
    case RT_ERR_DEGENERATE: return "degenerate triangle (zero area)";
    case RT_ERR_ALPHA: return "alpha outside [0,1]";
    case RT_ERR_TARGET: return "target has no colour/depth storage";
    case RT_ERR_MEM: return "out of memory for fragment staging";
    default: return "unknown error";
    }
}

static double edge(const RVert *a, const RVert *b, double px, double py) {
    return (b->x - a->x) * (py - a->y) - (b->y - a->y) * (px - a->x);
}

static int ensure_frags(RTarget *t, size_t want) {
    if (want <= t->frag_cap) return 1;
    size_t cap = t->frag_cap ? t->frag_cap : 256;
    while (cap < want) {
        if (cap > ((size_t)-1) / 2) { cap = want; break; }
        cap *= 2;
    }
    RtFrag *p = (RtFrag*)realloc(t->frag, cap * sizeof(RtFrag));
    if (!p) return 0;
    t->frag = p;
    t->frag_cap = cap;
    return 1;
}

/* Pass 1: mix the source colour with the destination colour for a partially
 * transparent fragment (0 < alpha < 1):
 *
 *     out = round(src * a + dst * (1 - a))
 *
 * Fragments that lose the depth test never reach this point, so the
 * destination read here and the destination written in pass 2 are always the
 * same pixel. */
static void mix_colour(const RTri *tri, const unsigned char dst[3], unsigned char out[3]) {
    double a = (double)tri->alpha, ia = 1.0 - a;
    for (int ch = 0; ch < 3; ++ch) {
        double v = (double)tri->rgb[ch] * a + (double)dst[ch] * ia;
        if (v < 0.0) v = 0.0;
        if (v > 255.0) v = 255.0;
        out[ch] = (unsigned char)(v + 0.5);
    }
}

int rt_triangle_at(RTarget *t, const RTri *tri, int tri_index, RtStats *stats) {
    if (stats) { stats->written = 0; stats->blended = 0; }
    if (!t || !tri) { if (t) record_error(t, RT_ERR_ARG, tri_index); return RT_ERR_ARG; }
    if (tri->alpha < 0.0f || tri->alpha > 1.0f || tri->alpha != tri->alpha) {
        record_error(t, RT_ERR_ALPHA, tri_index);
        return RT_ERR_ALPHA;
    }
    if (!t->rgb || !t->z) {
        record_error(t, RT_ERR_TARGET, tri_index);
        return RT_ERR_TARGET;
    }
    double area = edge(&tri->v[0], &tri->v[1], tri->v[2].x, tri->v[2].y);
    if (fabs(area) < 1e-12) {
        record_error(t, RT_ERR_DEGENERATE, tri_index);
        return RT_ERR_DEGENERATE;
    }
    int minx = (int)floor(fmin(fmin(tri->v[0].x, tri->v[1].x), tri->v[2].x));
    int maxx = (int)ceil(fmax(fmax(tri->v[0].x, tri->v[1].x), tri->v[2].x));
    int miny = (int)floor(fmin(fmin(tri->v[0].y, tri->v[1].y), tri->v[2].y));
    int maxy = (int)ceil(fmax(fmax(tri->v[0].y, tri->v[1].y), tri->v[2].y));
    if (minx < 0) { minx = 0; }
    if (miny < 0) { miny = 0; }
    if (maxx >= t->w) maxx = t->w - 1;
    if (maxy >= t->h) maxy = t->h - 1;

    int written = 0, blended = 0;
    int blend = t->blend;

    /* ---- pass 1: coverage + depth test, mix the colour, stage the write ----- */
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
            if ((float)z >= t->z[i]) continue;
            if (!blend || tri->alpha >= 1.0f) {
                /* blending off or fully opaque: plain overwrite */
                if (!ensure_frags(t, (size_t)written + 1)) {
                    record_error(t, RT_ERR_MEM, tri_index);
                    return RT_ERR_MEM;
                }
                RtFrag *f = &t->frag[written];
                f->idx = i;
                f->z = (float)z;
                f->rgb[0] = tri->rgb[0]; f->rgb[1] = tri->rgb[1]; f->rgb[2] = tri->rgb[2];
                ++written;
                continue;
            }
            if (tri->alpha <= 0.0f) continue; /* fully transparent: nothing to write */
            if (!ensure_frags(t, (size_t)written + 1)) {
                record_error(t, RT_ERR_MEM, tri_index);
                return RT_ERR_MEM;
            }
            RtFrag *f = &t->frag[written];
            f->idx = i;
            f->z = (float)z;
            mix_colour(tri, &t->rgb[i*3], f->rgb);
            ++written;
            ++blended;
        }
    }

    /* ---- pass 2: write the staged fragments back --------------------------- */
    for (int k = 0; k < written; ++k) {
        const RtFrag *f = &t->frag[k];
        size_t i = f->idx;
        t->z[i] = f->z;
        t->rgb[i*3+0] = f->rgb[0];
        t->rgb[i*3+1] = f->rgb[1];
        t->rgb[i*3+2] = f->rgb[2];
    }

    record_error(t, RT_OK, -1);
    t->last_stats.written = written;
    t->last_stats.blended = blended;
    if (stats) { stats->written = written; stats->blended = blended; }
    return written;
}

int rt_triangle(RTarget *t, const RTri *tri) {
    if (!t || !tri) return RT_ERR_ARG;
    int id = t->tris;
    int rc = rt_triangle_at(t, tri, id, &t->last_stats);
    if (rc >= 0) ++t->tris;
    return rc;
}

int rt_draw(RTarget *t, const RTri *tris, int n, RtStats *stats) {
    if (stats) { stats->written = 0; stats->blended = 0; }
    if (!t || (!tris && n > 0) || n < 0) {
        if (t) record_error(t, RT_ERR_ARG, -1);
        return RT_ERR_ARG;
    }
    int total_written = 0, total_blended = 0;
    for (int i = 0; i < n; ++i) {
        RtStats s;
        int rc = rt_triangle_at(t, &tris[i], i, &s);
        if (rc < 0) {
            t->last_stats.written = total_written;
            t->last_stats.blended = total_blended;
            if (stats) { stats->written = total_written; stats->blended = total_blended; }
            return rc; /* t->err_tri already names triangle i */
        }
        total_written += s.written;
        total_blended += s.blended;
        ++t->tris;
    }
    t->last_stats.written = total_written;
    t->last_stats.blended = total_blended;
    if (stats) { stats->written = total_written; stats->blended = total_blended; }
    return total_written;
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
