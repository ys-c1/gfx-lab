#include "shadow.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

int sh_init(ShadowMap *m, int w, int h) {
    if (!m || w <= 0 || h <= 0) return -1;
    m->buf = (float*)calloc((size_t)w * (size_t)h, sizeof(float));
    if (!m->buf) return -1;
    m->w = w; m->h = h;
    return 0;
}

void sh_free(ShadowMap *m) { if (m) { free(m->buf); m->buf = NULL; m->w = 0; m->h = 0; } }

int sh_clear(ShadowMap *m) {
    if (!m) return -1;
    for (int i = 0; i < m->w * m->h; ++i) m->buf[i] = 0.0f;
    return 0;
}

static int blocked(const ShSeg *s, float lx, float ly, float px, float py) {
    float rx = px - lx, ry = py - ly;
    float sx = s->x1 - s->x0, sy = s->y1 - s->y0;
    float den = rx * sy - ry * sx;
    if (fabsf(den) < 1e-9f) return 0;
    float t = ((s->x0 - lx) * sy - (s->y0 - ly) * sx) / den;
    float u = ((s->x0 - lx) * ry - (s->y0 - ly) * rx) / den;
    return t > 0.0f && t < 1.0f && u >= 0.0f && u <= 1.0f;
}

static int occluded(const ShSeg *segs, int nseg, const ShLight *light, float px, float py) {
    for (int i = 0; i < nseg; ++i)
        if (blocked(&segs[i], light->x, light->y, px, py)) return 1;
    return 0;
}

/* Falloff of an unoccluded pixel. */
static float falloff(const ShLight *light, float px, float py) {
    float dx = px - light->x, dy = py - light->y;
    float d = sqrtf(dx * dx + dy * dy);
    return light->intensity / (1.0f + d / light->radius);
}

/* Shared by the legacy and the incremental path, so both are bit-identical
 * for the same light. */
static float shade_pixel(const ShLight *light, const ShSeg *segs, int nseg,
                         float px, float py) {
    if (occluded(segs, nseg, light, px, py)) return 0.0f;
    return falloff(light, px, py);
}

int sh_render(ShadowMap *m, const ShLight *light, const ShSeg *segs, int nseg) {
    if (!m || !light || (nseg > 0 && !segs)) return -1;
    if (light->radius <= 0.0f || light->intensity < 0.0f) return -2;
    for (int y = 0; y < m->h; ++y) {
        for (int x = 0; x < m->w; ++x) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            m->buf[(size_t)y * m->w + x] = shade_pixel(light, segs, nseg, px, py);
        }
    }
    return 0;
}

int sh_write_pgm(const ShadowMap *m, const char *path) {
    if (!m || !path) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) return -2;
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

/* ==================================================================== */
/* Animated lights: interpolation + incremental (dirty block) update    */
/* ==================================================================== */

/* Record a failure: code, the offending parameter's name, and a formatted
 * message.  A function rather than a macro so a call with no varargs is
 * valid C11. */
#if defined(__GNUC__)
__attribute__((format(printf, 4, 5)))
#endif
static void sh_fail(ShError *e, int code, const char *param, const char *fmt, ...) {
    if (!e) return;
    e->code = code;
    e->param = param;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->msg, sizeof e->msg, fmt, ap);
    va_end(ap);
}

#define SH_MSG(e, c, p, ...) sh_fail((e), (c), (p), __VA_ARGS__)

static void sh_err_clear(ShError *e) {
    if (e) { e->code = SH_OK; e->param = NULL; e->msg[0] = '\0'; }
}

static int sh_light_ok(const ShLight *l) {
    if (!l) return 0;
    if (!isfinite(l->x) || !isfinite(l->y)) return 0;
    if (!isfinite(l->radius) || !isfinite(l->intensity)) return 0;
    if (l->radius <= 0.0f || l->intensity < 0.0f) return 0;
    return 1;
}

static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static float dist_point_seg(float px, float py, float ax, float ay, float bx, float by) {
    float vx = bx - ax, vy = by - ay;
    float wx = px - ax, wy = py - ay;
    float len2 = vx * vx + vy * vy;
    float t = len2 > 0.0f ? (wx * vx + wy * vy) / len2 : 0.0f;
    t = clampf(t, 0.0f, 1.0f);
    float dx = px - (ax + t * vx), dy = py - (ay + t * vy);
    return sqrtf(dx * dx + dy * dy);
}

