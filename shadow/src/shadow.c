#include "shadow.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static float shade_pixel(const ShLight *light, const ShSeg *segs, int nseg, int x, int y) {
    float px = (float)x + 0.5f, py = (float)y + 0.5f;
    for (int i = 0; i < nseg; ++i)
        if (blocked(&segs[i], light->x, light->y, px, py)) return 0.0f;
    float dx = px - light->x, dy = py - light->y;
    float d = sqrtf(dx * dx + dy * dy);
    return light->intensity * (1.0f / (1.0f + d / light->radius));
}

static int check_light(const ShLight *light) {
    if (light->radius <= 0.0f || light->intensity < 0.0f) return -2;
    return 0;
}

int sh_render(ShadowMap *m, const ShLight *light, const ShSeg *segs, int nseg) {
    if (!m || !light || (nseg > 0 && !segs)) return -1;
    int rc = check_light(light);
    if (rc) return rc;
    for (int y = 0; y < m->h; ++y)
        for (int x = 0; x < m->w; ++x)
            m->buf[(size_t)y * m->w + x] = shade_pixel(light, segs, nseg, x, y);
    return 0;
}

/* ---------------- incremental (two-phase: mark, then recompute) ---------------- */

/* Per-pixel change below SH_TOL is treated as invisible: a block is only
 * marked when the lighting in it could change by more than this. The residual
 * error this introduces is what the benchmark measures. */
#define SH_TOL 0.02f

int sh_blocks_init(ShBlocks *b, int w, int h, int block) {
    if (!b || w <= 0 || h <= 0) return -1;
    if (block <= 0) {
        fprintf(stderr, "sh_blocks_init: parameter 'block' must be > 0 (got %d)\n", block);
        return -3;
    }
    b->bw = (w + block - 1) / block;
    b->bh = (h + block - 1) / block;
    b->tab = (unsigned char*)calloc((size_t)b->bw * (size_t)b->bh, 1);
    b->acc = (float*)calloc((size_t)b->bw * (size_t)b->bh, sizeof(float));
    if (!b->tab || !b->acc) { free(b->tab); free(b->acc); return -1; }
    b->block = block; b->w = w; b->h = h;
    return 0;
}

void sh_blocks_free(ShBlocks *b) {
    if (b) { free(b->tab); free(b->acc); b->tab = NULL; b->acc = NULL; b->block = b->w = b->h = b->bw = b->bh = 0; }
}

static int check_finite(const ShLight *l, const char *param) {
    if (!isfinite((double)l->x)) { fprintf(stderr, "sh_mark: parameter '%s->x' is not finite (got %g)\n", param, (double)l->x); return -4; }
    if (!isfinite((double)l->y)) { fprintf(stderr, "sh_mark: parameter '%s->y' is not finite (got %g)\n", param, (double)l->y); return -4; }
    return 0;
}

/* Min distance from point (px,py) to rect [x0,x1]x[y0,y1]. */
static float rect_dmin(float px, float py, float x0, float y0, float x1, float y1) {
    float dx = px < x0 ? x0 - px : (px > x1 ? px - x1 : 0.0f);
    float dy = py < y0 ? y0 - py : (py > y1 ? py - y1 : 0.0f);
    return sqrtf(dx * dx + dy * dy);
}

/* Distance beyond which a light's contribution is below SH_TOL. */
static float influence_radius(const ShLight *l) {
    float r = l->radius * (l->intensity / SH_TOL - 1.0f);
    return r > 0.0f ? r : 0.0f;
}

static float cross2(float ax, float ay, float bx, float by) { return ax * by - ay * bx; }

static int pt_in_rect(float px, float py, float x0, float y0, float x1, float y1) {
    return px >= x0 && px <= x1 && py >= y0 && py <= y1;
}

static int pt_in_tri(float px, float py, const float *t) {
    float d1 = cross2(t[2] - t[0], t[3] - t[1], px - t[0], py - t[1]);
    float d2 = cross2(t[4] - t[2], t[5] - t[3], px - t[2], py - t[3]);
    float d3 = cross2(t[0] - t[4], t[1] - t[5], px - t[4], py - t[5]);
    int neg = (d1 < 0.0f) || (d2 < 0.0f) || (d3 < 0.0f);
    int pos = (d1 > 0.0f) || (d2 > 0.0f) || (d3 > 0.0f);
    return !(neg && pos);
}

