#include "penumbra.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SH_PI 3.14159265358979323846f
#define SH_GOLDEN_ANGLE 2.399963229728653f
#define SH_PEN_H_EPS 1e-6f

/* ------------------------------------------------------------------------- */
/* Field introspection and typed configuration                               */
/* ------------------------------------------------------------------------- */

static const char *const SH_FIELD_NAMES[SH_F_COUNT] = {
    "enable", "src_radius", "width_scale", "curve", "curve_exp", "curve_levels"
};

const char *sh_pen_field_name(ShField f) {
    if (f < 0 || f >= SH_F_COUNT) return NULL;
    return SH_FIELD_NAMES[f];
}

ShType sh_pen_field_type(ShField f) {
    switch (f) {
    case SH_F_ENABLE:       return SH_TY_INT;
    case SH_F_CURVE:        return SH_TY_INT;
    case SH_F_CURVE_LEVELS: return SH_TY_INT;
    case SH_F_SRC_RADIUS:   return SH_TY_FLOAT;
    case SH_F_WIDTH_SCALE:  return SH_TY_FLOAT;
    case SH_F_CURVE_EXP:    return SH_TY_FLOAT;
    default:                return SH_TY_NONE;
    }
}

static void pen_error_clear(ShError *err) {
    if (!err) return;
    err->code = 0;
    err->field = NULL;
    err->msg[0] = '\0';
}

static int pen_error(ShError *err, int code, ShField f, const char *what) {
    if (err) {
        err->code = code;
        err->field = (f >= 0 && f < SH_F_COUNT) ? SH_FIELD_NAMES[f] : NULL;
        snprintf(err->msg, sizeof(err->msg), "%s", what ? what : "error");
    }
    return code;
}

void sh_pen_cfg_default(ShPenCfg *cfg) {
    if (!cfg) return;
    cfg->enable = 0;
    cfg->src_radius = 6.0f;
    cfg->width_scale = 1.0f;
    cfg->curve = SH_CURVE_DISK;
    cfg->curve_exp = 2.0f;
    cfg->curve_levels = 256;
}

int sh_pen_cfg_validate(const ShPenCfg *cfg, ShError *err) {
    pen_error_clear(err);
    if (!cfg) return pen_error(err, SH_PEN_ERR_NULL, -1, "cfg is NULL");
    if (cfg->curve < 0 || cfg->curve >= SH_CURVE_COUNT)
        return pen_error(err, SH_PEN_ERR_RANGE, SH_F_CURVE, "curve is not a known preset");
    if (cfg->curve_levels < 2 || cfg->curve_levels > SH_PEN_MAX_LEVELS)
        return pen_error(err, SH_PEN_ERR_RANGE, SH_F_CURVE_LEVELS,
                         "curve_levels outside [2, SH_PEN_MAX_LEVELS]");
    if (!isfinite(cfg->curve_exp) || cfg->curve_exp <= 0.0f)
        return pen_error(err, SH_PEN_ERR_RANGE, SH_F_CURVE_EXP,
                         "curve_exp must be finite and > 0");
    if (!isfinite(cfg->width_scale) || cfg->width_scale <= 0.0f)
        return pen_error(err, SH_PEN_ERR_RANGE, SH_F_WIDTH_SCALE,
                         "width_scale must be finite and > 0");
    if (!isfinite(cfg->src_radius) || cfg->src_radius < 0.0f)
        return pen_error(err, SH_PEN_ERR_RANGE, SH_F_SRC_RADIUS,
                         "src_radius must be finite and >= 0");
    if (!cfg->enable) return SH_PEN_OK;
    if (cfg->src_radius <= 0.0f)
        return pen_error(err, SH_PEN_ERR_RANGE, SH_F_SRC_RADIUS,
                         "src_radius must be > 0 when penumbra is enabled");
    return SH_PEN_OK;
}