/* ------------------------------------------------------------------ */
/* Why the occlusion mask can change, and where                        */
/* ------------------------------------------------------------------ */
/*
 * For a fixed pixel P the blocker test in blocked() uses two intersection
 * parameters: t along L->P and u along A->B (A, B the blocker ends).  Both
 * are continuous in L, so the shadowed/unshadowed state of P can only change
 * when a parameter leaves its valid interval:
 *
 *   t = 0    L lies on the blocker             -> the light walks through the
 *                                                 scene; refresh everything
 *   t = 1    P lies exactly on the blocker     -> float knife edge at P
 *   u = 0    the ray L->P meets the blocker at A -> P lies beyond A on the
 *                                                 ray from the light; a wedge
 *   u = 1    same at B                         -> same, at the other end
 *
 * The u cases are the important ones.  "L, P, A collinear with P beyond A"
 * means P lies in the wedge with apex A opened away from the light, bounded
 * by the directions A-L0 and A-L1.  Over the light's motion that wedge is the
 * only region where a pixel can switch sides of a blocker end, so it is
 * rasterized and its blocks are the candidates for a mask recompute.  A wide
 * sweep (or the light stepping onto a blocker) falls back to a full refresh.
 *
 * Value changes are independent of visibility: every non-candidate block is
 * still checked against the falloff tolerance and, when that is exceeded,
 * re-shaded from the cached mask without any segment tests.
 */

/* Mark every block that contains any pixel covered by the triangle T0, T1, T2.
 * Pixels whose centre lies inside the triangle are marked.  The scanline is
 * edge-inclusive, so points exactly on an edge (the knife-edge case) are
 * marked too. */
static void mark_triangle(const ShAnim *a, unsigned char *cand,
                          float t0x, float t0y, float t1x, float t1y,
                          float t2x, float t2y, float margin) {
    float xs[3] = {t0x, t1x, t2x}, ys[3] = {t0y, t1y, t2y};
    float ymin = fminf(ys[0], fminf(ys[1], ys[2])) - margin;
    float ymax = fmaxf(ys[0], fmaxf(ys[1], ys[2])) + margin;
    if (ymax < -margin || ymin > (float)a->map.h + margin) return;
    if (ymin < -margin) ymin = -margin;
    if (ymax > (float)a->map.h + margin) ymax = (float)a->map.h + margin;

    /* Degenerate triangles (light did not move), handled as a segment. */
    int y0 = (int)floorf(ymin), y1 = (int)ceilf(ymax);
    for (int py = y0; py <= y1; ++py) {
        float sy = (float)py + 0.5f;
        float xmin = INFINITY, xmax = -INFINITY;
        for (int i = 0; i < 3; ++i) {
            int j = (i + 1) % 3;
            float ya = ys[i], yb = ys[j];
            if ((ya <= sy && sy <= yb) || (yb <= sy && sy <= ya)) {
                float dyy = yb - ya;
                float x = fabsf(dyy) < 1e-9f ? fminf(xs[i], xs[j])
                                             : xs[i] + (xs[j] - xs[i]) * (sy - ya) / dyy;
                if (x < xmin) xmin = x;
                if (x > xmax) xmax = x;
            }
        }
        if (xmin > xmax) continue;
        xmin -= margin; xmax += margin;
        int bx0 = (int)floorf(xmin / (float)a->block);
        int bx1 = (int)floorf(xmax / (float)a->block);
        if (bx0 < 0) bx0 = 0;
        if (bx1 >= a->bw) bx1 = a->bw - 1;
        if (bx0 >= a->bw) continue;
        int row = py / a->block;
        if (row < 0 || row >= a->bh) continue;
        for (int col = bx0; col <= bx1; ++col)
            cand[(size_t)row * (size_t)a->bw + (size_t)col] = 1;
    }
}

/* Mark the blocks along a blocker segment: pixels exactly on it sit on the
 * t = 1 float knife edge and flicker regardless of the light. */