static int segs_cross(float ax, float ay, float bx, float by,
                      float cx, float cy, float dx, float dy) {
    float d1 = cross2(dx - cx, dy - cy, ax - cx, ay - cy);
    float d2 = cross2(dx - cx, dy - cy, bx - cx, by - cy);
    float d3 = cross2(bx - ax, by - ay, cx - ax, cy - ay);
    float d4 = cross2(bx - ax, by - ay, dx - ax, dy - ay);
    return ((d1 > 0.0f && d2 < 0.0f) || (d1 < 0.0f && d2 > 0.0f)) &&
           ((d3 > 0.0f && d4 < 0.0f) || (d3 < 0.0f && d4 > 0.0f));
}

/* Triangle (t[0..5] = ax,ay,bx,by,cx,cy) vs rect [x0,x1]x[y0,y1]. */
static int tri_rect_hit(const float *t, float x0, float y0, float x1, float y1) {
    if (pt_in_rect(t[0], t[1], x0, y0, x1, y1) ||
        pt_in_rect(t[2], t[3], x0, y0, x1, y1) ||
        pt_in_rect(t[4], t[5], x0, y0, x1, y1)) return 1;
    if (pt_in_tri(x0, y0, t) || pt_in_tri(x1, y0, t) ||
        pt_in_tri(x0, y1, t) || pt_in_tri(x1, y1, t)) return 1;
    float rx[4] = {x0, x1, x1, x0}, ry[4] = {y0, y0, y1, y1};
    for (int e = 0; e < 3; ++e) {
        float ax = t[2 * e], ay = t[2 * e + 1];
        float bx = t[2 * ((e + 1) % 3)], by = t[2 * ((e + 1) % 3) + 1];
        for (int r = 0; r < 4; ++r)
            if (segs_cross(ax, ay, bx, by, rx[r], ry[r], rx[(r + 1) % 4], ry[(r + 1) % 4]))
                return 1;
    }
    return 0;
}

/* Sliver swept by the shadow boundary at segment endpoint (ex,ey) when the
 * light moves l1 -> l2: the flip region at an endpoint is the cone between
 * the two boundary rays beyond the endpoint. (Pixels on the light side of
 * the segment's supporting line can never flip while both lights stay on
 * the same side: the light-to-pixel ray cannot cross the line at all. The
 * crossing case is handled separately by the coarse fallback in sh_mark.)
 * The triangle is long enough to contain every flip whose visible impact
 * exceeds SH_TOL. Fills tri[6]; returns 1 if degenerate (caller falls back
 * to the coarse influence circles). */
static int sliver_tri(const ShLight *l1, const ShLight *l2, float ex, float ey,
                      float reach, float *tri) {
    float v1x = ex - l1->x, v1y = ey - l1->y;
    float v2x = ex - l2->x, v2y = ey - l2->y;
    float d1 = hypotf(v1x, v1y), d2 = hypotf(v2x, v2y);
    if (d1 < 1e-6f || d2 < 1e-6f) return 1;
    v1x /= d1; v1y /= d1; v2x /= d2; v2y /= d2;
    float dot = v1x * v2x + v1y * v2y;
    float c = sqrtf(fmaxf(0.0f, (1.0f + dot) * 0.5f));   /* cos(half sweep angle) */
    if (c < 0.05f) return 1;                             /* sweep ~180 deg: mark coarse */
    float L = reach / c;
    tri[0] = ex;            tri[1] = ey;
    tri[2] = ex + v1x * L;  tri[3] = ey + v1y * L;
    tri[4] = ex + v2x * L;  tri[5] = ey + v2y * L;
    return 0;
}