int sh_pen_cfg_set(ShPenCfg *cfg, ShField f, ShType ty, const void *value,
                   ShError *err) {
    pen_error_clear(err);
    if (!cfg) return pen_error(err, SH_PEN_ERR_NULL, -1, "cfg is NULL");
    if (!value) return pen_error(err, SH_PEN_ERR_NULL, f, "value is NULL");
    if (f < 0 || f >= SH_F_COUNT)
        return pen_error(err, SH_PEN_ERR_FIELD, -1, "unknown field id");
    ShType want = sh_pen_field_type(f);
    if (ty != want)
        return pen_error(err, SH_PEN_ERR_TYPE, f,
                         ty == SH_TY_NONE ? "declared type is SH_TY_NONE"
                                          : "declared type does not match the field type");

    if (want == SH_TY_INT) {
        int v = *(const int *)value;
        switch (f) {
        case SH_F_ENABLE:
            if (v != 0 && v != 1)
                return pen_error(err, SH_PEN_ERR_RANGE, f, "enable must be 0 or 1");
            cfg->enable = v;
            return SH_PEN_OK;
        case SH_F_CURVE:
            if (v < 0 || v >= SH_CURVE_COUNT)
                return pen_error(err, SH_PEN_ERR_RANGE, f, "curve is not a known preset");
            cfg->curve = v;
            return SH_PEN_OK;
        case SH_F_CURVE_LEVELS:
            if (v < 2 || v > SH_PEN_MAX_LEVELS)
                return pen_error(err, SH_PEN_ERR_RANGE, f,
                                 "curve_levels outside [2, SH_PEN_MAX_LEVELS]");
            cfg->curve_levels = v;
            return SH_PEN_OK;
        default:
            return pen_error(err, SH_PEN_ERR_FIELD, f, "field is not an int field");
        }
    }

    float v = *(const float *)value;
    switch (f) {
    case SH_F_SRC_RADIUS:
        if (!isfinite(v) || v < 0.0f)
            return pen_error(err, SH_PEN_ERR_RANGE, f, "src_radius must be finite and >= 0");
        cfg->src_radius = v;
        return SH_PEN_OK;
    case SH_F_WIDTH_SCALE:
        if (!isfinite(v) || v <= 0.0f)
            return pen_error(err, SH_PEN_ERR_RANGE, f, "width_scale must be finite and > 0");
        cfg->width_scale = v;
        return SH_PEN_OK;
    case SH_F_CURVE_EXP:
        if (!isfinite(v) || v <= 0.0f)
            return pen_error(err, SH_PEN_ERR_RANGE, f, "curve_exp must be finite and > 0");
        cfg->curve_exp = v;
        return SH_PEN_OK;
    default:
        return pen_error(err, SH_PEN_ERR_FIELD, f, "field is not a float field");
    }
}

/* ------------------------------------------------------------------------- */
/* Curve shapes and the LUT                                                  */
/* ------------------------------------------------------------------------- */

static float clamp01(float v) {
    if (!(v > 0.0f)) return 0.0f; /* also catches NaN */
    if (v > 1.0f) return 1.0f;
    return v;
}

/* Unit-disk visible fraction for an edge at normalized coordinate x:
 * 0.5 + (x*sqrt(1-x^2) + asin(x)) / pi. */
static float disk_weight(float x) {
    if (x <= -1.0f) return 0.0f;
    if (x >= 1.0f) return 1.0f;
    return 0.5f + (x * sqrtf(fmaxf(0.0f, 1.0f - x * x)) + asinf(x)) / SH_PI;
}

float sh_pen_curve(int curve, float curve_exp, float x) {
    if (!isfinite(x)) return 0.5f;
    if (x < -1.0f) x = -1.0f;
    if (x > 1.0f) x = 1.0f;
    float u = 0.5f * (x + 1.0f); /* [0, 1] ramp coordinate */
    switch (curve) {
    case SH_CURVE_LINEAR:
        return clamp01(u);
    case SH_CURVE_SMOOTH:
        return clamp01(u * u * (3.0f - 2.0f * u));
    case SH_CURVE_DISK:
        return clamp01(disk_weight(x));
    case SH_CURVE_POW: {
        float e = (isfinite(curve_exp) && curve_exp > 0.0f) ? curve_exp : 1.0f;
        return clamp01(powf(u, e));
    }
    default:
        return clamp01(u);
    }
}

float sh_pen_curve_cross(int curve, float curve_exp, float level) {
    if (!isfinite(level)) return 0.0f;
    if (level <= 0.0f) return -1.0f;
    if (level >= 1.0f) return 1.0f;
    /* Every preset is strictly increasing on (-1, 1); bisect to float ulp. */
    float lo = -1.0f, hi = 1.0f;
    for (int it = 0; it < 64; ++it) {
        float mid = 0.5f * (lo + hi);
        if (sh_pen_curve(curve, curve_exp, mid) < level) lo = mid;
        else hi = mid;
    }
    return 0.5f * (lo + hi);
}