static void mark_segment(const ShAnim *a, unsigned char *cand, const ShSeg *s) {
    float dx = s->x1 - s->x0, dy = s->y1 - s->y0;
    float len = hypotf(dx, dy);
    if (len < 1e-9f) {
        int bx = (int)(s->x0 / (float)a->block), by = (int)(s->y0 / (float)a->block);
        if (bx >= 0 && bx < a->bw && by >= 0 && by < a->bh)
            cand[(size_t)by * (size_t)a->bw + (size_t)bx] = 1;
        return;
    }
    long n = (long)(len / 0.5f) + 1;
    for (long i = 0; i <= n; ++i) {
        float t = (float)i / (float)n;
        float px = s->x0 + dx * t, py = s->y0 + dy * t;
        int bx = (int)(px / (float)a->block), by = (int)(py / (float)a->block);
        if (bx >= 0 && bx < a->bw && by >= 0 && by < a->bh)
            cand[(size_t)by * (size_t)a->bw + (size_t)bx] = 1;
    }
}

/* Does the light path cross a blocker segment (t = 0)?  Then the blocker
 * sweeps a large region and every pixel is a candidate. */
static int light_crosses_scene(const ShAnim *a, const ShLight *l0, const ShLight *l1) {
    for (int i = 0; i < a->nseg; ++i) {
        const ShSeg *s = &a->segs[i];
        float ax = s->x0, ay = s->y0, bx = s->x1, by = s->y1;
        float rx = l1->x - l0->x, ry = l1->y - l0->y;
        float sx = bx - ax, sy = by - ay;
        float den = rx * sy - ry * sx;
        if (fabsf(den) < 1e-9f) continue;
        float t = ((ax - l0->x) * sy - (ay - l0->y) * sx) / den;
        float u = ((ax - l0->x) * ry - (ay - l0->y) * rx) / den;
        if (t > 0.0f && t < 1.0f && u >= 0.0f && u <= 1.0f) return 1;
    }
    return 0;
}

/* Fill `cand` (one byte per block) with the blocks a visibility flip can
 * reach, and return 0.  Returns -1 when the sweep is too wide to bound
 * cheaply, and the caller should refresh every block. */
static int build_candidates(ShAnim *a, const ShLight *l0, const ShLight *l1) {
    memset(a->cand, 0, (size_t)a->bw * (size_t)a->bh);
    for (int i = 0; i < a->nseg; ++i) mark_segment(a, a->cand, &a->segs[i]);

    /*
     * For each blocker end E, the candidate pixels are those beyond E on the
     * ray from the light through E, i.e. in the wedge from E opened away from
     * the light and bounded by the directions E-L0 and E-L1.  Rasterize that
     * wedge as a triangle whose far vertices lie outside the map.
     *
     * If the light path as seen from E sweeps more than ~90 degrees the wedge
     * covers most of the map; return -1 for a full refresh instead.
     */
    float R = hypotf((float)a->map.w, (float)a->map.h) +
              2.0f * (float)a->block + 2.0f;
    for (int i = 0; i < a->nseg; ++i) {
        const ShSeg *s = &a->segs[i];
        const float ex[2] = {s->x0, s->x1};
        const float ey[2] = {s->y0, s->y1};
        for (int k = 0; k < 2; ++k) {
            float d0x = l0->x - ex[k], d0y = l0->y - ey[k];
            float d1x = l1->x - ex[k], d1y = l1->y - ey[k];
            float n0 = hypotf(d0x, d0y), n1 = hypotf(d1x, d1y);
            if (n0 < 1e-3f || n1 < 1e-3f) return -1;   /* light path hits E */
            d0x /= n0; d0y /= n0;
            d1x /= n1; d1y /= n1;
            if (d0x * d1x + d0y * d1y < 0.17f) return -1;   /* ~80 degrees */
            mark_triangle(a, a->cand, ex[k], ey[k],
                          ex[k] - R * d0x, ey[k] - R * d0y,
                          ex[k] - R * d1x, ey[k] - R * d1y, 0.5f);
        }
    }
    return 0;
}

