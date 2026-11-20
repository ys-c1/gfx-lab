#include "shadow.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

const char *sh_err_field(int rc) {
    switch (rc) {
        case SH_OK: return "none";
        case SH_ERR_ARG: return "args";
        case SH_ERR_RADIUS: return "radius";
        case SH_ERR_SIZE: return "size";
        case SH_ERR_SAMPLES: return "samples";
        default: return "unknown";
    }
}

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

static int sh_area_check(const ShAreaLight *l) {
    if (!l) return SH_ERR_ARG;
    if (l->radius <= 0.0f) return SH_ERR_RADIUS;
    if (l->size <= 0.0f) return SH_ERR_SIZE;
    if (l->samples <= 0) return SH_ERR_SAMPLES;
    return SH_OK;
}

int sh_area_points(const ShAreaLight *l, float *xy, int cap, int *n) {
    int rc = sh_area_check(l);
    if (rc != SH_OK) return rc;
    if (!n || cap < 0 || (cap > 0 && !xy)) return SH_ERR_ARG;
    const float golden = 2.39996322972865332f; /* 黄金角，向日葵螺旋 */
    int kmax = l->samples < cap ? l->samples : cap;
    for (int k = 0; k < kmax; ++k) {
        float r = l->size * sqrtf(((float)k + 0.5f) / (float)l->samples);
        float a = (float)k * golden;
        xy[2 * k]     = l->x + r * cosf(a);
        xy[2 * k + 1] = l->y + r * sinf(a);
    }
    *n = l->samples;
    return SH_OK;
}

int sh_render_area(ShadowMap *m, const ShAreaLight *l, const ShSeg *segs, int nseg) {
    if (!m || (nseg > 0 && !segs)) return SH_ERR_ARG;
    int rc = sh_area_check(l);
    if (rc != SH_OK) return rc;
    float *pts = (float*)malloc(sizeof(float) * 2 * (size_t)l->samples);
    if (!pts) return SH_ERR_ARG;
    int n = 0;
    sh_area_points(l, pts, l->samples, &n);
    for (int y = 0; y < m->h; ++y) {
        for (int x = 0; x < m->w; ++x) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            int vis = 0;
            for (int k = 0; k < n; ++k) {
                int occ = 0;
                for (int i = 0; i < nseg; ++i)
                    if (blocked(&segs[i], pts[2 * k], pts[2 * k + 1], px, py)) { occ = 1; break; }
                if (!occ) ++vis;
            }
            float cover = (float)vis / (float)n;
            float dx = px - l->x, dy = py - l->y;
            float d = sqrtf(dx * dx + dy * dy);
            float fall = 1.0f / (1.0f + d / l->radius);
            m->buf[(size_t)y * m->w + x] = l->intensity * fall * cover;
        }
    }
    free(pts);
    return SH_OK;
}

int sh_penumbra_width(const ShadowMap *m, const ShAreaLight *l, float *width_px) {
    if (!m || !width_px) return SH_ERR_ARG;
    int rc = sh_area_check(l);
    if (rc != SH_OK) return rc;
    /* 覆盖率以 1/samples 量化，取半个量化步作容差 */
    float tol = 0.5f / (float)l->samples;
    float best = 0.0f;
    for (int y = 0; y < m->h; ++y) {
        int run = 0;
        for (int x = 0; x < m->w; ++x) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float dx = px - l->x, dy = py - l->y;
            float d = sqrtf(dx * dx + dy * dy);
            float full = l->intensity / (1.0f + d / l->radius);
            float v = m->buf[(size_t)y * m->w + x];
            int pen = v > full * tol && v < full * (1.0f - tol);
            if (pen) { ++run; if ((float)run > best) best = (float)run; }
            else run = 0;
        }
    }
    *width_px = best;
    return SH_OK;
}
