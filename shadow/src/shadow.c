#include "shadow.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* Which part of a segment is bad; reported as "segs[i].<name>". */
enum {
    SH_SEG_X0 = 1,
    SH_SEG_Y0,
    SH_SEG_X1,
    SH_SEG_Y1,
    SH_SEG_SLOPE
};
static const char *const SEG_FIELD[] = { "", "x0", "y0", "x1", "y1", "slope" };

static int finite_positive(float v) { return isfinite(v) && v > 0.0f; }

static void clear_error(ShError *err) {
    if (!err) return;
    err->code = 0;
    err->field[0] = '\0';
}

static int fail(ShError *err, int code, const char *field) {
    if (err) {
        err->code = code;
        snprintf(err->field, sizeof err->field, "%s", field);
    }
    return code;
}

static int seg_fail(ShError *err, int index, int which) {
    if (err) {
        err->code = SH_ERR_FIELD;
        snprintf(err->field, sizeof err->field, "segs[%d].%s",
                 index, SEG_FIELD[which]);
    }
    return SH_ERR_FIELD;
}

static int map_ok(const ShadowMap *m) { return m && finite_positive(m->scale); }

/* --------------------------------------------------------------- lifecycle */

int sh_init(ShadowMap *m, int w, int h) {
    return sh_init_scaled(m, w, h, SH_DEFAULT_SCALE);
}

int sh_init_scaled(ShadowMap *m, int w, int h, float scale) {
    if (!m || w <= 0 || h <= 0) return SH_ERR_ARG;
    if (!finite_positive(scale)) return SH_ERR_ARG;
    m->buf = (float*)calloc((size_t)w * (size_t)h, sizeof(float));
    if (!m->buf) return SH_ERR_ARG;
    m->w = w; m->h = h;
    m->scale = scale;
    m->bias = SH_DEFAULT_BIAS;
    m->slope_clamp = SH_DEFAULT_SLOPE_CLAMP;
    return 0;
}

void sh_free(ShadowMap *m) { if (m) { free(m->buf); m->buf = NULL; m->w = 0; m->h = 0; } }

int sh_clear(ShadowMap *m) {
    if (!m) return SH_ERR_ARG;
    for (int i = 0; i < m->w * m->h; ++i) m->buf[i] = 0.0f;
    return 0;
}

/* ------------------------------------------- stage 1: offset from the slope */

int sh_set_bias(ShadowMap *m, float bias, float slope_clamp, ShError *err) {
    clear_error(err);
    if (!m) return SH_ERR_ARG;
    if (!isfinite(bias) || bias < 0.0f) return fail(err, SH_ERR_ARG, "bias");
    if (!finite_positive(slope_clamp)) return fail(err, SH_ERR_ARG, "slope_clamp");
    m->bias = bias;
    m->slope_clamp = slope_clamp;
    return 0;
}

int sh_seg_slope(const ShSeg *segs, int index, float *slope, ShError *err) {
    const ShSeg *s;
    float dx, dy, v;
    clear_error(err);
    if (!segs || !slope || index < 0) return SH_ERR_ARG;
    s = &segs[index];
    if (!isfinite(s->x0)) return seg_fail(err, index, SH_SEG_X0);
    if (!isfinite(s->y0)) return seg_fail(err, index, SH_SEG_Y0);
    if (!isfinite(s->x1)) return seg_fail(err, index, SH_SEG_X1);
    if (!isfinite(s->y1)) return seg_fail(err, index, SH_SEG_Y1);
    dx = s->x1 - s->x0;
    dy = s->y1 - s->y0;
    /* |dx| or |dy| can overflow to inf even with finite endpoints. */
    if (!isfinite(dx)) return seg_fail(err, index, SH_SEG_X1);
    if (!isfinite(dy)) return seg_fail(err, index, SH_SEG_Y1);
    if (dx == 0.0f) return seg_fail(err, index, SH_SEG_SLOPE);
    v = fabsf(dy / dx);
    if (!isfinite(v)) return seg_fail(err, index, SH_SEG_SLOPE);
    *slope = v;
    return 0;
}

float sh_seg_offset(const ShadowMap *m, const ShSeg *segs, int index) {
    float slope, clamped;
    if (!map_ok(m) || !segs || !(m->bias > 0.0f)) return 0.0f;
    if (sh_seg_slope(segs, index, &slope, NULL) != 0) return 0.0f;
    clamped = slope < m->slope_clamp ? slope : m->slope_clamp;
    return clamped * m->bias * m->scale;
}

/* ----------------------------------- stage 2: offset inside the intersection */

/* Along-ray distance from the receiving point to a hit on s, or -1. The
 * occluder is infinitely thin, so this is the closest approach of its volume
 * to the receiver along the ray. */
