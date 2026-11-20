#include "shadow.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

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

int sh_render(ShadowMap *m, const ShLight *light, const ShSeg *segs, int nseg) {
    if (!m || !light || (nseg > 0 && !segs)) return -1;
    if (light->radius <= 0.0f || light->intensity < 0.0f) return -2;
    for (int y = 0; y < m->h; ++y) {
        for (int x = 0; x < m->w; ++x) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            int shadowed = 0;
            for (int i = 0; i < nseg; ++i)
                if (blocked(&segs[i], light->x, light->y, px, py)) { shadowed = 1; break; }
            float v = 0.0f;
            if (!shadowed) {
                float dx = px - light->x, dy = py - light->y;
                float d = sqrtf(dx * dx + dy * dy);
                float fall = 1.0f / (1.0f + d / light->radius);
                v = light->intensity * fall;
            }
            m->buf[(size_t)y * m->w + x] = v;
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

/* ---- Multi-light -------------------------------------------------------- */

static int fail(ShError *err, int code, int light, int other,
                const char *what, int index) {
    if (err) {
        err->code = code;
        err->light = light;
        err->other = other;
        if (index >= 0)
            snprintf(err->msg, sizeof err->msg, "%s: light #%d", what, index);
        else
            snprintf(err->msg, sizeof err->msg, "%s", what);
    }
    return code;
}

static void clear_stats(ShStats *st) {
    if (!st) return;
    st->passes = 0;
    st->temp_pixels = 0;
    st->blend_ops = 0;
}

int sh_render_lights(ShadowMap *m, const ShLight *lights, int nlight,
                     const ShSeg *segs, int nseg, int enable_multi,
                     ShError *err, ShStats *stats) {
    clear_stats(stats);           /* stats describe a completed render only */
    if (err) { err->code = SH_OK; err->light = -1; err->other = -1; err->msg[0] = '\0'; }

    if (!m || !m->buf || m->w <= 0 || m->h <= 0) {
        return fail(err, SH_EARG, -1, -1, "invalid shadow map", -1);
    }
    if (nlight < 0 || nseg < 0 || (nseg > 0 && !segs)) {
        return fail(err, SH_EARG, -1, -1, "invalid argument", -1);
    }
    if (nlight == 0) {
        return fail(err, SH_ENOLIGHT, -1, -1, "no lights supplied", -1);
    }
    if (!lights) {
        return fail(err, SH_EARG, -1, -1, "null light array", -1);
    }

    /* Validation happens before any rendering: all lights first (so the
     * reported index is the first bad one), then the overlap check. */
    for (int i = 0; i < nlight; ++i) {
        const ShLight *l = &lights[i];
        if (!isfinite(l->x) || !isfinite(l->y) || !isfinite(l->radius) ||
            !isfinite(l->intensity) || l->radius <= 0.0f || l->intensity < 0.0f) {
            return fail(err, SH_EBADLIGHT, i, -1,
                        "invalid light (radius must be > 0, intensity >= 0)",
                        i);
        }
    }
    for (int i = 1; i < nlight; ++i) {
        for (int j = 0; j < i; ++j) {
            if (lights[i].x == lights[j].x && lights[i].y == lights[j].y) {
                if (err) {
                    err->code = SH_EDUP;
                    err->light = i;
                    err->other = j;
                    snprintf(err->msg, sizeof err->msg,
                             "lights #%d and #%d share position (%.3f, %.3f)",
                             j, i, (double)lights[i].x, (double)lights[i].y);
                }
                return SH_EDUP;
            }
        }
    }

    /* Legacy path: identical call sequence and arithmetic to the old API. */
    if (!enable_multi) {
        int rc = sh_render(m, &lights[0], segs, nseg);
        if (rc != SH_OK) return fail(err, rc, 0, -1, "legacy render failed", 0);
        if (stats) {
            stats->passes = 1;
            stats->temp_pixels = 0;
            stats->blend_ops = 0;
        }
        return SH_OK;
    }

    const size_t wh = (size_t)m->w * (size_t)m->h;
    const size_t n = (size_t)nlight;
    float *tmp = (float*)malloc(n * wh * sizeof(float));
    if (!tmp) return fail(err, SH_EARG, -1, -1, "out of memory for light maps", -1);

    /*
     * Stage 1 - one isolated render per light, in ascending light order.
     * Each temp map is exactly what sh_render() would produce for that light
     * alone: radiance_i(p) = intensity_i * fall_i(p) * visibility_i(p).
     */
    for (int i = 0; i < nlight; ++i) {
        ShadowMap t;
        t.w = m->w;
        t.h = m->h;
        t.buf = tmp + (size_t)i * wh;
        int rc = sh_render(&t, &lights[i], segs, nseg);
        if (rc != SH_OK) {
            free(tmp);
            return fail(err, rc, i, -1, "light pass failed", i);
        }
    }

    /*
     * Stage 2 - normalised composite, ascending light index order:
     *     denom(p) = sum_i intensity_i          (i = 0, 1, ..., nlight-1)
     *     num(p)   = sum_i radiance_i(p)          (same order)
     *     out(p)   = denom(p) > 0 ? num/denom : 0
     */
    for (size_t p = 0; p < wh; ++p) {
        float num = 0.0f, den = 0.0f;
        for (int i = 0; i < nlight; ++i) {
            num += tmp[(size_t)i * wh + p];
            den += lights[i].intensity;
        }
        m->buf[p] = den > 0.0f ? num / den : 0.0f;
    }

    free(tmp);
    if (stats) {
        stats->passes = nlight;
        stats->temp_pixels = n * wh;
        stats->blend_ops = n * wh;
    }
    return SH_OK;
}
