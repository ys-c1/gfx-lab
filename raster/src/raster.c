#define _POSIX_C_SOURCE 200809L
#include "raster.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int rt_init(RTarget *t, int w, int h) {
    if (!t || w <= 0 || h <= 0) return -1;
    t->w = w; t->h = h;
    t->stencil_enabled = 0;
    t->phase = RT_STENCIL_PHASE_NONE;
    t->z = (float*)malloc(sizeof(float) * (size_t)w * (size_t)h);
    t->rgb = (unsigned char*)malloc((size_t)w * (size_t)h * 3);
    t->stencil = (unsigned char*)calloc((size_t)w * (size_t)h, 1);
    t->pass = (unsigned char*)calloc((size_t)w * (size_t)h, 1);
    if (!t->z || !t->rgb || !t->stencil || !t->pass) { rt_free(t); return -1; }
    for (int i = 0; i < w * h; ++i) { t->z[i] = 1e30f; t->rgb[i*3+0] = 0; t->rgb[i*3+1] = 0; t->rgb[i*3+2] = 0; }
    return 0;
}

void rt_free(RTarget *t) {
    if (!t) return;
    free(t->z); free(t->rgb); free(t->stencil); free(t->pass);
    t->z = NULL; t->rgb = NULL; t->stencil = NULL; t->pass = NULL;
    t->w = 0; t->h = 0; t->stencil_enabled = 0; t->phase = RT_STENCIL_PHASE_NONE;
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

int rt_write_ppm(const RTarget *t, const char *path) {
    if (!t || !path) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) return -2;
    fprintf(f, "P6\n%d %d\n255\n", t->w, t->h);
    fwrite(t->rgb, 1, (size_t)t->w * (size_t)t->h * 3, f);
    fclose(f);
    return 0;
}

/* ------------------------------ stencil ------------------------------ */

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
}

static int stencil_fail(RTStencilErrorInfo *err, RTStencilError code,
                        const char *param, int index, int x, int y,
                        double value) {
    if (err) {
        err->code = code;
        err->param = param;
        err->index = index;
        err->x = x;
        err->y = y;
        err->value = value;
    }
    return -(int)code;
}

static void stencil_ok(RTStencilErrorInfo *err) {
    if (err) {
        err->code = RT_STENCIL_ERR_NONE;
        err->param = NULL;
        err->index = -1;
        err->x = -1;
        err->y = -1;
        err->value = 0.0;
    }
}

/* Lowest set bit of `v` that lies outside `mask`, or -1 if none. */
static int first_bit_outside(unsigned int v, unsigned int mask) {
    unsigned int bad = v & ~mask & 0xFFu;
    for (int b = 0; b < 8; ++b)
        if (bad & (1u << b)) return b;
    return -1;
}

int rt_stencil_enable(RTarget *t, int enabled) {
    if (!t || !t->stencil) return -(int)RT_STENCIL_ERR_NULL;
    t->stencil_enabled = enabled ? 1 : 0;
    if (!t->stencil_enabled) t->phase = RT_STENCIL_PHASE_NONE;
    return 0;
}

int rt_stencil_enabled(const RTarget *t) { return t ? t->stencil_enabled : 0; }

/* Fill stats->hist/min/max from the current stencil buffer. */
static void stencil_summarise(const RTarget *t, RTStencilStats *stats) {
    memset(stats->hist, 0, sizeof stats->hist);
    stats->min_value = 255;
    stats->max_value = 0;
    size_t n = (size_t)t->w * (size_t)t->h;
    for (size_t i = 0; i < n; ++i) {
        unsigned char v = t->stencil[i];
        ++stats->hist[v];
        if ((int)v < stats->min_value) stats->min_value = (int)v;
        if ((int)v > stats->max_value) stats->max_value = (int)v;
    }
}

static void stats_begin(RTStencilStats *stats, const RTarget *t) {
    if (!stats) return;
    memset(stats, 0, sizeof *stats);
    stats->pixels = t->w * t->h;
}

int rt_stencil_clear(RTarget *t, unsigned char value, RTStencilErrorInfo *err) {
    if (!t || !t->stencil || !t->pass) return stencil_fail(err, RT_STENCIL_ERR_NULL, "target", -1, -1, -1, 0.0);
    if (!t->stencil_enabled) { stencil_ok(err); return 0; }
    size_t n = (size_t)t->w * (size_t)t->h;
    memset(t->stencil, value, n);
    memset(t->pass, 0, n);
    t->phase = RT_STENCIL_PHASE_NONE;
    stencil_ok(err);
    return 0;
}

