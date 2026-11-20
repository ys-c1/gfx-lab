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

static int blocked(float x0, float y0, float x1, float y1, float lx, float ly, float px, float py) {
    float rx = px - lx, ry = py - ly;
    float sx = x1 - x0, sy = y1 - y0;
    float den = rx * sy - ry * sx;
    if (fabsf(den) < 1e-9f) return 0;
    float t = ((x0 - lx) * sy - (y0 - ly) * sx) / den;
    float u = ((x0 - lx) * ry - (y0 - ly) * rx) / den;
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
                if (blocked(segs[i].x0, segs[i].y0, segs[i].x1, segs[i].y1,
                            light->x, light->y, px, py)) { shadowed = 1; break; }
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

int sh_render_t(ShadowMap *m, const ShLight *light, const ShSegT *segs, int nseg, ShStats *st) {
    if (!m || !light || (nseg > 0 && !segs)) return -1;
    if (light->radius <= 0.0f || light->intensity < 0.0f) return -2;
    for (int i = 0; i < nseg; ++i) {
        if (!(segs[i].tau >= 0.0f && segs[i].tau <= 1.0f)) {
            fprintf(stderr, "sh_render_t: seg %d: tau %.6g out of [0,1]\n", i, (double)segs[i].tau);
            return SH_ERR_TAU(i);
        }
        float dx = segs[i].x1 - segs[i].x0, dy = segs[i].y1 - segs[i].y0;
        if (dx * dx + dy * dy < 1e-12f) {
            fprintf(stderr, "sh_render_t: seg %d: degenerate segment (zero length)\n", i);
            return SH_ERR_DEGEN(i);
        }
    }
    ShStats acc;
    if (st) { memset(&acc, 0, sizeof(acc)); }
    for (int y = 0; y < m->h; ++y) {
        for (int x = 0; x < m->w; ++x) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            /* 段 1：沿光线按遮挡体透光率累乘（float 与 double 双路，求累乘误差） */
            int layers = 0;
            float tf = 1.0f;
            double td = 1.0;
            for (int i = 0; i < nseg; ++i)
                if (blocked(segs[i].x0, segs[i].y0, segs[i].x1, segs[i].y1,
                            light->x, light->y, px, py)) {
                    ++layers;
                    tf *= segs[i].tau;
                    td *= (double)segs[i].tau;
                }
            /* 段 2：合成阴影亮度 */
            float dx = px - light->x, dy = py - light->y;
            float d = sqrtf(dx * dx + dy * dy);
            float fall = 1.0f / (1.0f + d / light->radius);
            float v = light->intensity * fall * tf;
            m->buf[(size_t)y * m->w + x] = v;
            if (st) {
                float err = fabsf(tf - (float)td);
                if (err > acc.max_trans_err) acc.max_trans_err = err;
                if (layers == 0) ++acc.n_lit;
                else if (layers == 1) { ++acc.n_single; acc.sum_single += v; }
                else { ++acc.n_overlap; acc.sum_overlap += v; }
            }
        }
    }
    if (st) {
        acc.mean_single = acc.n_single ? acc.sum_single / acc.n_single : 0.0;
        acc.mean_overlap = acc.n_overlap ? acc.sum_overlap / acc.n_overlap : 0.0;
        *st = acc;
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
