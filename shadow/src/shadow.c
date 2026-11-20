#include "shadow.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* ---- base map helpers --------------------------------------------------- */

int sh_init(ShadowMap *m, int w, int h) {
    if (!m || w <= 0 || h <= 0) return -1;
    m->buf = (float*)calloc((size_t)w * (size_t)h, sizeof(float));
    if (!m->buf) return -1;
    m->w = w; m->h = h;
    return 0;
}

void sh_free(ShadowMap *m) {
    if (m) { free(m->buf); m->buf = NULL; m->w = 0; m->h = 0; }
}

int sh_clear(ShadowMap *m) {
    if (!m) return -1;
    for (int i = 0; i < m->w * m->h; ++i) m->buf[i] = 0.0f;
    return 0;
}

static int blocked(const ShSeg *s, float lx, float ly, float px, float py,
                   long *ray_tests) {
    float rx = px - lx, ry = py - ly;
    float sx = s->x1 - s->x0, sy = s->y1 - s->y0;
    float den = rx * sy - ry * sx;
    if (ray_tests) ++*ray_tests;
    if (fabsf(den) < 1e-9f) return 0;
    float t = ((s->x0 - lx) * sy - (s->y0 - ly) * sx) / den;
    float u = ((s->x0 - lx) * ry - (s->y0 - ly) * rx) / den;
    return t > 0.0f && t < 1.0f && u >= 0.0f && u <= 1.0f;
}

/* Render one rectangular world region [x0,y0)-[x1,y1) into m. The map's
 * texel (0,0) samples world point (x0+0.5*sx, y0+0.5*sy); this is exactly
 * the math sh_render used, so a full-map region reproduces it bit for bit. */
static int render_region(ShadowMap *m, const ShLight *light,
                         const ShSeg *segs, int nseg,
                         float x0, float y0, float x1, float y1,
                         long *ray_tests, long *texels) {
    float sx = (x1 - x0) / (float)m->w;
    float sy = (y1 - y0) / (float)m->h;
    for (int y = 0; y < m->h; ++y) {
        float py = y0 + ((float)y + 0.5f) * sy;
        for (int x = 0; x < m->w; ++x) {
            float px = x0 + ((float)x + 0.5f) * sx;
            int shadowed = 0;
            for (int i = 0; i < nseg; ++i)
                if (blocked(&segs[i], light->x, light->y, px, py, ray_tests)) {
                    shadowed = 1; break;
                }
            float v = 0.0f;
            if (!shadowed) {
                float dx = px - light->x, dy = py - light->y;
                float d = sqrtf(dx * dx + dy * dy);
                float fall = 1.0f / (1.0f + d / light->radius);
                v = light->intensity * fall;
            }
            m->buf[(size_t)y * m->w + x] = v;
            if (texels) ++*texels;
        }
    }
    return 0;
}