static float hit_distance(const ShSeg *s, float lx, float ly,
                          float px, float py) {
    float rx = px - lx, ry = py - ly;
    float sx = s->x1 - s->x0, sy = s->y1 - s->y0;
    float den = rx * sy - ry * sx;
    float t, u;
    if (fabsf(den) < 1e-9f) return -1.0f;
    t = ((s->x0 - lx) * sy - (s->y0 - ly) * sx) / den;
    u = ((s->x0 - lx) * ry - (s->y0 - ly) * rx) / den;
    if (!(t > 0.0f && t < 1.0f && u >= 0.0f && u <= 1.0f)) return -1.0f;
    return (1.0f - t) * sqrtf(rx * rx + ry * ry);
}

/* A hit contributes to occlusion only if it lies at least offset world units
 * away from the receiver; nearer hits are the receiver's own surface. */
static int blocks(const ShSeg *s, float lx, float ly, float px, float py,
                  float offset) {
    float d = hit_distance(s, lx, ly, px, py);
    if (d < 0.0f) return 0;
    return d >= offset;
}

/* Pixel centre in world units. */
static float world_coord(const ShadowMap *m, int i) {
    return ((float)i + 0.5f) * m->scale;
}

static int pixel_with_offsets(const ShadowMap *m, const ShLight *light,
                              const ShSeg *segs, const float *offsets,
                              int nseg, int x, int y) {
    float px = world_coord(m, x), py = world_coord(m, y);
    for (int i = 0; i < nseg; ++i)
        if (blocks(&segs[i], light->x, light->y, px, py, offsets[i])) return 1;
    return 0;
}

static int light_ok(const ShLight *light) {
    return light && finite_positive(light->radius) &&
           isfinite(light->intensity) && light->intensity >= 0.0f &&
           isfinite(light->x) && isfinite(light->y);
}

/* Shared argument check; keeps the historical -2 for a bad light. */
static int check_args(const ShadowMap *m, const ShLight *light,
                      const ShSeg *segs, int nseg) {
    if (!map_ok(m) || nseg < 0 || (nseg > 0 && !segs)) return SH_ERR_ARG;
    if (!light_ok(light)) return SH_ERR_LIGHT;
    return 0;
}

int sh_pixel_blocked(const ShadowMap *m, const ShLight *light,
                     const ShSeg *segs, int nseg, int x, int y) {
    int blocked = 0, rc;
    rc = check_args(m, light, segs, nseg);
    if (rc != 0) return rc;
    if (x < 0 || y < 0 || x >= m->w || y >= m->h) return SH_ERR_ARG;
    /* No scratch array: this path is per-pixel, so each segment's offset is
     * recomputed exactly as sh_render_ex would have stored it. */
    for (int i = 0; i < nseg; ++i) {
        float slope;
        rc = sh_seg_slope(segs, i, &slope, NULL);
        if (rc != 0) return rc;
        if (blocks(&segs[i], light->x, light->y,
                   world_coord(m, x), world_coord(m, y),
                   sh_seg_offset(m, segs, i))) {
            blocked = 1;
            break;
        }
    }
    return blocked;
}

int sh_render(ShadowMap *m, const ShLight *light, const ShSeg *segs, int nseg) {
    return sh_render_ex(m, light, segs, nseg, NULL);
}

int sh_render_ex(ShadowMap *m, const ShLight *light, const ShSeg *segs,
                 int nseg, ShError *err) {
    float *offsets = NULL;
    int rc;
    clear_error(err);
    rc = check_args(m, light, segs, nseg);
    if (rc != 0) return rc;
    if (nseg > 0) {
        offsets = (float*)malloc(sizeof(float) * (size_t)nseg);
        if (!offsets) return SH_ERR_ARG;
        /* Validate and precompute before touching the map, so a bad slope
         * never leaves a half-written frame behind. */
        for (int i = 0; i < nseg; ++i) {
            float slope;
            int rc = sh_seg_slope(segs, i, &slope, err);
            if (rc != 0) { free(offsets); return rc; }
            offsets[i] = sh_seg_offset(m, segs, i);
        }
    }
    for (int y = 0; y < m->h; ++y) {
        for (int x = 0; x < m->w; ++x) {
            float v = 0.0f;
            if (!pixel_with_offsets(m, light, segs, offsets, nseg, x, y)) {
                float dx = world_coord(m, x) - light->x;
                float dy = world_coord(m, y) - light->y;
                float d = sqrtf(dx * dx + dy * dy);
                float fall = 1.0f / (1.0f + d / light->radius);
                v = light->intensity * fall;
            }
            m->buf[(size_t)y * m->w + x] = v;
        }
    }
    free(offsets);
    return 0;
}