double sh_pen_expected_width(int curve, float curve_exp, float halfwidth_px) {
    if (!(halfwidth_px > 0.0f)) return 0.0;
    double x90 = (double)sh_pen_curve_cross(curve, curve_exp, 0.9f);
    double x10 = (double)sh_pen_curve_cross(curve, curve_exp, 0.1f);
    return (double)halfwidth_px * (x90 - x10);
}

int sh_pen_curve_table(int curve, float curve_exp, int levels, float *out, int cap) {
    if (!out) return SH_PEN_ERR_NULL;
    if (curve < 0 || curve >= SH_CURVE_COUNT) return SH_PEN_ERR_RANGE;
    if (levels < 2 || levels > SH_PEN_MAX_LEVELS) return SH_PEN_ERR_RANGE;
    if (cap < levels) return SH_PEN_ERR_RANGE;
    for (int i = 0; i < levels; ++i) {
        float x = -1.0f + 2.0f * (float)i / (float)(levels - 1);
        out[i] = sh_pen_curve(curve, curve_exp, x);
    }
    return levels;
}

int sh_pen_curve_export(const char *path, int curve, float curve_exp, int levels) {
    if (!path) return SH_PEN_ERR_NULL;
    if (levels < 2 || levels > SH_PEN_MAX_LEVELS) return SH_PEN_ERR_RANGE;
    if (curve < 0 || curve >= SH_CURVE_COUNT) return SH_PEN_ERR_RANGE;
    float *tab = (float *)malloc((size_t)levels * sizeof(float));
    if (!tab) return SH_PEN_ERR_MEM;
    sh_pen_curve_table(curve, curve_exp, levels, tab, levels);
    FILE *f = fopen(path, "w");
    if (!f) { free(tab); return SH_PEN_ERR_IO; }
    fprintf(f, "# curve-weighted penumbra LUT\n");
    fprintf(f, "# curve=%d curve_exp=%.8g curve_levels=%d\n", curve, (double)curve_exp, levels);
    fprintf(f, "index\tx\tweight\n");
    for (int i = 0; i < levels; ++i) {
        float x = -1.0f + 2.0f * (float)i / (float)(levels - 1);
        fprintf(f, "%d\t%.9g\t%.9g\n", i, (double)x, (double)tab[i]);
    }
    if (fclose(f) != 0) { free(tab); return SH_PEN_ERR_IO; }
    free(tab);
    return SH_PEN_OK;
}

/* LUT set up once per render. */
typedef struct {
    float *lut;
    int levels;
} CurveLut;

static int curve_lut_build(CurveLut *t, const ShPenCfg *cfg) {
    t->levels = cfg->curve_levels;
    t->lut = (float *)malloc((size_t)t->levels * sizeof(float));
    if (!t->lut) return SH_PEN_ERR_MEM;
    if (sh_pen_curve_table(cfg->curve, cfg->curve_exp, t->levels, t->lut,
                           t->levels) != t->levels) {
        free(t->lut);
        t->lut = NULL;
        return SH_PEN_ERR_RANGE;
    }
    return SH_PEN_OK;
}

static void curve_lut_free(CurveLut *t) {
    free(t->lut);
    t->lut = NULL;
    t->levels = 0;
}

/* Evaluate the table with linear interpolation; also reports which entry the
 * coordinate landed on so the caller can count distinct levels in use. */
static float curve_lut_eval(const CurveLut *t, float x, int *index_out) {
    if (!isfinite(x)) x = 0.0f;
    if (x <= -1.0f) { if (index_out) *index_out = 0; return t->lut[0]; }
    if (x >= 1.0f) { if (index_out) *index_out = t->levels - 1; return t->lut[t->levels - 1]; }
    float pos = (x + 1.0f) * 0.5f * (float)(t->levels - 1);
    int i = (int)floorf(pos);
    if (i < 0) i = 0;
    if (i >= t->levels - 1) { if (index_out) *index_out = t->levels - 1; return t->lut[t->levels - 1]; }
    if (index_out) *index_out = i;
    float frac = pos - (float)i;
    return t->lut[i] + (t->lut[i + 1] - t->lut[i]) * frac;
}

