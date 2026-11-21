#define _POSIX_C_SOURCE 199309L
#include "shadow.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

/* ---- 环境光染色阴影 ---- */

#define SH_TINT_EXACT_SAMPLES 8 /* 低速档：每像素 8x8 子光线穷举 */
#define SH_TINT_FAST_SAMPLES 2  /* 高速档：每像素 2x2 子光线 + 色表 */

static float sh_clampf(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

static double sh_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

int sh_tint_init(ShTintState *st) {
    if (!st) return -1;
    memset(st, 0, sizeof(*st));
    return 0;
}

const ShColor *sh_tint_table(const ShTintState *st, int *n) {
    if (n) *n = SH_TINT_TABLE_SIZE;
    if (!st || !st->table_valid) return NULL;
    return st->table;
}

/* 段一：按环境色算阴影色 */
ShColor sh_tint_shadow_color(const ShTintConfig *cfg) {
    ShColor c = {0.0f, 0.0f, 0.0f};
    if (!cfg) return c;
    if (cfg->has_shadow_color) c = cfg->shadow_color;
    else if (cfg->has_ambient) c = cfg->ambient;
    c.r = sh_clampf(c.r); c.g = sh_clampf(c.g); c.b = sh_clampf(c.b);
    return c;
}

/* 段二：按遮挡比例混色 */
ShColor sh_tint_blend(ShColor lit, ShColor shadow, float occ) {
    ShColor out;
    occ = sh_clampf(occ);
    out.r = lit.r * (1.0f - occ) + shadow.r * occ;
    out.g = lit.g * (1.0f - occ) + shadow.g * occ;
    out.b = lit.b * (1.0f - occ) + shadow.b * occ;
    return out;
}

/* 段三：写回缓冲 */
void sh_tint_write(float *rgb_out, long idx, ShColor c) {
    rgb_out[3 * idx + 0] = c.r;
    rgb_out[3 * idx + 1] = c.g;
    rgb_out[3 * idx + 2] = c.b;
}

/* 每像素 nxn 子光线穷举遮挡比例 */
static float sh_occlusion(const ShLight *light, const ShSeg *segs, int nseg,
                          int x, int y, int n) {
    int hit = 0;
    for (int sy = 0; sy < n; ++sy) {
        for (int sx = 0; sx < n; ++sx) {
            float px = (float)x + ((float)sx + 0.5f) / (float)n;
            float py = (float)y + ((float)sy + 0.5f) / (float)n;
            for (int i = 0; i < nseg; ++i)
                if (blocked(&segs[i], light->x, light->y, px, py)) { ++hit; break; }
        }
    }
    return (float)hit / (float)(n * n);
}

static void sh_tint_build_table(ShTintState *st, ShColor shadow) {
    for (int i = 0; i < SH_TINT_TABLE_SIZE; ++i) {
        float t = (float)i / (float)(SH_TINT_TABLE_SIZE - 1);
        st->table[i].r = shadow.r * t;
        st->table[i].g = shadow.g * t;
        st->table[i].b = shadow.b * t;
    }
    st->table_color = shadow;
    st->table_valid = 1;
    ++st->table_rebuilds;
}

static int sh_tint_validate(const ShadowMap *m, const ShLight *light,
                            const ShSeg *segs, int nseg,
                            const ShTintConfig *cfg) {
    if (!m || !m->buf || !light || (nseg > 0 && !segs) || !cfg) return -1;
    if (light->radius <= 0.0f || light->intensity < 0.0f) return -2;
    if (cfg->enabled && cfg->has_ambient && cfg->has_shadow_color) {
        fprintf(stderr,
                "sh_tint: conflicting config: 'ambient' (has_ambient) vs "
                "'shadow_color' (has_shadow_color)\n");
        return -3;
    }
    if (cfg->enabled && cfg->mode != SH_TINT_EXACT && cfg->mode != SH_TINT_FAST)
        return -4;
    return 0;
}

/* 环境色变化单独统计：与上次见到的环境色不同则计数 */
static void sh_tint_track_ambient(ShTintState *st, const ShTintConfig *cfg) {
    if (!cfg->enabled || !cfg->has_ambient) return;
    if (!st->has_ambient || st->ambient.r != cfg->ambient.r ||
        st->ambient.g != cfg->ambient.g || st->ambient.b != cfg->ambient.b) {
        ++st->ambient_changes;
        st->ambient = cfg->ambient;
        st->has_ambient = 1;
    }
}

int sh_tint_render(ShTintState *st, const ShadowMap *m, const ShLight *light,
                   const ShSeg *segs, int nseg, const ShTintConfig *cfg,
                   float *rgb_out, ShTintStats *stats) {
    int rc = sh_tint_validate(m, light, segs, nseg, cfg);
    if (rc != 0) return rc;
    if (!st || !rgb_out) return -1;
    if (stats) memset(stats, 0, sizeof(*stats));

    sh_tint_track_ambient(st, cfg);
    long npix = (long)m->w * m->h;
    double t0 = sh_now_ms();
    double occ_sum = 0.0;

    if (!cfg->enabled) {
        /* 开关为假：行为不变，rgb 三通道复制原灰度亮度 */
        for (long i = 0; i < npix; ++i) {
            ShColor g = {m->buf[i], m->buf[i], m->buf[i]};
            sh_tint_write(rgb_out, i, g);
        }
    } else {
        ShColor shadow = sh_tint_shadow_color(cfg); /* 段一 */
        int fast = (cfg->mode == SH_TINT_FAST);
        int n = fast ? SH_TINT_FAST_SAMPLES : SH_TINT_EXACT_SAMPLES;
        if (fast && (!st->table_valid || st->table_color.r != shadow.r ||
                     st->table_color.g != shadow.g || st->table_color.b != shadow.b))
            sh_tint_build_table(st, shadow);
        for (int y = 0; y < m->h; ++y) {
            for (int x = 0; x < m->w; ++x) {
                long idx = (long)y * m->w + x;
                float occ = sh_occlusion(light, segs, nseg, x, y, n);
                occ_sum += occ;
                float v = m->buf[idx];
                ShColor lit = {v, v, v}, out;
                if (fast) { /* 段二（高速）：色表查染色项，比例量化 */
                    int ti = (int)(occ * (float)(SH_TINT_TABLE_SIZE - 1) + 0.5f);
                    float t = (float)ti / (float)(SH_TINT_TABLE_SIZE - 1);
                    out.r = lit.r * (1.0f - t) + st->table[ti].r;
                    out.g = lit.g * (1.0f - t) + st->table[ti].g;
                    out.b = lit.b * (1.0f - t) + st->table[ti].b;
                } else {    /* 段二（低速）：逐像素精确混色 */
                    out = sh_tint_blend(lit, shadow, occ);
                }
                sh_tint_write(rgb_out, idx, out); /* 段三 */
            }
        }
    }

    if (stats) {
        double dt = sh_now_ms() - t0;
        stats->pixels = npix;
        stats->blend_ratio = cfg->enabled ? occ_sum / (double)npix : 0.0;
        if (cfg->enabled) {
            if (cfg->mode == SH_TINT_FAST) stats->time_fast_ms = dt;
            else stats->time_exact_ms = dt;
        }
        stats->ambient_changes = st->ambient_changes;
        stats->table_rebuilds = st->table_rebuilds;
    }
    return 0;
}

int sh_tint_compare(ShTintState *st, const ShadowMap *m, const ShLight *light,
                    const ShSeg *segs, int nseg, const ShTintConfig *cfg,
                    ShTintStats *stats) {
    int rc = sh_tint_validate(m, light, segs, nseg, cfg);
    if (rc != 0) return rc;
    if (!st || !stats) return -1;
    long npix = (long)m->w * m->h;
    float *exact = (float *)malloc((size_t)npix * 3 * sizeof(float));
    float *fast = (float *)malloc((size_t)npix * 3 * sizeof(float));
    if (!exact || !fast) { free(exact); free(fast); return -1; }

    ShTintConfig ec = *cfg, fc = *cfg;
    ec.enabled = 1; ec.mode = SH_TINT_EXACT;
    fc.enabled = 1; fc.mode = SH_TINT_FAST;
    ShTintStats es, fs;
    rc = sh_tint_render(st, m, light, segs, nseg, &ec, exact, &es);
    if (rc == 0) rc = sh_tint_render(st, m, light, segs, nseg, &fc, fast, &fs);
    if (rc == 0) {
        double err = 0.0;
        for (long i = 0; i < npix * 3; ++i) err += fabs((double)exact[i] - fast[i]);
        memset(stats, 0, sizeof(*stats));
        stats->pixels = npix;
        stats->blend_ratio = es.blend_ratio;
        stats->tint_error = err / (double)(npix * 3); /* 高速档 vs 穷举 */
        stats->time_exact_ms = es.time_exact_ms;
        stats->time_fast_ms = fs.time_fast_ms;
        stats->ambient_changes = st->ambient_changes;
        stats->table_rebuilds = st->table_rebuilds;
    }
    free(exact);
    free(fast);
    return rc;
}
