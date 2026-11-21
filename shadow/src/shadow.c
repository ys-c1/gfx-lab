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

/* ===================== shaped lights ===================== */

static _Thread_local char g_err[160];

const char *sh_errmsg(void) { return g_err; }

static int fail(const ShShapeCfg *cfg, int code, const char *msg) {
    snprintf(g_err, sizeof(g_err), "%s", msg);
    if (cfg) snprintf((char*)cfg->err, sizeof(cfg->err), "%s", msg);
    return code;
}

/* tier defaults: LOW = few bins/samples (fast, coarse); HIGH = more (slower, finer) */
static void tier_defaults(const ShShapeCfg *cfg, int *bins, int *samples, int *picks) {
    if (cfg->tier == SH_TIER_HIGH) { *bins = 16; *samples = 24; *picks = 4; }
    else                            { *bins = 8;  *samples = 12; *picks = 3; }
}

static const char *flag_name(int bit) {
    switch (bit) {
        case SH_SHAPE_POINT:    return "SH_SHAPE_POINT";
        case SH_SHAPE_SEGMENT:  return "SH_SHAPE_SEGMENT";
        case SH_MASK_STRATIFIED:return "SH_MASK_STRATIFIED";
        case SH_MASK_EXHAUSTIVE:return "SH_MASK_EXHAUSTIVE";
        case SH_TIER_LOW:       return "SH_TIER_LOW";
        case SH_TIER_HIGH:      return "SH_TIER_HIGH";
        default:                return "UNKNOWN_FLAG";
    }
}

/* returns 0 when valid, negative error code otherwise; fills resolved counts */
static int validate(const ShShapeCfg *cfg, int *bins, int *samples, int *picks) {
    if (!cfg) return -1;
    const ShLight *l = &cfg->base;
    if (l->radius <= 0.0f || l->intensity < 0.0f)
        return fail(cfg, -2, "invalid base light: radius must be > 0 and intensity >= 0");

    /* mutually exclusive flag pairs: name both parties on conflict */
    if ((cfg->shape & SH_SHAPE_POINT) && (cfg->shape & SH_SHAPE_SEGMENT)) {
        char msg[160];
        snprintf(msg, sizeof msg, "config conflict: %s vs %s (shape)",
                 flag_name(SH_SHAPE_POINT), flag_name(SH_SHAPE_SEGMENT));
        return fail(cfg, -3, msg);
    }
    if ((cfg->mask_mode & SH_MASK_STRATIFIED) && (cfg->mask_mode & SH_MASK_EXHAUSTIVE)) {
        char msg[160];
        snprintf(msg, sizeof msg, "config conflict: %s vs %s (mask_mode)",
                 flag_name(SH_MASK_STRATIFIED), flag_name(SH_MASK_EXHAUSTIVE));
        return fail(cfg, -3, msg);
    }
    if ((cfg->tier & SH_TIER_LOW) && (cfg->tier & SH_TIER_HIGH)) {
        char msg[160];
        snprintf(msg, sizeof msg, "config conflict: %s vs %s (tier)",
                 flag_name(SH_TIER_LOW), flag_name(SH_TIER_HIGH));
        return fail(cfg, -3, msg);
    }

    int known_shape = SH_SHAPE_POINT | SH_SHAPE_SEGMENT;
    int known_mask  = SH_MASK_STRATIFIED | SH_MASK_EXHAUSTIVE;
    int known_tier  = SH_TIER_LOW | SH_TIER_HIGH;
    if (cfg->shape == 0 || (cfg->shape & ~known_shape) != 0)
        return fail(cfg, -4, "invalid config: shape must be exactly one of SH_SHAPE_POINT/SH_SHAPE_SEGMENT");
    if (cfg->mask_mode == 0 || (cfg->mask_mode & ~known_mask) != 0)
        return fail(cfg, -4, "invalid config: mask_mode must be exactly one of SH_MASK_STRATIFIED/SH_MASK_EXHAUSTIVE");
    if (cfg->tier == 0 || (cfg->tier & ~known_tier) != 0)
        return fail(cfg, -4, "invalid config: tier must be exactly one of SH_TIER_LOW/SH_TIER_HIGH");

    if (cfg->shape == SH_SHAPE_SEGMENT && !(cfg->half_len > 0.0f))
        return fail(cfg, -4, "invalid config: SH_SHAPE_SEGMENT requires half_len > 0");
    if (cfg->bins < 0 || cfg->samples < 0)
        return fail(cfg, -4, "invalid config: bins/samples must be >= 0 (0 = tier default)");
    if (cfg->bins > 128 || cfg->samples > 128)
        return fail(cfg, -4, "invalid config: bins/samples must be <= 128");

    int db, ds, dp;
    tier_defaults(cfg, &db, &ds, &dp);
    *bins    = cfg->bins    > 0 ? cfg->bins    : db;
    *samples = cfg->samples > 0 ? cfg->samples : ds;
    *picks   = dp;
    if (*picks > *samples) *picks = *samples;
    if (*bins < 1 || *samples < 1)
        return fail(cfg, -4, "invalid config: resolved bins/samples must be >= 1");
    return 0;
}