/* ------------------------------------------------------------------------- */
/* Geometry: hard visibility, boundary distance, penumbra half width         */
/* ------------------------------------------------------------------------- */

static int blocked(const ShSeg *s, float lx, float ly, float px, float py) {
    float rx = px - lx, ry = py - ly;
    float sx = s->x1 - s->x0, sy = s->y1 - s->y0;
    float den = rx * sy - ry * sx;
    if (fabsf(den) < 1e-9f) return 0;
    float t = ((s->x0 - lx) * sy - (s->y0 - ly) * sx) / den;
    float u = ((s->x0 - lx) * ry - (s->y0 - ly) * rx) / den;
    return t > 0.0f && t < 1.0f && u >= 0.0f && u <= 1.0f;
}

static int hard_visibility(const ShLight *light, const ShSeg *segs, int nseg,
                           float px, float py) {
    for (int i = 0; i < nseg; ++i)
        if (blocked(&segs[i], light->x, light->y, px, py)) return 0;
    return 1;
}

static float falloff(const ShLight *light, float px, float py) {
    float dx = px - light->x, dy = py - light->y;
    float d = sqrtf(dx * dx + dy * dy);
    return light->intensity / (1.0f + d / light->radius);
}

static float point_seg_dist(float px, float py, const ShSeg *s) {
    float vx = s->x1 - s->x0, vy = s->y1 - s->y0;
    float wx = px - s->x0, wy = py - s->y0;
    float len2 = vx * vx + vy * vy;
    float t = len2 > 0.0f ? (wx * vx + wy * vy) / len2 : 0.0f;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    float cx = s->x0 + t * vx, cy = s->y0 + t * vy;
    float dx = px - cx, dy = py - cy;
    return sqrtf(dx * dx + dy * dy);
}

/* Distance to the hard shadow boundary.
 *
 * Silhouette boundaries are the rays from the light through each segment
 * endpoint.  The hard shadow of one segment is the region beyond the segment
 * and between its two rays.  The distance to the nearer ray is the distance to
 * the nearest silhouette edge; the sign comes from the exact visibility test,
 * so it is correct for overlapping shadows too.
 *
 * Returns the unsigned boundary distance; *inside = 1 when the pixel is
 * shadowed (so the caller negates the distance).  1e30 when no silhouette
 * exists (nseg == 0). */
static float boundary_distance(const ShLight *light, const ShSeg *segs, int nseg,
                               float px, float py, int *inside) {
    *inside = !hard_visibility(light, segs, nseg, px, py);
    if (nseg <= 0) return 1e30f;

    float best = 1e30f;
    float ox = px - light->x, oy = py - light->y;
    for (int i = 0; i < nseg; ++i) {
        const ShSeg *s = &segs[i];
        for (int e = 0; e < 2; ++e) {
            float ex = e ? s->x1 : s->x0;
            float ey = e ? s->y1 : s->y0;
            float rx = ex - light->x, ry = ey - light->y;
            float rl = sqrtf(rx * rx + ry * ry);
            if (rl < SH_PEN_H_EPS) continue;
            /* The silhouette ray bounds the shadow only beyond its endpoint;
             * a point between the light and the blocker is not near an edge. */
            float depth = (ox * rx + oy * ry) / rl;
            if (depth < rl) continue;
            float d = fabsf((ox * ry - oy * rx) / rl); /* perpendicular */
            if (d < best) best = d;
        }
    }
    return best;
}

float sh_pen_halfwidth(const ShLight *light, const ShSeg *segs, int nseg,
                       float src_radius, float width_scale, float px, float py) {
    if (!light || !segs || nseg <= 0) return 0.0f;
    if (!isfinite(src_radius) || src_radius <= 0.0f) return 0.0f;
    if (!isfinite(width_scale) || width_scale <= 0.0f) width_scale = 1.0f;

    float dx = px - light->x, dy = py - light->y;
    float dr = sqrtf(dx * dx + dy * dy);
    float dbest = 1e30f;
    for (int i = 0; i < nseg; ++i) {
        float d = point_seg_dist(light->x, light->y, &segs[i]);
        if (d < dbest) dbest = d;
    }
    if (dbest <= SH_PEN_H_EPS) return 0.0f;
    /* Similar triangles: the penumbra band widens with the distance behind
     * the blocker, scaled by the emitter radius. */
    float w = src_radius * width_scale * (dr - dbest) / dbest;
    return (w > 0.0f) ? w : 0.0f;
}

