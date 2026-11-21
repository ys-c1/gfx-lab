#include "raster.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The fill pass and the wireframe overlay are independent state.  The
 * overlay is built into its own buffers (layer masks, then a merged
 * overlay image) and only stage 3 writes t->rgb.  Consequently a fill
 * drawn at any time can never cover the wireframe: the overlay does not
 * exist in the framebuffer until resolve time. */

int rt_init(RTarget *t, int w, int h) {
    if (!t || w <= 0 || h <= 0) return -1;
    memset(t, 0, sizeof *t);
    t->w = w; t->h = h;
    t->z = (float*)malloc(sizeof(float) * (size_t)w * (size_t)h);
    t->rgb = (unsigned char*)malloc((size_t)w * (size_t)h * 3);
    if (!t->z || !t->rgb) { rt_free(t); return -1; }
    for (int i = 0; i < w * h; ++i) {
        t->z[i] = 1e30f;
        t->rgb[i*3+0] = 0; t->rgb[i*3+1] = 0; t->rgb[i*3+2] = 0;
    }
    return 0;
}

void rt_wire_clear(RTarget *t) {
    if (!t) return;
    for (int i = 0; i < t->layer_count; ++i) free(t->layers[i].mask);
    t->layer_count = 0;
    t->layer_seq = 0;
    free(t->ov_cov); t->ov_cov = NULL;
    free(t->ov_pm);  t->ov_pm = NULL;
    free(t->ov_a);   t->ov_a = NULL;
    t->ov_staged = 0;
}