static void emitter_point(const ShShapeCfg *cfg, int s, int S, float *ex, float *ey) {
    float t = ((float)s + 0.5f) * 2.0f / (float)S - 1.0f; /* [-1,1] along axis */
    float ax = cosf(cfg->theta), ay = sinf(cfg->theta);
    *ex = cfg->base.x + t * cfg->half_len * ax;
    *ey = cfg->base.y + t * cfg->half_len * ay;
}

/* stage 1: direction mask — one row of emitter-sample selections per direction bin */
int sh_mask_build(const ShShapeCfg *cfg, ShDirMask *out) {
    int B, S, k;
    if (!out) return -1;
    int rc = validate(cfg, &B, &S, &k);
    if (rc != 0) return rc;
    memset(out, 0, sizeof(*out));
    out->bins = B; out->samples = S;
    out->theta = cfg->theta; out->half_len = cfg->half_len;
    out->sel = (unsigned char*)calloc((size_t)B * S, 1);
    out->weight = (float*)calloc((size_t)B * S, sizeof(float));
    if (!out->sel || !out->weight) { free(out->sel); free(out->weight); return -1; }

    if (cfg->mask_mode == SH_MASK_EXHAUSTIVE) {
        for (int b = 0; b < B; ++b)
            for (int s = 0; s < S; ++s) {
                out->sel[(size_t)b * S + s] = 1;
                out->weight[(size_t)b * S + s] = 1.0f / (float)S;
            }
        return 0;
    }

    /* stratified: k evenly spaced, per-bin rotated picks over the S emitter samples */
    unsigned char *taken = (unsigned char*)calloc((size_t)S, 1);
    if (!taken) { free(out->sel); free(out->weight); return -1; }
    int stride = S / k;
    if (stride < 1) stride = 1;
    for (int b = 0; b < B; ++b) {
        memset(taken, 0, (size_t)S);
        int got = 0;
        for (int j = 0; j < k && got < S; ++j) {
            int s = (b + j * stride) % S;
            if (taken[s]) continue;
            taken[s] = 1;
            out->sel[(size_t)b * S + s] = 1;
            out->weight[(size_t)b * S + s] = 1.0f / (float)k;
            ++got;
        }
    }
    free(taken);
    return 0;
}

void sh_mask_free(ShDirMask *mask) {
    if (!mask) return;
    free(mask->sel); free(mask->weight);
    mask->sel = NULL; mask->weight = NULL;
    mask->bins = mask->samples = 0;
}