int sh_render(ShadowMap *m, const ShLight *light, const ShSeg *segs, int nseg) {
    if (!m || !light || (nseg > 0 && !segs)) return -1;
    if (light->radius <= 0.0f || light->intensity < 0.0f) return -2;
    return render_region(m, light, segs, nseg,
                         0.0f, 0.0f, (float)m->w, (float)m->h, NULL, NULL);
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

/* ---- cascaded shadow maps ---------------------------------------------- */

const char *sh_csm_strerror(int code) {
    switch (code) {
    case SH_OK:         return "ok";
    case SH_ERR_ARG:    return "invalid argument: null cascade/cfg pointer";
    case SH_ERR_TIERS:  return "invalid config: field 'tiers' must be > 0";
    case SH_ERR_RANGE:  return "invalid config: field 'near/far' requires 0 <= near < far";
    case SH_ERR_RES:    return "invalid config: field 'res' must be > 0";
    case SH_ERR_MEM:    return "out of memory";
    case SH_ERR_LIGHT:  return "invalid light: radius must be > 0 and intensity >= 0";
    default:            return "unknown error";
    }
}

int sh_csm_build(ShCascade *c, float camx, float camy,
                 const ShTierCfg *cfg, int ntiers) {
    if (!c) return SH_ERR_ARG;
    c->camx = c->camy = 0.0f; c->n = 0;
    c->tiers = NULL; c->maps = NULL;
    if (!cfg) return SH_ERR_ARG;
    if (ntiers <= 0) return SH_ERR_TIERS;

    ShTier *tiers = (ShTier*)calloc((size_t)ntiers, sizeof(ShTier));
    ShadowMap *maps = (ShadowMap*)calloc((size_t)ntiers, sizeof(ShadowMap));
    if (!tiers || !maps) { free(tiers); free(maps); return SH_ERR_MEM; }

    int built = 0;
    for (int i = 0; i < ntiers; ++i) {
        if (!(cfg[i].near >= 0.0f) || !(cfg[i].far > cfg[i].near)) {
            sh_csm_free(&(ShCascade){camx, camy, built, tiers, maps});
            return SH_ERR_RANGE;
        }
        if (cfg[i].res <= 0) {
            sh_csm_free(&(ShCascade){camx, camy, built, tiers, maps});
            return SH_ERR_RES;
        }
        if (sh_init(&maps[i], cfg[i].res, cfg[i].res) != 0) {
            sh_csm_free(&(ShCascade){camx, camy, built, tiers, maps});
            return SH_ERR_MEM;
        }
        float far = cfg[i].far;
        tiers[i].index = i;
        tiers[i].near = cfg[i].near;
        tiers[i].far = far;
        tiers[i].res = cfg[i].res;
        tiers[i].x0 = camx - far;
        tiers[i].y0 = camy - far;
        tiers[i].x1 = camx + far;
        tiers[i].y1 = camy + far;
        tiers[i].texel = (2.0f * far) / (float)cfg[i].res;
        ++built;
    }

    c->camx = camx; c->camy = camy;
    c->n = ntiers; c->tiers = tiers; c->maps = maps;
    return SH_OK;
}

void sh_csm_free(ShCascade *c) {
    if (!c) return;
    if (c->maps) {
        for (int i = 0; i < c->n; ++i) sh_free(&c->maps[i]);
        free(c->maps);
    }
    free(c->tiers);
    c->tiers = NULL; c->maps = NULL; c->n = 0;
}

int sh_csm_render(ShCascade *c, const ShLight *light,
                  const ShSeg *segs, int nseg, ShCsmStats *st) {
    if (!c || !light || (nseg > 0 && !segs)) return SH_ERR_ARG;
    if (light->radius <= 0.0f || light->intensity < 0.0f) return SH_ERR_LIGHT;
    long rays = 0, tex = 0;
    for (int i = 0; i < c->n; ++i) {
        const ShTier *t = &c->tiers[i];
        int rc = render_region(&c->maps[i], light, segs, nseg,
                               t->x0, t->y0, t->x1, t->y1, &rays, &tex);
        if (rc != 0) return SH_ERR_ARG;
    }
    if (st) {
        st->ray_tests += rays;
        st->texels += tex;
    }
    return SH_OK;
}

int sh_csm_pick(const ShCascade *c, float d) {
    if (!c) return -1;
    for (int i = 0; i < c->n; ++i)
        if (d >= c->tiers[i].near && d < c->tiers[i].far) return i;
    return -1;
}

int sh_csm_compose(const ShCascade *c, ShadowMap *out,
                   float ox0, float oy0, float ox1, float oy1,
                   ShCsmStats *st) {
    if (!c || !out || !out->buf || !(ox1 > ox0) || !(oy1 > oy0))
        return SH_ERR_ARG;
    long composed = 0, uncovered = 0;
    float sx = (ox1 - ox0) / (float)out->w;
    float sy = (oy1 - oy0) / (float)out->h;
    for (int y = 0; y < out->h; ++y) {
        float py = oy0 + ((float)y + 0.5f) * sy;
        for (int x = 0; x < out->w; ++x) {
            float px = ox0 + ((float)x + 0.5f) * sx;
            float dx = px - c->camx, dy = py - c->camy;
            float d = sqrtf(dx * dx + dy * dy);
            int ti = sh_csm_pick(c, d);
            float v = 0.0f;
            if (ti >= 0) {
                const ShTier *t = &c->tiers[ti];
                const ShadowMap *m = &c->maps[ti];
                /* world point -> tier texel (nearest), clamped to the map */
                int tx = (int)((px - t->x0) / t->texel);
                int ty = (int)((py - t->y0) / t->texel);
                if (tx < 0) tx = 0; else if (tx >= m->w) tx = m->w - 1;
                if (ty < 0) ty = 0; else if (ty >= m->h) ty = m->h - 1;
                v = m->buf[(size_t)ty * m->w + tx];
                ++composed;
            } else {
                ++uncovered;
            }
            out->buf[(size_t)y * out->w + x] = v;
        }
    }
    if (st) { st->composed += composed; st->uncovered += uncovered; }
    return SH_OK;
}

int sh_csm_table(const ShCascade *c, const ShTier **out, int *n) {
    if (!c || !out || !n) return SH_ERR_ARG;
    *out = c->tiers;
    *n = c->n;
    return SH_OK;
}