/*
 * Per-block bound on the change of the non-occluded falloff term
 *     v = I*r / (r+d),   d = |p - L|
 * when the light moves from l0 to l1.
 *
 * The distance changes by at most the light displacement (triangle
 * inequality), so with r_min / I_max over both lights and d_min the block
 * centre distance to the light path minus the block half diagonal:
 *
 *   |dv/dL| * D <= I_max * r_min * D / (r_min + d_min)^2
 *   |dv/dr| * dr <= I_max * d_max * dr / (r_min + d_min)^2
 *   |dv/dI| * dI <= dI
 *
 * d_min lower-bounds every pixel distance along the whole motion and d_max
 * (farthest corner from either endpoint) upper-bounds it, since distance to
 * a point is convex along a segment.
 */
static float falloff_delta_bound(const ShAnim *a, const ShLight *l0, const ShLight *l1,
                                 int bx, int by) {
    float x0 = (float)(bx * a->block), y0 = (float)(by * a->block);
    float x1 = x0 + (float)a->block, y1 = y0 + (float)a->block;
    if (x1 > (float)a->map.w) x1 = (float)a->map.w;
    if (y1 > (float)a->map.h) y1 = (float)a->map.h;

    float cx = 0.5f * (x0 + x1), cy = 0.5f * (y0 + y1);
    float halfdiag = 0.5f * hypotf(x1 - x0, y1 - y0);
    float dmin = dist_point_seg(cx, cy, l0->x, l0->y, l1->x, l1->y) - halfdiag;
    if (dmin < 0.0f) dmin = 0.0f;

    float dmax = 0.0f;
    const float px[4] = {x0, x1, x1, x0};
    const float py[4] = {y0, y0, y1, y1};
    for (int i = 0; i < 4; ++i) {
        float d0 = hypotf(px[i] - l0->x, py[i] - l0->y);
        float d1 = hypotf(px[i] - l1->x, py[i] - l1->y);
        if (d0 > dmax) dmax = d0;
        if (d1 > dmax) dmax = d1;
    }

    float imax = fmaxf(l0->intensity, l1->intensity);
    float rmin = fminf(l0->radius, l1->radius);
    float denom = rmin + dmin;
    if (denom < 1e-6f) denom = 1e-6f;
    float g = 1.0f / (denom * denom);

    float D = hypotf(l1->x - l0->x, l1->y - l0->y);
    float dr = fabsf(l1->radius - l0->radius);
    float dI = fabsf(l1->intensity - l0->intensity);
    return imax * rmin * D * g + imax * dmax * dr * g + dI;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int sh_lerp_light(const ShLight *prev, const ShLight *next, float t, int frame_span,
                  ShLight *out, ShError *err) {
    sh_err_clear(err);
    if (!prev || !next || !out) {
        SH_MSG(err, SH_ERR_NULL, !out ? "out" : (!prev ? "prev" : "next"),
               "sh_lerp_light: required pointer is NULL");
        return SH_ERR_NULL;
    }
    if (!isfinite(t) || t < 0.0f || t > 1.0f) {
        SH_MSG(err, SH_ERR_T_RANGE, "t",
               "sh_lerp_light: t=%g is outside [0, 1]", (double)t);
        return SH_ERR_T_RANGE;
    }
    if (frame_span <= 0) {
        SH_MSG(err, SH_ERR_FRAME_SPAN, "frame_span",
               "sh_lerp_light: frame_span=%d must be > 0", frame_span);
        return SH_ERR_FRAME_SPAN;
    }
    if (!sh_light_ok(prev)) {
        SH_MSG(err, SH_ERR_LIGHT, "prev",
               "sh_lerp_light: prev light invalid (radius=%g intensity=%g; "
               "radius must be > 0, intensity >= 0, all finite)",
               (double)prev->radius, (double)prev->intensity);
        return SH_ERR_LIGHT;
    }
    if (!sh_light_ok(next)) {
        SH_MSG(err, SH_ERR_LIGHT, "next",
               "sh_lerp_light: next light invalid (radius=%g intensity=%g; "
               "radius must be > 0, intensity >= 0, all finite)",
               (double)next->radius, (double)next->intensity);
        return SH_ERR_LIGHT;
    }

    /* Key frames are reproduced bit-exactly. */
    if (t <= 0.0f) { *out = *prev; return SH_OK; }
    if (t >= 1.0f) { *out = *next; return SH_OK; }

    out->x = prev->x + (next->x - prev->x) * t;
    out->y = prev->y + (next->y - prev->y) * t;
    out->radius = prev->radius + (next->radius - prev->radius) * t;
    out->intensity = prev->intensity + (next->intensity - prev->intensity) * t;
    return SH_OK;
}

int sh_anim_init(ShAnim *a, int w, int h, int block) {
    if (!a) return SH_ERR_NULL;
    memset(a, 0, sizeof *a);
    if (w <= 0 || h <= 0 || block <= 0) return SH_ERR_ARG;
    if (sh_init(&a->map, w, h) != 0) return SH_ERR_MEM;
    a->block = block;
    a->bw = (w + block - 1) / block;
    a->bh = (h + block - 1) / block;
    size_t nblk = (size_t)a->bw * (size_t)a->bh;
    size_t npix = (size_t)w * (size_t)h;
    a->dirty = (unsigned char*)calloc(nblk, 1);
    a->cand = (unsigned char*)calloc(nblk, 1);
    a->err_bound = (float*)calloc(nblk, sizeof(float));
    a->occ = (unsigned char*)calloc(npix, 1);
    if (!a->dirty || !a->cand || !a->err_bound || !a->occ) {
        sh_anim_free(a); return SH_ERR_MEM;
    }
    a->eps = SH_EPS_DEFAULT;
    return SH_OK;
}

void sh_anim_free(ShAnim *a) {
    if (!a) return;
    sh_free(&a->map);
    free(a->segs); a->segs = NULL;
    free(a->dirty); a->dirty = NULL;
    free(a->cand); a->cand = NULL;
    free(a->err_bound); a->err_bound = NULL;
    free(a->occ); a->occ = NULL;
    a->nseg = 0; a->have_frame = 0; a->frame = 0;
    a->bw = a->bh = 0; a->block = 0;
}

int sh_anim_set_scene(ShAnim *a, const ShSeg *segs, int nseg) {
    if (!a || !a->map.buf) return SH_ERR_NULL;
    if (nseg < 0 || (nseg > 0 && !segs)) return SH_ERR_ARG;
    ShSeg *copy = NULL;
    if (nseg > 0) {
        copy = (ShSeg*)malloc(sizeof(ShSeg) * (size_t)nseg);
        if (!copy) return SH_ERR_MEM;
        memcpy(copy, segs, sizeof(ShSeg) * (size_t)nseg);
    }
    free(a->segs);
    a->segs = copy;
    a->nseg = nseg;
    a->have_frame = 0;      /* the cached map no longer matches the scene */
    a->frame = 0;
    return SH_OK;
}

int sh_anim_set_eps(ShAnim *a, float eps) {
    if (!a || !a->map.buf) return SH_ERR_NULL;
    if (!isfinite(eps) || eps < 0.0f) return SH_ERR_ARG;
    a->eps = eps;
    return SH_OK;
}

static void block_bounds(const ShAnim *a, int bx, int by, int *x0, int *y0, int *x1, int *y1) {
    *x0 = bx * a->block; *y0 = by * a->block;
    *x1 = *x0 + a->block; *y1 = *y0 + a->block;
    if (*x1 > a->map.w) *x1 = a->map.w;
    if (*y1 > a->map.h) *y1 = a->map.h;
}

/* Full recompute of one block: visibility mask and shaded value. */
static void render_block(ShAnim *a, const ShLight *l, int bx, int by) {
    int x0, y0, x1, y1;
    block_bounds(a, bx, by, &x0, &y0, &x1, &y1);
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            size_t i = (size_t)y * a->map.w + x;
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            int occ = occluded(a->segs, a->nseg, l, px, py);
            a->occ[i] = (unsigned char)occ;
            a->map.buf[i] = occ ? 0.0f : falloff(l, px, py);
        }
}

