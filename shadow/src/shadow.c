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

static void render_legacy(ShadowMap *m, const ShLight *light, const ShSeg *segs, int nseg) {
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
}

int sh_render(ShadowMap *m, const ShLight *light, const ShSeg *segs, int nseg) {
    if (!m || !light || (nseg > 0 && !segs)) return -1;
    if (light->radius <= 0.0f || light->intensity < 0.0f) return -2;
    render_legacy(m, light, segs, nseg);
    return 0;
}

void sh_config_default(ShConfig *c) {
    if (!c) return;
    c->segmented = 0;
    c->block = 4;
    c->threshold = 0.5f;
}

int sh_config_set(ShConfig *c, const char *field, ShValue v, const char **bad_field) {
    if (bad_field) *bad_field = NULL;
    if (!c || !field) return -1;
    if (strcmp(field, "segmented") == 0) {
        if (v.t != SH_T_INT) { if (bad_field) *bad_field = "segmented"; return -2; }
        if (v.i != 0 && v.i != 1) { if (bad_field) *bad_field = "segmented"; return -3; }
        c->segmented = v.i;
        return 0;
    }
    if (strcmp(field, "block") == 0) {
        if (v.t != SH_T_INT) { if (bad_field) *bad_field = "block"; return -2; }
        if (v.i < 1) { if (bad_field) *bad_field = "block"; return -3; }
        c->block = v.i;
        return 0;
    }
    if (strcmp(field, "threshold") == 0) {
        if (v.t != SH_T_FLOAT) { if (bad_field) *bad_field = "threshold"; return -2; }
        if (v.f < 0.0f || v.f > 1.0f) { if (bad_field) *bad_field = "threshold"; return -3; }
        c->threshold = v.f;
        return 0;
    }
    return -1;
}

/* 三段式：段 s 的归约网格在 grid 中的偏移 */
static int render_segmented(ShadowMap *m, const ShLight *light, const ShSeg *segs, int nseg,
                            const ShConfig *cfg, ShStats *st) {
    int w = m->w, h = m->h, B = cfg->block;
    int bw = (w + B - 1) / B, bh = (h + B - 1) / B;
    size_t ncell = (size_t)bw * (size_t)bh;
    float *local = (float*)malloc((size_t)w * (size_t)h * sizeof(float));
    float *grid = (float*)malloc((nseg > 0 ? (size_t)nseg : 1) * ncell * sizeof(float));
    if (!local || !grid) { free(local); free(grid); return -4; }

    if (st) {
        st->segments = nseg;
        st->local_bytes = (long)((size_t)w * (size_t)h * sizeof(float));
        st->reduced_bytes = (long)((size_t)nseg * ncell * sizeof(float));
    }

    /* 阶段一 + 阶段二：按段写局部缓冲，再按段归约 */
    for (int s = 0; s < nseg; ++s) {
        float *g = grid + (size_t)s * ncell;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                local[(size_t)y * w + x] =
                    blocked(&segs[s], light->x, light->y,
                            (float)x + 0.5f, (float)y + 0.5f) ? 1.0f : 0.0f;
        for (int by = 0; by < bh; ++by) {
            for (int bx = 0; bx < bw; ++bx) {
                int x0 = bx * B, y0 = by * B;
                int x1 = x0 + B < w ? x0 + B : w;
                int y1 = y0 + B < h ? y0 + B : h;
                float mx = 0.0f;
                for (int y = y0; y < y1; ++y)
                    for (int x = x0; x < x1; ++x)
                        if (local[(size_t)y * w + x] > mx) mx = local[(size_t)y * w + x];
                g[(size_t)by * bw + bx] = mx;
                if (st) {
                    ++st->reductions;
                    for (int y = y0; y < y1; ++y)
                        for (int x = x0; x < x1; ++x) {
                            float d = fabsf(local[(size_t)y * w + x] - mx);
                            st->pre_post_diff += (double)d;
                            if (d != 0.0f) ++st->pre_post_cells;
                        }
                }
            }
        }
    }

    /* 阶段三：按归约结果合成 */
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            size_t cell = (size_t)(y / B) * bw + (size_t)(x / B);
            int shadowed = 0;
            for (int s = 0; s < nseg; ++s)
                if (grid[(size_t)s * ncell + cell] >= cfg->threshold) { shadowed = 1; break; }
            float v = 0.0f;
            if (!shadowed) {
                float px = (float)x + 0.5f, py = (float)y + 0.5f;
                float dx = px - light->x, dy = py - light->y;
                float d = sqrtf(dx * dx + dy * dy);
                float fall = 1.0f / (1.0f + d / light->radius);
                v = light->intensity * fall;
            }
            m->buf[(size_t)y * w + x] = v;
        }
    }

    free(local);
    free(grid);
    return 0;
}

int sh_render_ex(ShadowMap *m, const ShLight *light, const ShSeg *segs, int nseg,
                 const ShConfig *cfg, ShStats *stats) {
    if (!m || !light || (nseg > 0 && !segs)) return -1;
    if (light->radius <= 0.0f || light->intensity < 0.0f) return -2;
    if (stats) memset(stats, 0, sizeof(*stats));
    if (!cfg || !cfg->segmented) {
        render_legacy(m, light, segs, nseg);
        if (stats) stats->segments = nseg;
        return 0;
    }
    if (cfg->block < 1 || cfg->threshold < 0.0f || cfg->threshold > 1.0f) return -3;
    return render_segmented(m, light, segs, nseg, cfg, stats);
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
