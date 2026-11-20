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

int sh_render_off(ShadowMap *m, const ShLight *light, const ShSeg *segs, int nseg,
                  float ox, float oy) {
    if (!m || !light || (nseg > 0 && !segs)) return -1;
    if (light->radius <= 0.0f || light->intensity < 0.0f) return -2;
    float lx = light->x + ox, ly = light->y + oy;
    for (int y = 0; y < m->h; ++y) {
        for (int x = 0; x < m->w; ++x) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            int shadowed = 0;
            for (int i = 0; i < nseg; ++i)
                if (blocked(&segs[i], lx, ly, px, py)) { shadowed = 1; break; }
            float v = 0.0f;
            if (!shadowed) {
                float dx = px - lx, dy = py - ly;
                float d = sqrtf(dx * dx + dy * dy);
                float fall = 1.0f / (1.0f + d / light->radius);
                v = light->intensity * fall;
            }
            m->buf[(size_t)y * m->w + x] = v;
        }
    }
    return 0;
}

int sh_render(ShadowMap *m, const ShLight *light, const ShSeg *segs, int nseg) {
    return sh_render_off(m, light, segs, nseg, 0.0f, 0.0f);
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

/* ---- 时序稳定 ---- */

int sh_stab_init(ShStab *s) {
    if (!s) return SH_STAB_E_ARG;
    s->has_prev = 0;
    s->prev_x = s->prev_y = 0.0f;
    s->n_jump = 0;
    s->jump_sum = s->jump_max = s->jump_last = 0.0;
    s->curve = NULL;
    s->curve_len = s->curve_cap = 0;
    return SH_STAB_OK;
}

void sh_stab_free(ShStab *s) {
    if (!s) return;
    free(s->curve);
    sh_stab_init(s);
}

int sh_stab_seed(ShStab *s, float x, float y) {
    if (!s) return SH_STAB_E_ARG;
    s->prev_x = x; s->prev_y = y;
    s->has_prev = 1;
    return SH_STAB_OK;
}

static int curve_push(ShStab *s, const ShStabSample *sm) {
    if (s->curve_len == s->curve_cap) {
        size_t cap = s->curve_cap ? s->curve_cap * 2 : 64;
        ShStabSample *p = (ShStabSample*)realloc(s->curve, cap * sizeof(*p));
        if (!p) return SH_STAB_E_MEM;
        s->curve = p; s->curve_cap = cap;
    }
    s->curve[s->curve_len++] = *sm;
    return SH_STAB_OK;
}

int sh_stab_frame(ShStab *s, const ShStabCfg *cfg,
                  float raw_x, float raw_y,
                  float slope_x, float slope_y,
                  ShStabSample *out) {
    if (!s || !cfg || !out) return SH_STAB_E_ARG;
    if (!(cfg->k >= 0.0f && cfg->k <= 1.0f)) return SH_STAB_E_K;      /* NaN 也在此拦截 */
    if (!(cfg->slope_k >= 0.0f)) return SH_STAB_E_SLOPE;
    float ox, oy;
    if (cfg->suppress) {
        if (!s->has_prev) return SH_STAB_E_PREV;
        /* 段1：按前帧偏移做指数平滑 */
        float smx = s->prev_x + cfg->k * (raw_x - s->prev_x);
        float smy = s->prev_y + cfg->k * (raw_y - s->prev_y);
        /* 段2：按斜率修正 */
        ox = smx + cfg->slope_k * slope_x;
        oy = smy + cfg->slope_k * slope_y;
    } else {
        ox = raw_x; oy = raw_y; /* 不抑制：输出=原始偏移，结果与未启用相同 */
    }
    /* 段3：跳变量统计（本帧原始偏移 vs 前帧输出偏移） */
    float jump = 0.0f;
    if (s->has_prev) {
        float jx = raw_x - s->prev_x, jy = raw_y - s->prev_y;
        jump = sqrtf(jx * jx + jy * jy);
        s->n_jump++;
        s->jump_sum += jump;
        if (jump > s->jump_max) s->jump_max = jump;
        s->jump_last = jump;
    }
    out->raw_x = raw_x; out->raw_y = raw_y;
    out->out_x = ox;    out->out_y = oy;
    out->jump = jump;
    int rc = curve_push(s, out);
    if (rc != SH_STAB_OK) return rc;
    s->prev_x = ox; s->prev_y = oy;
    s->has_prev = 1;
    return SH_STAB_OK;
}

int sh_stab_stats(const ShStab *s, ShStabStats *out) {
    if (!s || !out) return SH_STAB_E_ARG;
    out->frames = s->n_jump;
    out->mean = s->n_jump ? s->jump_sum / (double)s->n_jump : 0.0;
    out->max = s->jump_max;
    out->last = s->jump_last;
    return SH_STAB_OK;
}

int sh_stab_export_csv(const ShStab *s, const char *path) {
    if (!s || !path) return SH_STAB_E_ARG;
    FILE *f = fopen(path, "w");
    if (!f) return SH_STAB_E_IO;
    fprintf(f, "frame,raw_x,raw_y,out_x,out_y,jump\n");
    for (size_t i = 0; i < s->curve_len; ++i) {
        const ShStabSample *sm = &s->curve[i];
        fprintf(f, "%zu,%.6f,%.6f,%.6f,%.6f,%.6f\n",
                i, sm->raw_x, sm->raw_y, sm->out_x, sm->out_y, sm->jump);
    }
    fclose(f);
    return SH_STAB_OK;
}

const char *sh_stab_err(int rc) {
    switch (rc) {
    case SH_STAB_OK:     return "ok";
    case SH_STAB_E_ARG:  return "argument: null pointer";
    case SH_STAB_E_K:    return "field k: smoothing coefficient out of range [0,1]";
    case SH_STAB_E_SLOPE:return "field slope_k: slope coefficient out of range (need >= 0)";
    case SH_STAB_E_PREV: return "field prev: previous frame offset missing (call sh_stab_seed first)";
    case SH_STAB_E_IO:   return "export: cannot open file";
    case SH_STAB_E_MEM:  return "curve: out of memory";
    default:             return "unknown error";
    }
}