/* ------------------------------------------------- stage 3: stripe counting */

/* The pixel that holds segment s at centre coordinate k along the segment's
 * dominant axis. Stepping along that axis visits every crossed pixel once
 * and keeps the sample count proportional to the segment's pixel length, so
 * the count does not depend on the resolution. */
static int surface_pixel(const ShadowMap *m, const ShSeg *s, int steep,
                         int k, int *out_x, int *out_y) {
    float c = world_coord(m, k);
    int ix, iy;
    if (steep) {
        float x = s->x0 + (c - s->y0) * (s->x1 - s->x0) / (s->y1 - s->y0);
        ix = (int)floorf(x / m->scale);
        iy = k;
    } else {
        float y = s->y0 + (c - s->x0) * (s->y1 - s->y0) / (s->x1 - s->x0);
        ix = k;
        iy = (int)floorf(y / m->scale);
    }
    if (ix < 0 || iy < 0 || ix >= m->w || iy >= m->h) return 0;
    *out_x = ix;
    *out_y = iy;
    return 1;
}

static void seg_bounds(const ShadowMap *m, const ShSeg *s, int *first,
                       int *last, int *steep) {
    float dx = s->x1 - s->x0, dy = s->y1 - s->y0;
    float a, b;
    *steep = fabsf(dy) > fabsf(dx);
    a = *steep ? s->y0 : s->x0;
    b = *steep ? s->y1 : s->x1;
    if (a > b) { float tmp = a; a = b; b = tmp; }
    *first = (int)floorf(a / m->scale);
    *last = (int)floorf(b / m->scale);
}

int sh_acne(const ShadowMap *m, const ShLight *light, const ShSeg *segs,
            int nseg, ShAcne *out, ShError *err) {
    clear_error(err);
    if (!map_ok(m) || !light_ok(light) || !out || nseg < 0 ||
        (nseg > 0 && !segs))
        return SH_ERR_ARG;
    out->checked = out->shadowed = out->stripes = out->longest = 0;
    for (int i = 0; i < nseg; ++i) {
        const ShSeg *s = &segs[i];
        float slope, off;
        int first, last, steep, run = 0, rc;
        rc = sh_seg_slope(segs, i, &slope, err);
        if (rc != 0) return rc;
        seg_bounds(m, s, &first, &last, &steep);
        off = sh_seg_offset(m, segs, i);
        for (int k = first; k <= last; ++k) {
            int ix, iy;
            if (!surface_pixel(m, s, steep, k, &ix, &iy)) continue;
            out->checked++;
            if (blocks(s, light->x, light->y,
                       world_coord(m, ix), world_coord(m, iy), off)) {
                out->shadowed++;
                if (++run > out->longest) out->longest = run;
            } else {
                if (run > 0) out->stripes++;
                run = 0;
            }
        }
        if (run > 0) out->stripes++;
    }
    return 0;
}

int sh_seg_needed_offset(const ShadowMap *m, const ShLight *light,
                         const ShSeg *segs, int index, float *needed,
                         ShError *err) {
    const ShSeg *s;
    int first, last, steep;
    float worst = 0.0f, slope;
    int rc;
    clear_error(err);
    if (!map_ok(m) || !light_ok(light) || !segs || !needed || index < 0)
        return SH_ERR_ARG;
    rc = sh_seg_slope(segs, index, &slope, err);
    if (rc != 0) return rc;
    s = &segs[index];
    seg_bounds(m, s, &first, &last, &steep);
    for (int k = first; k <= last; ++k) {
        int ix, iy;
        float d;
        if (!surface_pixel(m, s, steep, k, &ix, &iy)) continue;
        /* Orientation does not matter: the offset has to exceed the ray
         * distance to the hit on either side of the line. */
        d = hit_distance(s, light->x, light->y,
                         world_coord(m, ix), world_coord(m, iy));
        if (d < 0.0f) continue;
        if (d > worst) worst = d;
    }
    *needed = worst > 0.0f ? worst + m->scale * 1e-3f : 0.0f;
    return 0;
}

/* ------------------------------------------------------------------ output */

int sh_write_pgm(const ShadowMap *m, const char *path) {
    if (!m || !path) return SH_ERR_ARG;
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "P5\n%d %d\n255\n", m->w, m->h);
    for (int i = 0; i < m->w * m->h; ++i) {
        float v = m->buf[i];
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        unsigned char b = (unsigned char)(v * 255.0f + 0.5f);
        fwrite(&b, 1, 1, f);
    }
    fclose(f);
    return 0;
}