static int func_holds(RTStencilFunc f, unsigned int ref, unsigned int v) {
    switch (f) {
    case RT_STENCIL_FUNC_NEVER: return 0;
    case RT_STENCIL_FUNC_LESS: return v < ref;
    case RT_STENCIL_FUNC_EQUAL: return v == ref;
    case RT_STENCIL_FUNC_LEQUAL: return v <= ref;
    case RT_STENCIL_FUNC_GREATER: return v > ref;
    case RT_STENCIL_FUNC_NOTEQUAL: return v != ref;
    case RT_STENCIL_FUNC_GEQUAL: return v >= ref;
    case RT_STENCIL_FUNC_ALWAYS: return 1;
    default: return 0;
    }
}

int rt_stencil_write(RTarget *t, const RTStencilShape *shape,
                     RTStencilStats *stats, RTStencilErrorInfo *err) {
    if (!t || !t->stencil || !t->pass) return stencil_fail(err, RT_STENCIL_ERR_NULL, "target", -1, -1, -1, 0.0);
    stats_begin(stats, t);
    if (!t->stencil_enabled) { stencil_ok(err); return 0; }
    if (!shape) return stencil_fail(err, RT_STENCIL_ERR_NULL, "shape", -1, -1, -1, 0.0);
    if (!isfinite(shape->cx) || !isfinite(shape->cy) || !isfinite(shape->radius))
        return stencil_fail(err, RT_STENCIL_ERR_GEOMETRY, "shape", -1, -1, -1, 0.0);
    if (!(shape->radius > 0.0))
        return stencil_fail(err, RT_STENCIL_ERR_GEOMETRY, "radius", -1, -1, -1, shape->radius);
    if (shape->write_mask == 0)
        return stencil_fail(err, RT_STENCIL_ERR_BAD_MASK, "write_mask", -1, -1, -1, 0.0);
    int bad = first_bit_outside(shape->value, shape->write_mask);
    if (bad >= 0)
        return stencil_fail(err, RT_STENCIL_ERR_BAD_VALUE, "value", bad,
                            -1, -1, (double)shape->value);

    int covered = 0, written = 0;
    double r2 = shape->radius * shape->radius;
    unsigned char wm = shape->write_mask;
    unsigned char v = shape->value;
    double t0 = now_ms();
    for (int y = 0; y < t->h; ++y) {
        double dy = ((double)y + 0.5) - shape->cy;
        for (int x = 0; x < t->w; ++x) {
            double dx = ((double)x + 0.5) - shape->cx;
            if (dx * dx + dy * dy > r2) continue;
            ++covered;
            size_t i = (size_t)y * (size_t)t->w + (size_t)x;
            unsigned char before = t->stencil[i];
            unsigned char after = (unsigned char)((before & (unsigned char)~wm) | (v & wm));
            t->stencil[i] = after;
            if (after != before) ++written;
        }
    }
    t->phase = RT_STENCIL_PHASE_WRITE;
    if (stats) {
        stats->covered = covered;
        stats->written = written;
        stats->elapsed_ms = now_ms() - t0;
        stencil_summarise(t, stats);
    }
    stencil_ok(err);
    return 0;
}

int rt_stencil_test(RTarget *t, const RTStencilTest *test,
                    RTStencilStats *stats, RTStencilErrorInfo *err) {
    if (!t || !t->stencil || !t->pass) return stencil_fail(err, RT_STENCIL_ERR_NULL, "target", -1, -1, -1, 0.0);
    stats_begin(stats, t);
    if (!t->stencil_enabled) { stencil_ok(err); return 0; }
    if (!test) return stencil_fail(err, RT_STENCIL_ERR_NULL, "test", -1, -1, -1, 0.0);
    if ((int)test->func < 0 || (int)test->func >= RT_STENCIL_FUNC_COUNT)
        return stencil_fail(err, RT_STENCIL_ERR_BAD_FUNC, "func", -1, -1, -1, (double)test->func);
    if (test->read_mask == 0)
        return stencil_fail(err, RT_STENCIL_ERR_BAD_MASK, "read_mask", -1, -1, -1, 0.0);
    int bad = first_bit_outside(test->ref, test->read_mask);
    if (bad >= 0)
        return stencil_fail(err, RT_STENCIL_ERR_BAD_VALUE, "ref", bad,
                            -1, -1, (double)test->ref);
    if (t->phase != RT_STENCIL_PHASE_WRITE && t->phase != RT_STENCIL_PHASE_TEST)
        return stencil_fail(err, RT_STENCIL_ERR_STATE, "phase", -1, -1, -1, (double)t->phase);

    double t0 = now_ms();
    unsigned char rm = test->read_mask;
    unsigned int ref = test->ref & rm;
    size_t n = (size_t)t->w * (size_t)t->h;
    int passes = 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned int v = t->stencil[i] & rm;
        unsigned char ok = (unsigned char)(func_holds(test->func, ref, v) ? 1 : 0);
        t->pass[i] = ok;
        passes += ok;
    }
    t->phase = RT_STENCIL_PHASE_TEST;
    if (stats) {
        stats->tested = (int)n;
        stats->passes = passes;
        stats->elapsed_ms = now_ms() - t0;
        stencil_summarise(t, stats);
    }
    stencil_ok(err);
    return 0;
}