/* The signed, clamped edge coordinate plus the shadow side.  Shared by the
 * field renderer and the single-pixel entry point so they cannot diverge. */
static void edge_coord(const ShLight *light, const ShSeg *segs, int nseg,
                       const ShPenCfg *cfg, float px, float py,
                       float *x_out, int *inside_out) {
    int inside = 0;
    float d = boundary_distance(light, segs, nseg, px, py, &inside);
    float h = sh_pen_halfwidth(light, segs, nseg, cfg->src_radius,
                               cfg->width_scale, px, py);
    float x;
    if (h <= SH_PEN_H_EPS) x = inside ? -1.0f : 1.0f;
    else {
        x = (inside ? -d : d) / h;
        if (x < -1.0f) x = -1.0f;
        if (x > 1.0f) x = 1.0f;
    }
    if (x_out) *x_out = x;
    if (inside_out) *inside_out = inside;
}

int sh_pen_edge_coord(const ShLight *light, const ShSeg *segs, int nseg,
                      const ShPenCfg *cfg, int px, int py, float *x_out) {
    if (!light || !cfg || !x_out || (nseg > 0 && !segs)) return SH_PEN_ERR_NULL;
    if (nseg < 0) return SH_PEN_ERR_RANGE;
    ShError err;
    int rc = sh_pen_cfg_validate(cfg, &err);
    if (rc != SH_PEN_OK) return rc;
    edge_coord(light, segs, nseg, cfg, (float)px + 0.5f, (float)py + 0.5f, x_out, NULL);
    return SH_PEN_OK;
}

/* ------------------------------------------------------------------------- */
/* The two stages                                                           */
/* ------------------------------------------------------------------------- */

int sh_pen_weight_pixel(const ShLight *light, const ShSeg *segs, int nseg,
                        const ShPenCfg *cfg, int px, int py, float *weight,
                        float *x_out) {
    if (!light || !cfg || !weight || (nseg > 0 && !segs)) return SH_PEN_ERR_NULL;
    if (nseg < 0) return SH_PEN_ERR_RANGE;
    ShError err;
    int rc = sh_pen_cfg_validate(cfg, &err);
    if (rc != SH_PEN_OK) return rc;

    float x;
    int inside = 0;
    edge_coord(light, segs, nseg, cfg, (float)px + 0.5f, (float)py + 0.5f, &x, &inside);
    if (x_out) *x_out = x;
    if (!cfg->enable) { *weight = inside ? 0.0f : 1.0f; return SH_PEN_OK; }

    CurveLut lut;
    rc = curve_lut_build(&lut, cfg);
    if (rc != SH_PEN_OK) return rc;
    *weight = curve_lut_eval(&lut, x, NULL);
    curve_lut_free(&lut);
    return SH_PEN_OK;
}

