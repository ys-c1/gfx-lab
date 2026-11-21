#include "shadow.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

/* ------------------------------------------------------------------------- */
/* 错误报告                                                                  */
/* ------------------------------------------------------------------------- */
static char g_err[256] = "";

static void set_err(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_err, sizeof(g_err), fmt, ap);
    va_end(ap);
}

const char *sh_last_error(void) { return g_err; }

/* ------------------------------------------------------------------------- */
/* 档位预设                                                                  */
/* ------------------------------------------------------------------------- */
static const ShRenderCfg g_presets[] = {
    {SH_GEAR_OFF,     1,  1, 0.0f},
    {SH_GEAR_FAST,    4,  4, 0.0f},   /* 高速档：16 点 */
    {SH_GEAR_SLOW,   12, 12, 0.0f},   /* 低速档：144 点 */
    {SH_GEAR_EXHAUST,48, 48, 0.0f},   /* 穷举参考：2304 点 */
};

const ShRenderCfg *sh_preset_cfg(int gear) {
    if (gear < SH_GEAR_OFF || gear > SH_GEAR_EXHAUST) return NULL;
    return &g_presets[gear];
}

/* ------------------------------------------------------------------------- */
/* 基础工具                                                                  */
/* ------------------------------------------------------------------------- */
int sh_init(ShadowMap *m, int w, int h) {
    if (!m || w <= 0 || h <= 0) { set_err("bad argument: sh_init w=%d h=%d", w, h); return -1; }
    m->buf = (float*)calloc((size_t)w * (size_t)h, sizeof(float));
    if (!m->buf) { set_err("out of memory: %d x %d", w, h); return -1; }
    m->w = w; m->h = h;
    return 0;
}

void sh_free(ShadowMap *m) { if (m) { free(m->buf); m->buf = NULL; m->w = 0; m->h = 0; } }

int sh_clear(ShadowMap *m) {
    if (!m) { set_err("bad argument: sh_clear(NULL)"); return -1; }
    for (int i = 0; i < m->w * m->h; ++i) m->buf[i] = 0.0f;
    return 0;
}

/* 线段 s 是否挡住从 (lx,ly) 到 (px,py) 的射线（与原实现完全一致）。 */
static int blocked(const ShSeg *s, float lx, float ly, float px, float py) {
    float rx = px - lx, ry = py - ly;
    float sx = s->x1 - s->x0, sy = s->y1 - s->y0;
    float den = rx * sy - ry * sx;
    if (fabsf(den) < 1e-9f) return 0;
    float t = ((s->x0 - lx) * sy - (s->y0 - ly) * sx) / den;
    float u = ((s->x0 - lx) * ry - (s->y0 - ly) * rx) / den;
    return t > 0.0f && t < 1.0f && u >= 0.0f && u <= 1.0f;
}

static int valid_light(const ShLight *l) {
    if (!l) return 0;
    if (l->radius <= 0.0f || l->intensity < 0.0f) return 0;
    if (l->aw < 0.0f || l->ah < 0.0f) return 0;
    if ((l->aw == 0.0f) != (l->ah == 0.0f)) return 0;   /* 两半轴必须同时为 0 或同时 >0 */
    return 1;
}

/* 面光管线的统一参数/冲突校验。返回 0 表示可按面光路径执行。
 * 返回 1 表示应走点光源旧路径（开关为假）。负数为错误。 */
static int validate_ex(const ShLight *light, const ShRenderCfg *cfg) {
    if (!light) { set_err("bad argument: light is NULL"); return -1; }
    if (!valid_light(light)) { set_err("invalid light: radius=%g intensity=%g aw=%g ah=%g",
                                       light ? light->radius : 0.0,
                                       light ? light->intensity : 0.0,
                                       light ? light->aw : 0.0,
                                       light ? light->ah : 0.0); return -2; }
    if (!cfg) { set_err("bad argument: cfg is NULL"); return -1; }
    if (cfg->gear < SH_GEAR_OFF || cfg->gear > SH_GEAR_EXHAUST) {
        set_err("bad argument: unknown gear=%d", cfg->gear); return -1;
    }
    if (cfg->cols <= 0 || cfg->rows <= 0) {
        set_err("bad argument: sample grid %dx%d must be positive", cfg->cols, cfg->rows);
        return -1;
    }
    if (cfg->jitter < 0.0f || cfg->jitter > 1.0f) {
        set_err("bad argument: jitter=%g out of [0,1]", cfg->jitter); return -1;
    }

    int area_on  = light->aw > 0.0f && light->ah > 0.0f;
    int gear_on  = cfg->gear != SH_GEAR_OFF;
    int multi_pt = cfg->cols * cfg->rows > 1;

    /* 冲突配置要报错，并指明两方。 */
    if (area_on && cfg->gear == SH_GEAR_OFF) {
        set_err("config conflict: light.aw/ah=%g/%g (area light ON) vs cfg.gear=OFF (point mode)",
                light->aw, light->ah);
        return -3;
    }
    if (!area_on && gear_on) {
        set_err("config conflict: light.aw=ah=0 (point light) vs cfg.gear=%d (area mode)",
                cfg->gear);
        return -3;
    }
    if (area_on && !multi_pt) {
        set_err("config conflict: light.aw/ah=%g/%g (area light ON) vs sample grid %dx%d (single point)",
                light->aw, light->ah, cfg->cols, cfg->rows);
        return -3;
    }
    if (!area_on && multi_pt) {
        set_err("config conflict: light.aw=ah=0 (point light) vs sample grid %dx%d (multi sample)",
                cfg->cols, cfg->rows);
        return -3;
    }
    return area_on ? 0 : 1;
}