int sh_mask_write_csv(const ShDirMask *mask, const char *path) {
    if (!mask || !path || !mask->sel || !mask->weight) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) return -2;
    int B = mask->bins, S = mask->samples;
    fprintf(f, "# shadow direction mask v1\n");
    fprintf(f, "# bins=%d,samples=%d,theta=%.6f,half_len=%.6f\n",
            B, S, mask->theta, mask->half_len);
    fprintf(f, "# section=select\n");
    for (int b = 0; b < B; ++b) {
        fprintf(f, "%d", b);
        for (int s = 0; s < S; ++s) fprintf(f, ",%u", (unsigned)mask->sel[(size_t)b * S + s]);
        fputc('\n', f);
    }
    fprintf(f, "# section=weight\n");
    for (int b = 0; b < B; ++b) {
        fprintf(f, "%d", b);
        for (int s = 0; s < S; ++s) fprintf(f, ",%.6f", mask->weight[(size_t)b * S + s]);
        fputc('\n', f);
    }
    fclose(f);
    return 0;
}

static int pixel_bin(const ShShapeCfg *cfg, int B, float px, float py) {
    float phi = atan2f(py - cfg->base.y, px - cfg->base.x) - cfg->theta;
    float two_pi = 2.0f * (float)M_PI;
    while (phi < 0.0f) phi += two_pi;
    while (phi >= two_pi) phi -= two_pi;
    int b = (int)(phi / two_pi * (float)B);
    if (b < 0) b = 0;
    if (b >= B) b = B - 1;
    return b;
}

/* stages 2+3: directional visibility weighting and shadow compositing */
int sh_render_shaped(ShadowMap *m, const ShShapeCfg *cfg, const ShSeg *segs, int nseg) {
    if (!m || !cfg || (nseg > 0 && !segs)) return -1;
    ShDirMask mask;
    int rc = sh_mask_build(cfg, &mask);
    if (rc != 0) return rc;
    int B = mask.bins, S = mask.samples;

    for (int y = 0; y < m->h; ++y) {
        for (int x = 0; x < m->w; ++x) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            int b = pixel_bin(cfg, B, px, py);

            /* stage 2: weight only the emitter samples selected for this
               direction. the emission cosine (strip-normal projection of the
               ray) supplies the directionality: broadside rays keep ~full
               emission while end-on rays collapse toward 0; occlusion simply
               removes the blocked samples from the average. */
            float wvis = 0.0f;
            for (int s = 0; s < S; ++s) {
                size_t idx = (size_t)b * S + s;
                if (!mask.sel[idx]) continue;
                float ex, ey;
                emitter_point(cfg, s, S, &ex, &ey);
                float rx = px - ex, ry = py - ey;
                float rlen = sqrtf(rx * rx + ry * ry);
                float ax = cosf(cfg->theta), ay = sinf(cfg->theta);
                float cos_e = rlen > 1e-9f ? fabsf(ax * ry - ay * rx) / rlen : 0.0f;
                int occluded = 0;
                for (int i = 0; i < nseg; ++i)
                    if (blocked(&segs[i], ex, ey, px, py)) { occluded = 1; break; }
                if (!occluded) wvis += mask.weight[idx] * cos_e;
            }
            float vis = wvis; /* 0..1: unoccluded broadside -> ~1, end-on -> ~0 */

            /* stage 3: composite with the legacy distance falloff */
            float dx = px - cfg->base.x, dy = py - cfg->base.y;
            float d = sqrtf(dx * dx + dy * dy);
            float fall = 1.0f / (1.0f + d / cfg->base.radius);
            m->buf[(size_t)y * m->w + x] = cfg->base.intensity * fall * vis;
        }
    }
    sh_mask_free(&mask);
    return 0;
}

int sh_render_cfg(ShadowMap *m, const ShShapeCfg *cfg, const ShSeg *segs, int nseg) {
    if (!m || !cfg || (nseg > 0 && !segs)) return -1;
    if (!cfg->enable) {
        /* switch off: legacy point-light behavior, unchanged */
        return sh_render(m, &cfg->base, segs, nseg);
    }
    return sh_render_shaped(m, cfg, segs, nseg);
}