int sh_pen_weights(ShadowMap *m, const ShLight *light, const ShSeg *segs, int nseg,
                   const ShPenCfg *cfg, float *weight, ShPenStats *stats,
                   ShError *err) {
    pen_error_clear(err);
    if (!m || !m->buf || !light || !weight || (nseg > 0 && !segs))
        return pen_error(err, SH_PEN_ERR_NULL, -1, "null map, light, segs or weight");
    if (m->w <= 0 || m->h <= 0)
        return pen_error(err, SH_PEN_ERR_STATE, -1, "map has no pixels");
    if (nseg < 0) return pen_error(err, SH_PEN_ERR_RANGE, -1, "nseg must be >= 0");
    int rc = sh_pen_cfg_validate(cfg, err);
    if (rc != SH_PEN_OK) return rc;

    CurveLut lut;
    int built = 0;
    if (cfg->enable) {
        rc = curve_lut_build(&lut, cfg);
        if (rc != SH_PEN_OK)
            return pen_error(err, rc, SH_F_CURVE_LEVELS, "curve LUT allocation failed");
        built = 1;
    }

    /* Per-LUT-index hit flags, for the "curve levels actually used" count. */
    unsigned char *used = NULL;
    long used_count = 0;
    if (built) {
        used = (unsigned char *)calloc((size_t)lut.levels, 1);
        if (!used) {
            curve_lut_free(&lut);
            return pen_error(err, SH_PEN_ERR_MEM, -1, "out of memory for level table");
        }
    }

    long lit = 0, umbra = 0, pen = 0;
    for (int y = 0; y < m->h; ++y) {
        for (int x = 0; x < m->w; ++x) {
            size_t idx = (size_t)y * (size_t)m->w + (size_t)x;
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float coord;
            int inside = 0;
            edge_coord(light, segs, nseg, cfg, px, py, &coord, &inside);
            float w;
            int li = 0;
            if (!built) {
                w = inside ? 0.0f : 1.0f;
            } else if (coord <= -1.0f) {
                w = lut.lut[0];
                li = 0;
            } else if (coord >= 1.0f) {
                w = lut.lut[lut.levels - 1];
                li = lut.levels - 1;
            } else {
                w = curve_lut_eval(&lut, coord, &li);
            }
            if (built && !used[li]) { used[li] = 1; ++used_count; }
            weight[idx] = w;
            if (w >= 1.0f) ++lit;
            else if (w <= 0.0f) ++umbra;
            else ++pen;
        }
    }
    if (built) {
        curve_lut_free(&lut);
        free(used);
    }

    if (stats) {
        size_t wbytes = (size_t)m->w * (size_t)m->h * sizeof(float);
        size_t lbytes = built ? (size_t)cfg->curve_levels * sizeof(float) : 0;
        memset(stats, 0, sizeof(*stats));
        stats->pixels = (long)m->w * (long)m->h;
        stats->lit_pixels = lit;
        stats->umbra_pixels = umbra;
        stats->penumbra_pixels = pen;
        stats->curve = cfg->curve;
        stats->curve_levels = cfg->curve_levels;
        stats->curve_levels_used = built ? used_count : 0;
        stats->bytes = wbytes + lbytes;
    }
    return SH_PEN_OK;
}

int sh_pen_blend(ShadowMap *m, const ShLight *light, const float *weight,
                 ShPenStats *stats) {
    if (!m || !m->buf || !light || !weight) return SH_PEN_ERR_NULL;
    if (m->w <= 0 || m->h <= 0) return SH_PEN_ERR_STATE;

    long lit = 0, umbra = 0, pen = 0;
    for (int y = 0; y < m->h; ++y) {
        for (int x = 0; x < m->w; ++x) {
            size_t idx = (size_t)y * (size_t)m->w + (size_t)x;
            float w = weight[idx];
            if (!(w > 0.0f)) w = 0.0f;
            if (w > 1.0f) w = 1.0f;
            /* Stage 2: mix light and dark by the stage-1 weight. */
            float light_v = falloff(light, (float)x + 0.5f, (float)y + 0.5f);
            float dark_v = 0.0f;
            m->buf[idx] = dark_v + (light_v - dark_v) * w;
            if (w >= 1.0f) ++lit;
            else if (w <= 0.0f) ++umbra;
            else ++pen;
        }
    }
    if (stats) {
        stats->pixels = (long)m->w * (long)m->h;
        stats->lit_pixels = lit;
        stats->umbra_pixels = umbra;
        stats->penumbra_pixels = pen;
    }
    return SH_PEN_OK;
}