/* ------------------------------------------------------------------------- */
/* 点光源旧路径（开关为假时逐位保持原行为）                                   */
/* ------------------------------------------------------------------------- */
static void render_point(ShadowMap *m, float *cov, const ShLight *light,
                         const ShSeg *segs, int nseg) {
    for (int y = 0; y < m->h; ++y) {
        for (int x = 0; x < m->w; ++x) {
            size_t idx = (size_t)y * m->w + x;
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
            m->buf[idx] = v;
            if (cov) cov[idx] = shadowed ? 0.0f : 1.0f;
        }
    }
}

int sh_render(ShadowMap *m, const ShLight *light, const ShSeg *segs, int nseg) {
    if (!m || !light || (nseg > 0 && !segs)) { set_err("bad argument: sh_render"); return -1; }
    if (!valid_light(light)) { set_err("invalid light: radius=%g intensity=%g",
                                       light->radius, light->intensity); return -2; }
    /* 旧接口只支持点光源；对面光调用即冲突，指明两方。 */
    if (light->aw > 0.0f || light->ah > 0.0f) {
        set_err("config conflict: light.aw/ah=%g/%g (area light) vs legacy sh_render (point-only API)",
                light->aw, light->ah);
        return -3;
    }
    render_point(m, NULL, light, segs, nseg);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* 第 1 段：按光源朝向取多点                                                  */
/* ------------------------------------------------------------------------- */
/* 确定性整数哈希，供抖动使用（无 jitter 时不参与，保证网格结果可复现）。 */
static unsigned hash_u(unsigned x) {
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16; return x;
}

int sh_local_point(const ShRenderCfg *cfg, int idx, float *lx, float *ly) {
    if (!cfg || !lx || !ly) { set_err("bad argument: sh_local_point"); return -1; }
    int n = cfg->cols * cfg->rows;
    if (idx < 0 || idx >= n) { set_err("bad argument: sample idx=%d out of [0,%d)", idx, n); return -1; }
    int r = idx / cfg->cols, c = idx % cfg->cols;
    /* 网格点落在 [-1,1]^2，再由 sh_sample_quad 分别乘半宽 aw / 半高 ah。 */
    float fx = cfg->cols == 1 ? 0.0f : ((float)c + 0.5f) * 2.0f / (float)cfg->cols - 1.0f;
    float fy = cfg->rows == 1 ? 0.0f : ((float)r + 0.5f) * 2.0f / (float)cfg->rows - 1.0f;
    if (cfg->jitter > 0.0f) {
        float sx = ((float)(hash_u((unsigned)idx * 73856093u) & 0xffff) / 32767.5f) - 1.0f;
        float sy = ((float)(hash_u((unsigned)idx * 19349663u) & 0xffff) / 32767.5f) - 1.0f;
        float stepx = cfg->cols == 1 ? 0.0f : 2.0f / (float)cfg->cols;
        float stepy = cfg->rows == 1 ? 0.0f : 2.0f / (float)cfg->rows;
        fx += sx * stepx * cfg->jitter * 0.5f;
        fy += sy * stepy * cfg->jitter * 0.5f;
    }
    *lx = fx; *ly = fy;
    return 0;
}

int sh_sample_quad(const ShLight *light, const ShRenderCfg *cfg, ShPoint *out) {
    int mode = validate_ex(light, cfg);
    if (mode < 0) return mode;       /* 错误已带信息 */
    if (!out) { set_err("bad argument: sample output is NULL"); return -1; }
    int n = cfg->cols * cfg->rows;
    float ca = cosf(light->angle), sa = sinf(light->angle);
    if (mode == 1) {                 /* 点光源：单点落在光源中心 */
        out[0].x = light->x; out[0].y = light->y;
        return 1;
    }
    for (int i = 0; i < n; ++i) {
        float lx = 0.0f, ly = 0.0f;
        sh_local_point(cfg, i, &lx, &ly);
        lx *= light->aw; ly *= light->ah;              /* 局部坐标（发光面内） */
        out[i].x = light->x + lx * ca - ly * sa;       /* 按朝向旋转到世界坐标 */
        out[i].y = light->y + lx * sa + ly * ca;
    }
    return n;
}

/* ------------------------------------------------------------------------- */
/* 第 2 段：按覆盖比例合成                                                    */
/* ------------------------------------------------------------------------- */
int sh_composite(ShadowMap *m, float *cov, const ShLight *light,
                 const ShRenderCfg *cfg, const ShSeg *segs, int nseg) {
    if (!m) { set_err("bad argument: shadow map is NULL"); return -1; }
    if (nseg > 0 && !segs) { set_err("bad argument: nseg=%d but segs=NULL", nseg); return -1; }
    int mode = validate_ex(light, cfg);
    if (mode < 0) return mode;

    if (mode == 1) {                 /* 开关为假：旧路径，逐位不变 */
        render_point(m, cov, light, segs, nseg);
        return 0;
    }

    int n = cfg->cols * cfg->rows;
    ShPoint *pts = (ShPoint*)malloc((size_t)n * sizeof(ShPoint));
    if (!pts) { set_err("out of memory: %d sample points", n); return -1; }
    int got = sh_sample_quad(light, cfg, pts);
    if (got < 0) { free(pts); return got; }

    float inv_n = 1.0f / (float)n;
    for (int y = 0; y < m->h; ++y) {
        for (int x = 0; x < m->w; ++x) {
            size_t pidx = (size_t)y * m->w + x;
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            int lit = 0;
            for (int k = 0; k < n; ++k) {
                int shadowed = 0;
                for (int i = 0; i < nseg; ++i)
                    if (blocked(&segs[i], pts[k].x, pts[k].y, px, py)) { shadowed = 1; break; }
                if (!shadowed) ++lit;
            }
            float coverage = (float)lit * inv_n;    /* 覆盖比例：未遮挡采样占比 */
            if (cov) cov[pidx] = coverage;
            float dx = px - light->x, dy = py - light->y;
            float d = sqrtf(dx * dx + dy * dy);
            float fall = 1.0f / (1.0f + d / light->radius);
            m->buf[pidx] = light->intensity * fall * coverage;
        }
    }
    free(pts);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* 第 3 段：半影宽度                                                          */
/* ------------------------------------------------------------------------- */
/* 沿一条扫描线统计半影宽度：部分覆盖（0 < cov < 1）连续段的跨度。
 * 本影（cov=0）与全亮（cov=1）之间的部分覆盖区间即半影；扫描线只经过
 * 本影+半影而未到达全亮区时同样计入。返回 -1 表示该扫描线无半影。 */
static float transition_width(const float *line, int len) {
    const float eps = 1e-6f;
    float best = -1.0f;
    int start = -1;
    for (int i = 0; i <= len; ++i) {
        int partial = i < len && line[i] > eps && line[i] < 1.0f - eps;
        if (partial && start < 0) start = i;
        if (!partial && start >= 0) {
            float w = (float)(i - start);
            if (w > best) best = w;
            start = -1;
        }
    }
    return best;
}

int sh_penumbra(const float *cov, int w, int h, ShPenumbra *out) {
    if (!cov || !out || w <= 0 || h <= 0) { set_err("bad argument: sh_penumbra"); return -1; }
    out->width_avg = 0.0f; out->width_max = 0.0f; out->transitions = 0;
    float sum = 0.0f; int count = 0;
    float *line = (float*)malloc((size_t)(w > h ? w : h) * sizeof(float));
    if (!line) { set_err("out of memory: penumbra scanline"); return -1; }

    for (int y = 0; y < h; ++y) {                 /* 水平扫描线 */
        for (int x = 0; x < w; ++x) line[x] = cov[(size_t)y * w + x];
        float tw = transition_width(line, w);
        if (tw >= 0.0f) { sum += tw; ++count; if (tw > out->width_max) out->width_max = tw; }
    }
    for (int x = 0; x < w; ++x) {                 /* 垂直扫描线 */
        for (int y = 0; y < h; ++y) line[y] = cov[(size_t)y * w + x];
        float tw = transition_width(line, h);
        if (tw >= 0.0f) { sum += tw; ++count; if (tw > out->width_max) out->width_max = tw; }
    }
    free(line);
    out->transitions = count;
    if (count) out->width_avg = sum / (float)count;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* 完整面光管线                                                              */
/* ------------------------------------------------------------------------- */
int sh_render_ex(ShadowMap *m, float *cov, ShPenumbra *out_pen,
                 const ShLight *light, const ShRenderCfg *cfg,
                 const ShSeg *segs, int nseg) {
    int rc = sh_composite(m, cov, light, cfg, segs, nseg);
    if (rc != 0) return rc;
    if (out_pen) {
        if (!cov) { set_err("bad argument: penumbra requested but coverage buffer is NULL"); return -1; }
        rc = sh_penumbra(cov, m->w, m->h, out_pen);
        if (rc != 0) return rc;
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
int sh_write_pgm(const ShadowMap *m, const char *path) {
    if (!m || !path) { set_err("bad argument: sh_write_pgm"); return -1; }
    FILE *f = fopen(path, "wb");
    if (!f) { set_err("cannot open %s for writing", path); return -2; }
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
