#include "vignette.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static float curve(const VignetteCfg *c, float d) {
    float t = (d - c->inner) / (c->outer - c->inner);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    float s = t * t * (3.0f - 2.0f * t); /* smoothstep */
    return powf(1.0f - s, c->gamma);
}

int vg_validate(const VignetteCfg *cfg, const char **where) {
    static const char *names[] = {"cfg",     "strength", "inner",   "outer",
                                  "gamma",   "lut_size", "enabled"};
    if (where) *where = NULL;
    if (!cfg) { if (where) *where = names[0]; return -1; }
    if (!(cfg->strength >= 0.0f && cfg->strength <= 1.0f)) {
        if (where) *where = names[1]; return 1;
    }
    if (!(cfg->inner >= 0.0f && cfg->inner <= 1.0f)) {
        if (where) *where = names[2]; return 2;
    }
    if (!(cfg->outer > cfg->inner && cfg->outer <= 1.0f)) {
        if (where) *where = names[3]; return 3;
    }
    if (!(isfinite(cfg->gamma) && cfg->gamma > 0.0f)) {
        if (where) *where = names[4]; return 4;
    }
    if (cfg->lut_size < 2 || cfg->lut_size > 4096) {
        if (where) *where = names[5]; return 5;
    }
    if (cfg->enabled != 0 && cfg->enabled != 1) {
        if (where) *where = names[6]; return 6;
    }
    return 0;
}

int vg_lut_build(const VignetteCfg *cfg, float *lut, int n) {
    if (vg_validate(cfg, NULL) != 0) return -1;
    if (!lut || n < 2) return -2;
    for (int i = 0; i < n; ++i)
        lut[i] = curve(cfg, (float)i / (float)(n - 1));
    return 0;
}

static int weights(const VignetteCfg *cfg, int w, int h,
                   const float *lut, int n, float *out) {
    if (vg_validate(cfg, NULL) != 0) return -1;
    if (w <= 0 || h <= 0 || !out) return -2;
    if (lut && n < 2) return -2;
    float cx = (float)w * 0.5f, cy = (float)h * 0.5f;
    float maxd = sqrtf(cx * cx + cy * cy);
    float scale = (float)(n - 1);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy;
            float d = sqrtf(dx * dx + dy * dy) / maxd;
            float v;
            if (lut) {
                float pos = d * scale;
                int i = (int)pos;
                if (i >= n - 1) v = lut[n - 1];
                else { float f = pos - (float)i; v = lut[i] + (lut[i + 1] - lut[i]) * f; }
            } else {
                v = curve(cfg, d);
            }
            out[(size_t)y * (size_t)w + (size_t)x] = v;
        }
    }
    return 0;
}

int vg_weights_ref(const VignetteCfg *cfg, int w, int h, float *out) {
    return weights(cfg, w, h, NULL, 0, out);
}

int vg_weights_fast(const VignetteCfg *cfg, int w, int h,
                    const float *lut, int n, float *out) {
    if (!lut) return -2;
    return weights(cfg, w, h, lut, n, out);
}

int vg_apply(const VignetteCfg *cfg, int w, int h,
             const float *weights, const float *in, float *out) {
    if (vg_validate(cfg, NULL) != 0) return -1;
    if (w <= 0 || h <= 0 || !in || !out) return -2;
    size_t npx = (size_t)w * (size_t)h;
    if (!cfg->enabled) {
        memmove(out, in, npx * sizeof(float));
        return 0;
    }
    if (!weights) return -2;
    for (size_t i = 0; i < npx; ++i)
        out[i] = in[i] * (1.0f - cfg->strength * (1.0f - weights[i]));
    return 0;
}

int vg_stats(const VignetteCfg *cfg, int w, int h,
             const float *weights, VignetteStats *st) {
    if (vg_validate(cfg, NULL) != 0) return -1;
    if (w <= 0 || h <= 0 || !weights || !st) return -2;
    st->strength = cfg->enabled ? cfg->strength : 0.0f;
    st->edge_pixels = 0;
    st->min_weight = 1.0f;
    size_t npx = (size_t)w * (size_t)h;
    for (size_t i = 0; i < npx; ++i) {
        if (weights[i] < 1.0f - 1e-6f) ++st->edge_pixels;
        if (weights[i] < st->min_weight) st->min_weight = weights[i];
    }
    return 0;
}

int vg_export_curve(const VignetteCfg *cfg, const char *path, int rows) {
    if (vg_validate(cfg, NULL) != 0) return -1;
    if (!path || rows < 2) return -2;
    float *lut = (float *)malloc((size_t)cfg->lut_size * sizeof(float));
    if (!lut) return -3;
    if (vg_lut_build(cfg, lut, cfg->lut_size) != 0) { free(lut); return -3; }
    FILE *f = fopen(path, "w");
    if (!f) { free(lut); return -4; }
    fprintf(f, "t,exact,lut\n");
    float scale = (float)(cfg->lut_size - 1);
    for (int r = 0; r < rows; ++r) {
        float t = (float)r / (float)(rows - 1);
        float pos = t * scale;
        int i = (int)pos;
        float lv;
        if (i >= cfg->lut_size - 1) lv = lut[cfg->lut_size - 1];
        else { float fr = pos - (float)i; lv = lut[i] + (lut[i + 1] - lut[i]) * fr; }
        fprintf(f, "%.6f,%.6f,%.6f\n", t, curve(cfg, t), lv);
    }
    fclose(f);
    free(lut);
    return 0;
}