int sh_render_pen(ShadowMap *m, const ShLight *light, const ShSeg *segs, int nseg,
                  const ShPenCfg *cfg, ShPenStats *stats, ShError *err) {
    pen_error_clear(err);
    if (!m || !m->buf || !light)
        return pen_error(err, SH_PEN_ERR_NULL, -1, "null map or light");
    if (nseg < 0) return pen_error(err, SH_PEN_ERR_RANGE, -1, "nseg must be >= 0");
    if (nseg > 0 && !segs) return pen_error(err, SH_PEN_ERR_NULL, -1, "segs is NULL");

    /* Disabled: forward to the untouched renderer, so the buffer (and the PGM
     * written from it) is byte-for-byte identical to the pre-feature version. */
    if (!cfg || !cfg->enable) return sh_render(m, light, segs, nseg);

    int rc = sh_pen_cfg_validate(cfg, err);
    if (rc != SH_PEN_OK) return rc;
    if (light->radius <= 0.0f || light->intensity < 0.0f)
        return pen_error(err, SH_PEN_ERR_LIGHT, -1, "light radius <= 0 or intensity < 0");

    size_t npix = (size_t)m->w * (size_t)m->h;
    float *weight = (float *)malloc(npix * sizeof(float));
    if (!weight) return pen_error(err, SH_PEN_ERR_MEM, -1, "out of memory for weights");
    rc = sh_pen_weights(m, light, segs, nseg, cfg, weight, stats, err);
    if (rc == SH_PEN_OK) rc = sh_pen_blend(m, light, weight, stats);

    /* Measure the 10-90 width on the row that has the widest transition (best
     * pixel resolution, so the smallest quantization error) and compare it
     * with the analytic width at that row's crossing midpoint. */
    if (rc == SH_PEN_OK && stats) {
        double best_w = 0.0, best_exp = 0.0;
        int found = 0;
        for (int row = 0; row < m->h; ++row) {
            double wpx = 0.0, x_hi = 0.0, x_lo = 0.0;
            if (sh_pen_measure_width(weight, m->w, m->h, row, &wpx, &x_hi, &x_lo)
                != SH_PEN_OK)
                continue;
            float mx = (float)(0.5 * (x_hi + x_lo)) + 0.5f;
            float my = (float)row + 0.5f;
            float h = sh_pen_halfwidth(light, segs, nseg, cfg->src_radius,
                                       cfg->width_scale, mx, my);
            double exp_w = sh_pen_expected_width(cfg->curve, cfg->curve_exp, h);
            if (!found || exp_w > best_exp) {
                found = 1;
                best_w = wpx;
                best_exp = exp_w;
            }
        }
        if (found) {
            stats->width_px = best_w;
            stats->width_expected_px = best_exp;
            stats->width_err_px = fabs(best_w - best_exp);
        }
    }
    free(weight);
    return rc;
}

/* ------------------------------------------------------------------------- */
/* Per-pixel reference                                                       */
/* ------------------------------------------------------------------------- */

int sh_render_pen_ref(ShadowMap *m, const ShLight *light, const ShSeg *segs,
                      int nseg, float src_radius, int samples, ShError *err) {
    pen_error_clear(err);
    if (!m || !m->buf || !light || (nseg > 0 && !segs))
        return pen_error(err, SH_PEN_ERR_NULL, -1, "null map, light or segs");
    if (m->w <= 0 || m->h <= 0)
        return pen_error(err, SH_PEN_ERR_STATE, -1, "map has no pixels");
    if (light->radius <= 0.0f || light->intensity < 0.0f)
        return pen_error(err, SH_PEN_ERR_LIGHT, -1, "light radius <= 0 or intensity < 0");
    if (!isfinite(src_radius) || src_radius < 0.0f)
        return pen_error(err, SH_PEN_ERR_RANGE, SH_F_SRC_RADIUS,
                         "src_radius must be finite and >= 0");
    if (samples <= 0)
        return pen_error(err, SH_PEN_ERR_RANGE, -1, "samples must be > 0");

    for (int y = 0; y < m->h; ++y) {
        for (int x = 0; x < m->w; ++x) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            int visible = 0;
            for (int k = 0; k < samples; ++k) {
                float rr = src_radius * sqrtf(((float)k + 0.5f) / (float)samples);
                float a = (float)k * SH_GOLDEN_ANGLE;
                float lx = light->x + rr * cosf(a);
                float ly = light->y + rr * sinf(a);
                int vis = 1;
                for (int i = 0; i < nseg; ++i)
                    if (blocked(&segs[i], lx, ly, px, py)) { vis = 0; break; }
                visible += vis;
            }
            float cov = (float)visible / (float)samples;
            m->buf[(size_t)y * (size_t)m->w + x] = falloff(light, px, py) * cov;
        }
    }
    return SH_PEN_OK;
}

/* ------------------------------------------------------------------------- */
/* Measurement and comparison                                               */
/* ------------------------------------------------------------------------- */