int sh_mark(ShBlocks *b, const ShLight *prev, const ShLight *next, const ShSeg *segs, int nseg) {
    if (!b || !b->tab || !prev || !next || (nseg > 0 && !segs)) return -1;
    if (b->block <= 0) {
        fprintf(stderr, "sh_mark: parameter 'block' must be > 0 (got %d)\n", b->block);
        return -3;
    }
    int rc = check_light(prev); if (rc) return rc;
    rc = check_light(next); if (rc) return rc;
    rc = check_finite(prev, "prev"); if (rc) return rc;
    rc = check_finite(next, "next"); if (rc) return rc;

    memset(b->tab, 0, (size_t)b->bw * (size_t)b->bh);

    float ldx = next->x - prev->x, ldy = next->y - prev->y;
    float moved = sqrtf(ldx * ldx + ldy * ldy);
    int shape_changed = (prev->radius != next->radius) || (prev->intensity != next->intensity);
    if (moved == 0.0f && !shape_changed) return 0;   /* nothing can change */

    /* Falloff-change reach: |d fall| <= I * moved / r / (1 + d/r)^2 > SH_TOL
     * gives d < sqrt(I*moved*r/SH_TOL) - r around each light position. If
     * radius/intensity also changed, fall back to the full influence circles. */
    float dfall_prev = 0.0f, dfall_next = 0.0f;
    if (shape_changed) {
        dfall_prev = influence_radius(prev);
        dfall_next = influence_radius(next);
    } else if (moved > 0.0f) {
        float v = next->intensity * moved / (next->radius * SH_TOL);
        if (v > 1.0f) dfall_prev = dfall_next = next->radius * (sqrtf(v) - 1.0f);
    }

    float dmax_prev = influence_radius(prev);
    float dmax_next = influence_radius(next);

    /* Per segment: one far-cone sliver per endpoint = 2 triangles, or coarse
     * flag. Layout per segment: coarse[i], tris[12] (two triangles of 6
     * floats). A sliver reach is the influence radius plus the endpoint-to-
     * light distance, so the triangles contain every flip whose impact
     * exceeds SH_TOL; the exact triangle-rect test needs no map clamping.
     * If the light crosses the segment's supporting line, the flip region is
     * a half-plane: fall back to the coarse influence circles. */
    float *tris = NULL;
    unsigned char *coarse = NULL;
    if (nseg > 0) {
        tris = (float*)malloc((size_t)nseg * 12 * sizeof(float));
        coarse = (unsigned char*)calloc((size_t)nseg, 1);
        if (!tris || !coarse) { free(tris); free(coarse); return -1; }
        for (int i = 0; i < nseg; ++i) {
            const ShSeg *sg = &segs[i];
            float sx = sg->x1 - sg->x0, sy = sg->y1 - sg->y0;
            float side1 = sx * (prev->y - sg->y0) - sy * (prev->x - sg->x0);
            float side2 = sx * (next->y - sg->y0) - sy * (next->x - sg->x0);
            if (side1 * side2 <= 0.0f) { coarse[i] = 1; continue; }
            float r1 = dmax_prev + hypotf(sg->x0 - prev->x, sg->y0 - prev->y);
            float r2 = dmax_next + hypotf(sg->x0 - next->x, sg->y0 - next->y);
            float reach0 = fmaxf(r1, r2);
            r1 = dmax_prev + hypotf(sg->x1 - prev->x, sg->y1 - prev->y);
            r2 = dmax_next + hypotf(sg->x1 - next->x, sg->y1 - next->y);
            float reach1 = fmaxf(r1, r2);
            int c0 = sliver_tri(prev, next, sg->x0, sg->y0, reach0, &tris[i * 12 + 0]);
            int c1 = sliver_tri(prev, next, sg->x1, sg->y1, reach1, &tris[i * 12 + 6]);
            coarse[i] = (unsigned char)(c0 || c1);
        }
    }

    for (int by = 0; by < b->bh; ++by) {
        float ry0 = (float)(by * b->block);
        float ry1 = (float)((by + 1) * b->block < b->h ? (by + 1) * b->block : b->h);
        for (int bx = 0; bx < b->bw; ++bx) {
            float rx0 = (float)(bx * b->block);
            float rx1 = (float)((bx + 1) * b->block < b->w ? (bx + 1) * b->block : b->w);
            int dirty = 0;
            if (dfall_prev > 0.0f && rect_dmin(prev->x, prev->y, rx0, ry0, rx1, ry1) <= dfall_prev) dirty = 1;
            if (!dirty && dfall_next > 0.0f && rect_dmin(next->x, next->y, rx0, ry0, rx1, ry1) <= dfall_next) dirty = 1;
            for (int i = 0; !dirty && i < nseg; ++i) {
                if (coarse[i]) {
                    if (rect_dmin(prev->x, prev->y, rx0, ry0, rx1, ry1) <= dmax_prev ||
                        rect_dmin(next->x, next->y, rx0, ry0, rx1, ry1) <= dmax_next) dirty = 1;
                } else {
                    if (tri_rect_hit(&tris[i * 12 + 0], rx0, ry0, rx1, ry1) ||
                        tri_rect_hit(&tris[i * 12 + 6], rx0, ry0, rx1, ry1)) dirty = 1;
                }
            }
            b->tab[(size_t)by * b->bw + bx] = (unsigned char)dirty;
        }
    }
    free(tris);
    free(coarse);

    /* Accumulate the sub-threshold falloff drift of unmarked blocks; once the
     * bound on the total drift since the block was last recomputed exceeds
     * SH_TOL, mark it too. Without this the per-frame threshold lets error
     * accumulate frame over frame without bound. */
    float im = next->intensity * moved / next->radius;
    for (int by = 0; by < b->bh; ++by) {
        float ry0 = (float)(by * b->block);
        float ry1 = (float)((by + 1) * b->block < b->h ? (by + 1) * b->block : b->h);
        for (int bx = 0; bx < b->bw; ++bx) {
            size_t idx = (size_t)by * b->bw + bx;
            if (b->tab[idx]) { b->acc[idx] = 0.0f; continue; }
            float delta;
            if (shape_changed) {
                delta = SH_TOL;   /* outside both influence circles |dv| < SH_TOL */
            } else if (moved > 0.0f) {
                float rx0 = (float)(bx * b->block);
                float rx1 = (float)((bx + 1) * b->block < b->w ? (bx + 1) * b->block : b->w);
                float dmin = fminf(rect_dmin(prev->x, prev->y, rx0, ry0, rx1, ry1),
                                   rect_dmin(next->x, next->y, rx0, ry0, rx1, ry1));
                float g = 1.0f + dmin / next->radius;
                delta = im / (g * g);
            } else {
                delta = 0.0f;
            }
            b->acc[idx] += delta;
            if (b->acc[idx] > SH_TOL) { b->tab[idx] = 1; b->acc[idx] = 0.0f; }
        }
    }
    return 0;
}