int rt_stencil_shade(RTarget *t, const RTStencilShade *shade,
                     RTStencilStats *stats, RTStencilErrorInfo *err) {
    if (!t || !t->stencil || !t->pass) return stencil_fail(err, RT_STENCIL_ERR_NULL, "target", -1, -1, -1, 0.0);
    stats_begin(stats, t);
    if (!t->stencil_enabled) { stencil_ok(err); return 0; }
    if (!shade) return stencil_fail(err, RT_STENCIL_ERR_NULL, "shade", -1, -1, -1, 0.0);
    if (!isfinite(shade->z))
        return stencil_fail(err, RT_STENCIL_ERR_RANGE, "z", -1, -1, -1, shade->z);
    if (t->phase != RT_STENCIL_PHASE_TEST)
        return stencil_fail(err, RT_STENCIL_ERR_STATE, "phase", -1, -1, -1, (double)t->phase);

    double t0 = now_ms();
    size_t n = (size_t)t->w * (size_t)t->h;
    int shaded = 0;
    for (size_t i = 0; i < n; ++i) {
        if (!t->pass[i]) continue;
        t->z[i] = (float)shade->z;
        t->rgb[i*3+0] = shade->rgb[0];
        t->rgb[i*3+1] = shade->rgb[1];
        t->rgb[i*3+2] = shade->rgb[2];
        ++shaded;
    }
    /* Shading is terminal for this cycle; a further shade must be preceded
     * by a new test (which re-derives the pass set). */
    t->phase = RT_STENCIL_PHASE_SHADE;
    if (stats) {
        stats->shaded = shaded;
        stats->elapsed_ms = now_ms() - t0;
        stencil_summarise(t, stats);
    }
    stencil_ok(err);
    return 0;
}

/* ------------------------- direct / bit access ------------------------ */

static int check_pixel(const RTarget *t, int x, int y, RTStencilErrorInfo *err,
                       const char *what) {
    if (!t || !t->stencil || !t->pass)
        return stencil_fail(err, RT_STENCIL_ERR_NULL, "target", -1, -1, -1, 0.0);
    if (x < 0 || y < 0 || x >= t->w || y >= t->h)
        return stencil_fail(err, RT_STENCIL_ERR_INDEX, what, -1, x, y, 0.0);
    return 0;
}

int rt_stencil_get(const RTarget *t, int x, int y, unsigned char *out,
                   RTStencilErrorInfo *err) {
    int rc = check_pixel(t, x, y, err, "x/y");
    if (rc != 0) return rc;
    if (!out) return stencil_fail(err, RT_STENCIL_ERR_NULL, "out", -1, x, y, 0.0);
    /* While disabled there is no stencil state; reads report zero rather than
     * exposing whatever bytes the buffer happens to hold. */
    *out = t->stencil_enabled ? t->stencil[(size_t)y * (size_t)t->w + (size_t)x] : 0;
    stencil_ok(err);
    return 0;
}

int rt_stencil_pass_at(const RTarget *t, int x, int y, unsigned char *out,
                       RTStencilErrorInfo *err) {
    int rc = check_pixel(t, x, y, err, "x/y");
    if (rc != 0) return rc;
    if (!out) return stencil_fail(err, RT_STENCIL_ERR_NULL, "out", -1, x, y, 0.0);
    if (!t->stencil_enabled) {
        *out = 0;
        stencil_ok(err);
        return 0;
    }
    if (t->phase != RT_STENCIL_PHASE_TEST && t->phase != RT_STENCIL_PHASE_SHADE)
        return stencil_fail(err, RT_STENCIL_ERR_STATE, "phase", -1, x, y, (double)t->phase);
    *out = t->pass[(size_t)y * (size_t)t->w + (size_t)x];
    stencil_ok(err);
    return 0;
}