/* Re-evaluate only the falloff of a block whose visibility did not change:
 * no segment tests, one distance per pixel. */
static void refade_block(ShAnim *a, const ShLight *l, int bx, int by) {
    int x0, y0, x1, y1;
    block_bounds(a, bx, by, &x0, &y0, &x1, &y1);
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            size_t i = (size_t)y * a->map.w + x;
            a->map.buf[i] = a->occ[i] ? 0.0f : falloff(l, (float)x + 0.5f, (float)y + 0.5f);
        }
}

/* Visibility mask for the whole map at light `l`.  Called after sh_render()
 * on a full refresh; the two share occluded()/falloff(), so mask and values
 * agree exactly. */
static void rebuild_mask(ShAnim *a, const ShLight *l) {
    for (int y = 0; y < a->map.h; ++y)
        for (int x = 0; x < a->map.w; ++x) {
            size_t i = (size_t)y * a->map.w + x;
            a->occ[i] = (unsigned char)occluded(a->segs, a->nseg, l,
                                                (float)x + 0.5f, (float)y + 0.5f);
        }
}

int sh_anim_step(ShAnim *a, const ShLight *prev, const ShLight *next, float t,
                 int frame_span, int interpolate, ShStepStats *stats, ShError *err) {
    sh_err_clear(err);
    if (stats) memset(stats, 0, sizeof *stats);
    if (!a || !a->map.buf) {
        SH_MSG(err, SH_ERR_NULL, "anim", "sh_anim_step: anim is NULL or not initialised");
        return SH_ERR_NULL;
    }
    if (!next) {
        SH_MSG(err, SH_ERR_NULL, "next", "sh_anim_step: next light is NULL");
        return SH_ERR_NULL;
    }

    ShLight lit;
    int rc;
    if (interpolate) {
        if (!prev) {
            SH_MSG(err, SH_ERR_NULL, "prev",
                   "sh_anim_step: prev light is required when interpolate != 0");
            return SH_ERR_NULL;
        }
        rc = sh_lerp_light(prev, next, t, frame_span, &lit, err);
        if (rc != SH_OK) return rc;
    } else {
        /* Interpolation disabled: use the next key light verbatim.  t and
         * frame_span play no part, so this reproduces the legacy result. */
        if (!sh_light_ok(next)) {
            SH_MSG(err, SH_ERR_LIGHT, "next",
                   "sh_anim_step: next light invalid (radius=%g intensity=%g)",
                   (double)next->radius, (double)next->intensity);
            return SH_ERR_LIGHT;
        }
        lit = *next;
    }

    const int total_blocks = a->bw * a->bh;
    int full = !interpolate || !a->have_frame;
    int recompute_blocks = 0, refade_blocks = 0, skip_blocks = 0;
    long recompute_pixels = 0, refade_pixels = 0;

    if (!full) {
        if (light_crosses_scene(a, &a->light, &lit) ||
            build_candidates(a, &a->light, &lit) != 0)
            full = 1;
    }

    if (full) {
        if (sh_render(&a->map, &lit, a->segs, a->nseg) != 0) {
            SH_MSG(err, SH_ERR_LIGHT, "next",
                   "sh_anim_step: sh_render rejected the light");
            return SH_ERR_LIGHT;
        }
        rebuild_mask(a, &lit);
        for (int i = 0; i < total_blocks; ++i) {
            a->dirty[i] = 2;
            a->err_bound[i] = 0.0f;
        }
        recompute_blocks = total_blocks;
        recompute_pixels = (long)a->map.w * (long)a->map.h;
    } else {
        for (int by = 0; by < a->bh; ++by) {
            for (int bx = 0; bx < a->bw; ++bx) {
                size_t i = (size_t)by * (size_t)a->bw + (size_t)bx;
                int x0, y0, x1, y1;
                block_bounds(a, bx, by, &x0, &y0, &x1, &y1);
                int px_count = (x1 - x0) * (y1 - y0);

                if (a->cand[i]) {
                    /* Visibility may have changed: recompute mask and value. */
                    render_block(a, &lit, bx, by);
                    a->dirty[i] = 2;
                    a->err_bound[i] = 0.0f;
                    ++recompute_blocks;
                    recompute_pixels += px_count;
                    continue;
                }
                float acc = a->err_bound[i] +
                            falloff_delta_bound(a, &a->light, &lit, bx, by);
                if (acc <= a->eps) {
                    a->dirty[i] = 0;
                    a->err_bound[i] = acc;
                    ++skip_blocks;
                    continue;
                }
                /* Mask is provably unchanged; re-shade from it. */
                refade_block(a, &lit, bx, by);
                a->dirty[i] = 1;
                a->err_bound[i] = 0.0f;
                ++refade_blocks;
                refade_pixels += px_count;
            }
        }
    }

    a->light = lit;
    a->have_frame = 1;
    int frame = a->frame++;

    if (stats) {
        stats->frame = frame;
        stats->interpolated = interpolate != 0;
        stats->full_refresh = full;
        stats->dirty_blocks = recompute_blocks + refade_blocks;
        stats->recompute_blocks = recompute_blocks;
        stats->refade_blocks = refade_blocks;
        stats->skip_blocks = skip_blocks;
        stats->total_blocks = total_blocks;
        stats->recompute_pixels = recompute_pixels;
        stats->refade_pixels = refade_pixels;
        stats->dirty_pixels = recompute_pixels + refade_pixels;
        stats->total_pixels = (long)a->map.w * (long)a->map.h;
        float mb = 0.0f;
        for (int i = 0; i < total_blocks; ++i)
            if (a->err_bound[i] > mb) mb = a->err_bound[i];
        stats->max_err_bound = mb;
    }
    return SH_OK;
}