int sh_render_blocks(ShadowMap *m, const ShBlocks *b, const ShLight *light, const ShSeg *segs, int nseg) {
    if (!m || !b || !b->tab || !light || (nseg > 0 && !segs)) return -1;
    if (b->block <= 0) {
        fprintf(stderr, "sh_render_blocks: parameter 'block' must be > 0 (got %d)\n", b->block);
        return -3;
    }
    int rc = check_light(light);
    if (rc) return rc;
    for (int by = 0; by < b->bh; ++by) {
        int y1 = (by + 1) * b->block < m->h ? (by + 1) * b->block : m->h;
        for (int bx = 0; bx < b->bw; ++bx) {
            if (!b->tab[(size_t)by * b->bw + bx]) continue;
            int x1 = (bx + 1) * b->block < m->w ? (bx + 1) * b->block : m->w;
            for (int y = by * b->block; y < y1; ++y)
                for (int x = bx * b->block; x < x1; ++x)
                    m->buf[(size_t)y * m->w + x] = shade_pixel(light, segs, nseg, x, y);
        }
    }
    return 0;
}

int sh_render_incr(ShadowMap *m, ShBlocks *b, const ShLight *prev, const ShLight *next,
                   const ShSeg *segs, int nseg, ShIncrStats *stats) {
    if (!m || !b) return -1;
    int rc = sh_mark(b, prev, next, segs, nseg);
    if (rc) return rc;
    rc = sh_render_blocks(m, b, next, segs, nseg);
    if (rc) return rc;
    if (stats) {
        int hit = 0;
        for (int i = 0; i < b->bw * b->bh; ++i) hit += b->tab[i] ? 1 : 0;
        stats->bw = b->bw; stats->bh = b->bh; stats->hit = hit; stats->tab = b->tab;
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