void rt_free(RTarget *t) {
    if (!t) return;
    rt_wire_clear(t);
    free(t->z); free(t->rgb);
    t->z = NULL; t->rgb = NULL;
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

/* ================= wireframe overlay ================= */

/* Stage 1 primitive.  The same edge function the fill pass uses is
 * evaluated at the pixel centre:
 *
 *     e(p) = (b-a) x (p-a) = (b.x-a.x)*(p.y-a.y) - (b.y-a.y)*(p.x-a.x)
 *
 * and |e| / |ab| is the perpendicular distance from the centre to the
 * edge's line.  The projection parameter t decides whether the nearest
 * point lies on the edge itself; when it does not, the distance to the
 * corresponding endpoint is used, so both ends are capped (round caps)
 * instead of the edge being extended as an infinite line.  This is why
 * coincident vertices and short edges have to be rejected up front. */
int rt_wire_edge_dist(const RVert *a, const RVert *b, int x, int y, double *dist) {
    if (!a || !b || !dist || x < 0 || y < 0) return RT_WIRE_ERR_ARG;
    double px = (double)x + 0.5, py = (double)y + 0.5;
    double dx = b->x - a->x, dy = b->y - a->y;
    double len2 = dx * dx + dy * dy;
    if (!(len2 > 0.0)) return RT_WIRE_ERR_DUP_VERTEX; /* also catches NaN */
    double t = ((px - a->x) * dx + (py - a->y) * dy) / len2;
    if (t <= 0.0) {
        *dist = hypot(px - a->x, py - a->y);
    } else if (t >= 1.0) {
        *dist = hypot(px - b->x, py - b->y);
    } else {
        double e = edge(a, b, px, py);
        *dist = fabs(e) / sqrt(len2);
    }
    return RT_WIRE_OK;
}

const char *rt_wire_strerror(int code) {
    switch (code) {
    case RT_WIRE_OK: return "ok";
    case RT_WIRE_ERR_ARG: return "null or out-of-range argument";
    case RT_WIRE_ERR_LAYER: return "layer number must be >= 0";
    case RT_WIRE_ERR_OPACITY: return "opacity must be finite and within [0,1]";
    case RT_WIRE_ERR_HALFWIDTH: return "wire half width must be finite and >= 0";
    case RT_WIRE_ERR_VERTEX: return "vertex coordinate is not finite";
    case RT_WIRE_ERR_DUP_VERTEX: return "coincident vertices";
    case RT_WIRE_ERR_SHORT_EDGE: return "edge shorter than RT_WIRE_MIN_EDGE";
    case RT_WIRE_ERR_CAPACITY: return "all overlay layer slots are in use";
    case RT_WIRE_ERR_MEM: return "out of memory";
    default: return "unknown wireframe error";
    }
}

/* Formats the full location: which layer, which stored slot, which edge
 * and which field.  Returns 0 on success, -2 if cap is too small. */
int rt_wire_errfmt(const RWireErr *err, char *buf, size_t cap) {
    if (!err || !buf || cap == 0) return -1;
    char where[96];
    if (err->layer_slot >= 0 && err->layer >= 0)
        snprintf(where, sizeof where, "layer %d (slot %d)", err->layer, err->layer_slot);
    else if (err->layer >= 0)
        snprintf(where, sizeof where, "layer %d", err->layer);
    else if (err->layer_slot >= 0)
        snprintf(where, sizeof where, "slot %d", err->layer_slot);
    else
        snprintf(where, sizeof where, "-");
    const char *field = (err->field && err->field[0]) ? err->field : "-";
    int n;
    if (err->edge >= 0)
        n = snprintf(buf, cap, "%s: %s; %s; edge %d (v%d-v%d)", where, field,
                     rt_wire_strerror(err->code), err->edge, err->vertex, err->vertex2);
    else
        n = snprintf(buf, cap, "%s: %s; %s", where, field, rt_wire_strerror(err->code));
    return (n < 0) ? -1 : (n >= (int)cap ? -2 : 0);
}

static int fail(RWireErr *err, int code, const char *field, int slot, int layer,
                int e, int v, int v2) {
    if (err) {
        err->code = code;
        err->field = field;
        err->layer_slot = slot;
        err->layer = layer;
        err->edge = e;
        err->vertex = v;
        err->vertex2 = v2;
    }
    return code;
}

/* Validation of a layer spec and its triangle.  Runs before any storage
 * is allocated, so a rejected layer leaves the target exactly as it was. */
static int stroke_check(const RTri *tri, const RWireSpec *spec, RWireErr *err) {
    if (!tri || !spec) return fail(err, RT_WIRE_ERR_ARG, "triangle", -1, -1, -1, -1, -1);
    if (spec->layer < 0)
        return fail(err, RT_WIRE_ERR_LAYER, "layer", -1, spec->layer, -1, -1, -1);
    if (!isfinite(spec->opacity) || spec->opacity < 0.0f || spec->opacity > 1.0f)
        return fail(err, RT_WIRE_ERR_OPACITY, "opacity", -1, spec->layer, -1, -1, -1);
    if (!isfinite(spec->halfwidth) || spec->halfwidth < 0.0)
        return fail(err, RT_WIRE_ERR_HALFWIDTH, "halfwidth", -1, spec->layer, -1, -1, -1);
    for (int i = 0; i < 3; ++i) {
        if (!isfinite(tri->v[i].x) || !isfinite(tri->v[i].y))
            return fail(err, RT_WIRE_ERR_VERTEX, "vertex", -1, spec->layer, -1, i, -1);
    }
    for (int i = 0; i < 3; ++i) {
        int j = (i + 1) % 3;
        double dx = tri->v[j].x - tri->v[i].x;
        double dy = tri->v[j].y - tri->v[i].y;
        double len = sqrt(dx * dx + dy * dy);
        if (len == 0.0)
            return fail(err, RT_WIRE_ERR_DUP_VERTEX, "edge", -1, spec->layer, i, i, j);
        if (len < RT_WIRE_MIN_EDGE)
            return fail(err, RT_WIRE_ERR_SHORT_EDGE, "edge", -1, spec->layer, i, i, j);
    }
    return RT_WIRE_OK;
}

/* Stage 1: stroke.  The bounding box is the triangle's box grown by
 * ceil(halfwidth)+1, in double before any int cast so a huge half width
 * cannot overflow.  Nothing outside the target is visited. */
int rt_wire_stroke(RTarget *t, const RTri *tri, const RWireSpec *spec,
                   RWireStrokeStats *stats, RWireErr *err) {
    if (stats) {
        stats->layer_slot = -1; stats->layer = -1; stats->opacity = 0.0f;
        stats->covered = 0; stats->per_edge[0] = stats->per_edge[1] = stats->per_edge[2] = 0;
    }
    if (err) {
        err->code = RT_WIRE_OK; err->field = ""; err->layer_slot = -1; err->layer = -1;
        err->edge = -1; err->vertex = -1; err->vertex2 = -1;
    }
    if (!t || !spec) return fail(err, RT_WIRE_ERR_ARG, "spec", -1, -1, -1, -1, -1);
    /* Disabled: do not validate, do not allocate, do not touch a byte. */
    if (!spec->enabled) return RT_WIRE_OK;

    int rc = stroke_check(tri, spec, err);
    if (rc != RT_WIRE_OK) return rc;
    if (t->layer_count >= RT_WIRE_MAX_LAYERS)
        return fail(err, RT_WIRE_ERR_CAPACITY, "layers", -1, spec->layer, -1, -1, -1);

    unsigned char *mask = (unsigned char*)calloc((size_t)t->w * (size_t)t->h, 1);
    if (!mask) return fail(err, RT_WIRE_ERR_MEM, "mask", -1, spec->layer, -1, -1, -1);

    double margin = ceil(spec->halfwidth) + 1.0;
    double dminx = fmin(fmin(tri->v[0].x, tri->v[1].x), tri->v[2].x) - margin;
    double dmaxx = fmax(fmax(tri->v[0].x, tri->v[1].x), tri->v[2].x) + margin;
    double dminy = fmin(fmin(tri->v[0].y, tri->v[1].y), tri->v[2].y) - margin;
    double dmaxy = fmax(fmax(tri->v[0].y, tri->v[1].y), tri->v[2].y) + margin;

    int per_edge[3] = {0, 0, 0}, covered = 0;
    int minx = 0, miny = 0, maxx = -1, maxy = -1;
    if (!(dmaxx < 0.0 || dmaxy < 0.0 || dminx > (double)(t->w - 1) ||
          dminy > (double)(t->h - 1))) {
        int bx0 = dminx < 0.0 ? 0 : (int)floor(dminx);
        int bx1 = dmaxx > (double)(t->w - 1) ? t->w - 1 : (int)floor(dmaxx);
        int by0 = dminy < 0.0 ? 0 : (int)floor(dminy);
        int by1 = dmaxy > (double)(t->h - 1) ? t->h - 1 : (int)floor(dmaxy);
        for (int y = by0; y <= by1; ++y) {
            for (int x = bx0; x <= bx1; ++x) {
                int hit = 0;
                for (int i = 0; i < 3; ++i) {
                    double d;
                    rt_wire_edge_dist(&tri->v[i], &tri->v[(i + 1) % 3], x, y, &d);
                    if (d <= spec->halfwidth) { ++per_edge[i]; hit = 1; }
                }
                if (!hit) continue;
                mask[(size_t)y * t->w + x] = 1;
                ++covered;
                if (maxx < 0) { minx = maxx = x; miny = maxy = y; }
                else {
                    if (x < minx) minx = x;
                    if (x > maxx) maxx = x;
                    if (y < miny) miny = y;
                    if (y > maxy) maxy = y;
                }
            }
        }
    }

    RTWireLayer *L = &t->layers[t->layer_count];
    L->layer = spec->layer;
    L->opacity = spec->opacity;
    L->seq = t->layer_seq++;
    L->rgb[0] = spec->rgb[0]; L->rgb[1] = spec->rgb[1]; L->rgb[2] = spec->rgb[2];
    L->covered = covered;
    L->per_edge[0] = per_edge[0]; L->per_edge[1] = per_edge[1]; L->per_edge[2] = per_edge[2];
    L->minx = minx; L->miny = miny; L->maxx = maxx; L->maxy = maxy;
    L->mask = mask;
    /* Any merge built before this stroke is stale now. */
    free(t->ov_cov); t->ov_cov = NULL;
    free(t->ov_pm);  t->ov_pm = NULL;
    free(t->ov_a);   t->ov_a = NULL;
    t->ov_staged = 0;

    int slot = t->layer_count++;
    if (stats) {
        stats->layer_slot = slot;
        stats->layer = spec->layer;
        stats->opacity = spec->opacity;
        stats->covered = covered;
        stats->per_edge[0] = per_edge[0];
        stats->per_edge[1] = per_edge[1];
        stats->per_edge[2] = per_edge[2];
    }
    return covered;
}

/* Composite order: ascending layer number; equal layer numbers keep
 * stroke order, so replaying strokes reproduces the same image. */
static int order_cmp(const void *pa, const void *pb, void *ctx) {
    const RTWireLayer *L = (const RTWireLayer*)ctx;
    int a = *(const int*)pa, b = *(const int*)pb;
    if (L[a].layer != L[b].layer) return L[a].layer < L[b].layer ? -1 : 1;
    if (L[a].seq != L[b].seq) return L[a].seq < L[b].seq ? -1 : 1;
    return a < b ? -1 : 1;
}

/* Insertion sort over at most RT_WIRE_MAX_LAYERS keys; deterministic and
 * dependency-free (qsort_r is not portable in C11). */
static void order_sort(int *keys, int n, const RTWireLayer *L) {
    for (int i = 1; i < n; ++i) {
        int k = keys[i], j = i - 1;
        while (j >= 0 && order_cmp(&keys[j], &k, (void*)L) > 0) {
            keys[j + 1] = keys[j];
            --j;
        }
        keys[j + 1] = k;
    }
}

/* Stage 2: merge.  Source-over in the ordered layer list:
 *
 *   a_out = a_src + a_dst * (1 - a_src)
 *   c_out = c_src * a_src + c_dst * (1 - a_src)
 *
 * c is premultiplied, so the composite stays correct for any ordering
 * and never needs an un-premultiply step.  Everything is done in double;
 * the work is limited to the union bounding box of the layers. */
int rt_wire_merge(RTarget *t, RWireMergeStats *stats, RWireErr *err) {
    if (stats) memset(stats, 0, sizeof *stats);
    if (err) {
        err->code = RT_WIRE_OK; err->field = ""; err->layer_slot = -1; err->layer = -1;
        err->edge = -1; err->vertex = -1; err->vertex2 = -1;
    }
    if (!t) return fail(err, RT_WIRE_ERR_ARG, "target", -1, -1, -1, -1, -1);

    free(t->ov_cov); t->ov_cov = NULL;
    free(t->ov_pm);  t->ov_pm = NULL;
    free(t->ov_a);   t->ov_a = NULL;
    t->ov_staged = 0;
    if (t->layer_count == 0) return RT_WIRE_OK;

    size_t n = (size_t)t->w * (size_t)t->h;
    t->ov_cov = (unsigned char*)calloc(n, 1);
    t->ov_pm = (double*)calloc(n * 3, sizeof(double));
    t->ov_a = (double*)calloc(n, sizeof(double));
    if (!t->ov_cov || !t->ov_pm || !t->ov_a) {
        free(t->ov_cov); t->ov_cov = NULL;
        free(t->ov_pm);  t->ov_pm = NULL;
        free(t->ov_a);   t->ov_a = NULL;
        return fail(err, RT_WIRE_ERR_MEM, "overlay", -1, -1, -1, -1, -1);
    }

    int keys[RT_WIRE_MAX_LAYERS];
    for (int i = 0; i < t->layer_count; ++i) keys[i] = i;
    order_sort(keys, t->layer_count, t->layers);

    if (stats) {
        stats->layers = t->layer_count;
        for (int k = 0; k < t->layer_count; ++k) {
            int s = keys[k];
            stats->order[k] = s;
            stats->order_layer[k] = t->layers[s].layer;
            stats->order_opacity[k] = t->layers[s].opacity;
            stats->order_covered[k] = t->layers[s].covered;
        }
    }

    for (int k = 0; k < t->layer_count; ++k) {
        const RTWireLayer *L = &t->layers[keys[k]];
        if (L->covered == 0) continue;
        double a_src = (double)L->opacity;
        double c_src[3] = {(double)L->rgb[0], (double)L->rgb[1], (double)L->rgb[2]};
        double keep = 1.0 - a_src;
        for (int y = L->miny; y <= L->maxy; ++y) {
            const unsigned char *mrow = L->mask + (size_t)y * t->w;
            for (int x = L->minx; x <= L->maxx; ++x) {
                if (!mrow[x]) continue;
                size_t i = (size_t)y * t->w + x;
                double a_dst = t->ov_a[i];
                double a_out = a_src + a_dst * keep;
                for (int c = 0; c < 3; ++c)
                    t->ov_pm[i * 3 + c] = c_src[c] * a_src + t->ov_pm[i * 3 + c] * keep;
                t->ov_a[i] = a_out;
                t->ov_cov[i] = 1;
            }
        }
    }

    if (stats) {
        for (size_t i = 0; i < n; ++i) {
            if (!t->ov_cov[i]) continue;
            ++stats->covered;
            if (t->ov_a[i] >= 1.0 - 1e-12) ++stats->opaque;
        }
        for (int k = 0; k < t->layer_count; ++k) {
            const RTWireLayer *L = &t->layers[keys[k]];
            if (L->covered) stats->contributions += L->covered;
        }
    }
    t->ov_staged = 1;
    return 0;
}

/* Stage 3: blend premultiplied overlay over the frame.  Depth is never
 * read or written, so the fill pass result is untouched below the
 * overlay.  A staged overlay is consumed on success so a second call
 * cannot blend it twice. */
int rt_wire_blend(RTarget *t, RWireBlendStats *stats, RWireErr *err) {
    if (stats) { stats->blended = 0; stats->replaced = 0; }
    if (err) {
        err->code = RT_WIRE_OK; err->field = ""; err->layer_slot = -1; err->layer = -1;
        err->edge = -1; err->vertex = -1; err->vertex2 = -1;
    }
    if (!t) return fail(err, RT_WIRE_ERR_ARG, "target", -1, -1, -1, -1, -1);
    if (!t->ov_staged || !t->ov_a) return RT_WIRE_OK; /* nothing merged */

    size_t n = (size_t)t->w * (size_t)t->h;
    int blended = 0, replaced = 0;
    for (size_t i = 0; i < n; ++i) {
        double a = t->ov_a[i];
        if (!(a > 0.0)) continue;
        double keep = 1.0 - a;
        for (int c = 0; c < 3; ++c) {
            double v = t->ov_pm[i * 3 + c] + (double)t->rgb[i * 3 + c] * keep;
            if (v < 0.0) v = 0.0;
            if (v > 255.0) v = 255.0;
            t->rgb[i * 3 + c] = (unsigned char)(v + 0.5);
        }
        ++blended;
        if (a >= 1.0 - 1e-12) ++replaced;
    }
    free(t->ov_cov); t->ov_cov = NULL;
    free(t->ov_pm);  t->ov_pm = NULL;
    free(t->ov_a);   t->ov_a = NULL;
    t->ov_staged = 0;
    if (stats) { stats->blended = blended; stats->replaced = replaced; }
    return 0;
}

int rt_wire_resolve(RTarget *t, RWireResolveStats *stats, RWireErr *err) {
    if (stats) { memset(&stats->merge, 0, sizeof stats->merge);
                 stats->blend.blended = stats->blend.replaced = 0; }
    int rc = rt_wire_merge(t, stats ? &stats->merge : NULL, err);
    if (rc != RT_WIRE_OK) return rc;
    return rt_wire_blend(t, stats ? &stats->blend : NULL, err);
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