int sh_write_trajectory(const char *path, const ShLight *prev, const ShLight *next,
                        int frames, int frame_span, ShError *err) {
    sh_err_clear(err);
    if (!path || !prev || !next) {
        SH_MSG(err, SH_ERR_NULL, !path ? "path" : (!prev ? "prev" : "next"),
               "sh_write_trajectory: required pointer is NULL");
        return SH_ERR_NULL;
    }
    if (frames < 2) {
        SH_MSG(err, SH_ERR_ARG, "frames",
               "sh_write_trajectory: frames=%d must be >= 2", frames);
        return SH_ERR_ARG;
    }
    if (frame_span <= 0) {
        SH_MSG(err, SH_ERR_FRAME_SPAN, "frame_span",
               "sh_write_trajectory: frame_span=%d must be > 0", frame_span);
        return SH_ERR_FRAME_SPAN;
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        SH_MSG(err, SH_ERR_IO, "path",
               "sh_write_trajectory: cannot open '%s' for writing", path);
        return SH_ERR_IO;
    }
    fprintf(f, "frame,t,x,y,radius,intensity\n");
    for (int i = 0; i < frames; ++i) {
        float tt = (float)i / (float)(frames - 1);
        ShLight l;
        ShError e1;
        if (sh_lerp_light(prev, next, tt, frame_span, &l, &e1) != SH_OK) {
            fclose(f);
            if (err) *err = e1;
            return e1.code;
        }
        int fr = (int)lroundf(tt * (float)frame_span);
        fprintf(f, "%d,%.6f,%.6f,%.6f,%.6f,%.6f\n", fr, (double)tt,
                (double)l.x, (double)l.y, (double)l.radius, (double)l.intensity);
    }
    if (fclose(f) != 0) {
        SH_MSG(err, SH_ERR_IO, "path", "sh_write_trajectory: write to '%s' failed", path);
        return SH_ERR_IO;
    }
    return SH_OK;
}

int sh_compare_maps(const ShadowMap *a, const ShadowMap *b,
                    double *max_err, double *mean_err, double *rmse) {
    if (!a || !b || !a->buf || !b->buf) return SH_ERR_NULL;
    if (a->w != b->w || a->h != b->h) return SH_ERR_ARG;
    size_t n = (size_t)a->w * (size_t)a->h;
    double mx = 0.0, sum = 0.0, sq = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double d = fabs((double)a->buf[i] - (double)b->buf[i]);
        if (d > mx) mx = d;
        sum += d;
        sq += d * d;
    }
    if (max_err) *max_err = mx;
    if (mean_err) *mean_err = n ? sum / (double)n : 0.0;
    if (rmse) *rmse = n ? sqrt(sq / (double)n) : 0.0;
    return SH_OK;
}