/* Fractional x where the segment between pixel i and i+1 crosses `level`. */
static double cross_between(const float *w, size_t base, int i, int j, float level) {
    float a = w[base + (size_t)i];
    float b = w[base + (size_t)j];
    if (b == a) return (double)i;
    double t = ((double)level - (double)a) / ((double)b - (double)a);
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    return (double)i + t * (double)(j - i);
}

/* First crossing of `level` in the direction of the transition, given the last
 * pixel at or past the start level (i_start) and the first pixel at or past
 * the end level (i_end).  Interpolation only spans the adjacent pair that
 * straddles the level, so a non-linear curve is measured correctly. */

int sh_pen_measure_width(const float *weight, int w, int h, int row,
                         double *width_px, double *x_hi, double *x_lo) {
    if (!weight) return SH_PEN_ERR_NULL;
    if (w <= 1 || h <= 0 || row < 0 || row >= h) return SH_PEN_ERR_RANGE;
    if (width_px) *width_px = 0.0;
    if (x_hi) *x_hi = -1.0;
    if (x_lo) *x_lo = -1.0;

    size_t base = (size_t)row * (size_t)w;

    /* Falling edge (lit -> shadowed): find the last pixel at >= 0.9 and the
     * first later pixel at <= 0.1, then interpolate each level on its own
     * adjacent pair. */
    int i_hi = -1, i_lo = -1;
    for (int i = 0; i < w; ++i) {
        float v = weight[base + (size_t)i];
        if (v >= 0.9f) i_hi = i;
        if (i_hi >= 0 && v <= 0.1f) { i_lo = i; break; }
    }
    if (i_hi >= 0 && i_lo > i_hi) {
        double x90 = 0.0, x10 = 0.0;
        /* 0.9 crossing: between i_hi and i_hi+1 (value falls below 0.9). */
        int j = i_hi;
        while (j < i_lo && weight[base + (size_t)(j + 1)] >= 0.9f) ++j;
        x90 = cross_between(weight, base, j, (j + 1 < w) ? j + 1 : j, 0.9f);
        /* 0.1 crossing: between i_lo-1 and i_lo. */
        int k = i_lo;
        while (k > 0 && weight[base + (size_t)(k - 1)] <= 0.1f) --k;
        x10 = cross_between(weight, base, (k - 1 >= 0) ? k - 1 : k, k, 0.1f);
        if (width_px) *width_px = fabs(x90 - x10);
        if (x_hi) *x_hi = x90;
        if (x_lo) *x_lo = x10;
        return SH_PEN_OK;
    }

    /* Rising edge (shadowed -> lit). */
    int i_lo2 = -1, i_hi2 = -1;
    for (int i = 0; i < w; ++i) {
        float v = weight[base + (size_t)i];
        if (v <= 0.1f) i_lo2 = i;
        if (i_lo2 >= 0 && v >= 0.9f) { i_hi2 = i; break; }
    }
    if (i_lo2 >= 0 && i_hi2 > i_lo2) {
        double x10 = 0.0, x90 = 0.0;
        int j = i_lo2;
        while (j < i_hi2 && weight[base + (size_t)(j + 1)] <= 0.1f) ++j;
        x10 = cross_between(weight, base, j, (j + 1 < w) ? j + 1 : j, 0.1f);
        int k = i_hi2;
        while (k > 0 && weight[base + (size_t)(k - 1)] >= 0.9f) --k;
        x90 = cross_between(weight, base, (k - 1 >= 0) ? k - 1 : k, k, 0.9f);
        if (width_px) *width_px = fabs(x90 - x10);
        if (x_hi) *x_hi = x90;
        if (x_lo) *x_lo = x10;
        return SH_PEN_OK;
    }

    return SH_PEN_ERR_STATE;
}

int sh_pen_compare(const ShadowMap *a, const ShadowMap *b,
                   double *max_abs, double *rms) {
    if (!a || !b || !a->buf || !b->buf) return SH_PEN_ERR_NULL;
    if (a->w != b->w || a->h != b->h) return SH_PEN_ERR_RANGE;
    size_t n = (size_t)a->w * (size_t)a->h;
    double mx = 0.0, ss = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double d = fabs((double)a->buf[i] - (double)b->buf[i]);
        if (d > mx) mx = d;
        ss += d * d;
    }
    if (max_abs) *max_abs = mx;
    if (rms) *rms = n ? sqrt(ss / (double)n) : 0.0;
    return SH_PEN_OK;
}