static int check_bits(const RTarget *t, const unsigned char *out, int out_bytes,
                      RTStencilErrorInfo *err, const char *param) {
    if (!t || !t->stencil || !t->pass)
        return stencil_fail(err, RT_STENCIL_ERR_NULL, "target", -1, -1, -1, 0.0);
    if (!out) return stencil_fail(err, RT_STENCIL_ERR_NULL, param, -1, -1, -1, 0.0);
    int need = (t->w * t->h + 7) / 8;
    if (out_bytes != need)
        return stencil_fail(err, RT_STENCIL_ERR_RANGE, "out_bytes", -1, -1, -1, (double)out_bytes);
    return 0;
}

int rt_stencil_export_values(const RTarget *t, unsigned char *out, int n,
                             RTStencilErrorInfo *err) {
    if (!t || !t->stencil)
        return stencil_fail(err, RT_STENCIL_ERR_NULL, "target", -1, -1, -1, 0.0);
    int need = t->w * t->h;
    if (n != need) return stencil_fail(err, RT_STENCIL_ERR_RANGE, "n", -1, -1, -1, (double)n);
    if (!out) return stencil_fail(err, RT_STENCIL_ERR_NULL, "out", -1, -1, -1, 0.0);
    if (!t->stencil_enabled) {
        memset(out, 0, (size_t)need);
        stencil_ok(err);
        return 0;
    }
    memcpy(out, t->stencil, (size_t)need);
    stencil_ok(err);
    return 0;
}

int rt_stencil_export_pass_bits(const RTarget *t, unsigned char *out,
                                int out_bytes, RTStencilErrorInfo *err) {
    int rc = check_bits(t, out, out_bytes, err, "out");
    if (rc != 0) return rc;
    if (!t->stencil_enabled) {
        memset(out, 0, (size_t)out_bytes);
        stencil_ok(err);
        return 0;
    }
    if (t->phase != RT_STENCIL_PHASE_TEST && t->phase != RT_STENCIL_PHASE_SHADE)
        return stencil_fail(err, RT_STENCIL_ERR_STATE, "phase", -1, -1, -1, (double)t->phase);
    size_t n = (size_t)t->w * (size_t)t->h;
    memset(out, 0, (size_t)out_bytes);
    for (size_t i = 0; i < n; ++i)
        if (t->pass[i]) out[i >> 3] |= (unsigned char)(1u << (i & 7));
    stencil_ok(err);
    return 0;
}

int rt_stencil_export_value_bits(const RTarget *t, int bit,
                                 unsigned char *out, int out_bytes,
                                 RTStencilErrorInfo *err) {
    int rc = check_bits(t, out, out_bytes, err, "out");
    if (rc != 0) return rc;
    if (bit < 0 || bit > 7)
        return stencil_fail(err, RT_STENCIL_ERR_RANGE, "bit", -1, -1, -1, (double)bit);
    if (!t->stencil_enabled) {
        memset(out, 0, (size_t)out_bytes);
        stencil_ok(err);
        return 0;
    }
    size_t n = (size_t)t->w * (size_t)t->h;
    memset(out, 0, (size_t)out_bytes);
    for (size_t i = 0; i < n; ++i)
        if (t->stencil[i] & (unsigned char)(1u << bit))
            out[i >> 3] |= (unsigned char)(1u << (i & 7));
    stencil_ok(err);
    return 0;
}

const char *rt_stencil_error_name(RTStencilError e) {
    switch (e) {
    case RT_STENCIL_ERR_NONE: return "none";
    case RT_STENCIL_ERR_NULL: return "null-argument";
    case RT_STENCIL_ERR_GEOMETRY: return "bad-geometry";
    case RT_STENCIL_ERR_BAD_FUNC: return "bad-function";
    case RT_STENCIL_ERR_BAD_MASK: return "bad-mask";
    case RT_STENCIL_ERR_BAD_VALUE: return "bad-value";
    case RT_STENCIL_ERR_STATE: return "bad-phase";
    case RT_STENCIL_ERR_INDEX: return "index-out-of-range";
    case RT_STENCIL_ERR_RANGE: return "out-of-range";
    default: return "unknown";
    }
}

const char *rt_stencil_phase_name(int phase) {
    switch (phase) {
    case RT_STENCIL_PHASE_NONE: return "none";
    case RT_STENCIL_PHASE_WRITE: return "write";
    case RT_STENCIL_PHASE_TEST: return "test";
    case RT_STENCIL_PHASE_SHADE: return "shade";
    default: return "unknown";
    }
}
