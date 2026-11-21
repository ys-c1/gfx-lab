#include "shadow.h"
#include "penumbra.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)
#define TEST_PI 3.14159265358979323846

static int map_equal(const ShadowMap *a, const ShadowMap *b) {
    if (a->w != b->w || a->h != b->h) return 0;
    return memcmp(a->buf, b->buf,
                  (size_t)a->w * (size_t)a->h * sizeof(float)) == 0;
}

/* Independent per-pixel curve evaluation by direct bisection on the analytic
 * shape, with no LUT, no interpolation and no table lookup.  This is the
 * "逐像素计算" cross-check for the weight stage. */
static float curve_direct(int curve, float expo, float x) {
    if (x <= -1.0f) return 0.0f;
    if (x >= 1.0f) return 1.0f;
    switch (curve) {
    case SH_CURVE_LINEAR: return 0.5f * (x + 1.0f);
    case SH_CURVE_SMOOTH: {
        float u = 0.5f * (x + 1.0f);
        return u * u * (3.0f - 2.0f * u);
    }
    case SH_CURVE_DISK: {
        double xd = (double)x;
        return (float)(0.5 + (xd * sqrt(1.0 - xd * xd) + asin(xd)) / TEST_PI);
    }
    case SH_CURVE_POW: {
        float u = 0.5f * (x + 1.0f);
        return powf(u, expo);
    }
    default: return 0.0f;
    }
}

int main(void) {
    ShError err;
    ShPenCfg cfg;
    sh_pen_cfg_default(&cfg);

    /* ---- 1. types: every field has exactly one type, and the setter rejects
     *         a mismatch naming the field ---------------------------------- */
    CK(sh_pen_field_type(SH_F_ENABLE) == SH_TY_INT);
    CK(sh_pen_field_type(SH_F_CURVE) == SH_TY_INT);
    CK(sh_pen_field_type(SH_F_CURVE_LEVELS) == SH_TY_INT);
    CK(sh_pen_field_type(SH_F_SRC_RADIUS) == SH_TY_FLOAT);
    CK(sh_pen_field_type(SH_F_WIDTH_SCALE) == SH_TY_FLOAT);
    CK(sh_pen_field_type(SH_F_CURVE_EXP) == SH_TY_FLOAT);
    CK(sh_pen_field_type((ShField)99) == SH_TY_NONE);
    CK(sh_pen_field_name((ShField)99) == NULL);
    CK(strcmp(sh_pen_field_name(SH_F_CURVE_LEVELS), "curve_levels") == 0);

    int i1 = 1;
    float f1 = 2.0f;
    /* Wrong type for an int field. */
    CK(sh_pen_cfg_set(&cfg, SH_F_ENABLE, SH_TY_FLOAT, &f1, &err) == SH_PEN_ERR_TYPE);
    CK(strcmp(err.field, "enable") == 0);
    /* Wrong type for a float field. */
    CK(sh_pen_cfg_set(&cfg, SH_F_SRC_RADIUS, SH_TY_INT, &i1, &err) == SH_PEN_ERR_TYPE);
    CK(strcmp(err.field, "src_radius") == 0);
    /* SH_TY_NONE is never a match. */
    CK(sh_pen_cfg_set(&cfg, SH_F_CURVE, SH_TY_NONE, &i1, &err) == SH_PEN_ERR_TYPE);
    CK(strcmp(err.field, "curve") == 0);
    /* Unknown field. */
    CK(sh_pen_cfg_set(&cfg, (ShField)99, SH_TY_INT, &i1, &err) == SH_PEN_ERR_FIELD);
    /* NULL value names the field too. */
    CK(sh_pen_cfg_set(&cfg, SH_F_WIDTH_SCALE, SH_TY_FLOAT, NULL, &err) == SH_PEN_ERR_NULL);
    CK(strcmp(err.field, "width_scale") == 0);
    /* Out-of-range values name the field. */
    int bad_levels = 1;
    CK(sh_pen_cfg_set(&cfg, SH_F_CURVE_LEVELS, SH_TY_INT, &bad_levels, &err) == SH_PEN_ERR_RANGE);
    CK(strcmp(err.field, "curve_levels") == 0);
    int bad_curve = 7;
    CK(sh_pen_cfg_set(&cfg, SH_F_CURVE, SH_TY_INT, &bad_curve, &err) == SH_PEN_ERR_RANGE);
    CK(strcmp(err.field, "curve") == 0);
    float bad_scale = -1.0f;
    CK(sh_pen_cfg_set(&cfg, SH_F_WIDTH_SCALE, SH_TY_FLOAT, &bad_scale, &err) == SH_PEN_ERR_RANGE);
    CK(strcmp(err.field, "width_scale") == 0);
    float nanish = NAN;
    CK(sh_pen_cfg_set(&cfg, SH_F_CURVE_EXP, SH_TY_FLOAT, &nanish, &err) == SH_PEN_ERR_RANGE);
    CK(strcmp(err.field, "curve_exp") == 0);

    /* Valid set round-trips into the config. */
    int lv = 64;
    CK(sh_pen_cfg_set(&cfg, SH_F_CURVE_LEVELS, SH_TY_INT, &lv, &err) == SH_PEN_OK);
    CK(cfg.curve_levels == 64);

    /* ---- 2. curve shapes: endpoints, midpoints, hand values -------------- */
    CK(fabsf(sh_pen_curve(SH_CURVE_LINEAR, 0, -1.0f) - 0.0f) < 1e-6f);
    CK(fabsf(sh_pen_curve(SH_CURVE_LINEAR, 0, 1.0f) - 1.0f) < 1e-6f);
    CK(fabsf(sh_pen_curve(SH_CURVE_LINEAR, 0, 0.0f) - 0.5f) < 1e-6f);
    /* disk at x=0 is exactly 0.5 (half the light disk visible) */
    CK(fabsf(sh_pen_curve(SH_CURVE_DISK, 0, 0.0f) - 0.5f) < 1e-5f);
    /* smoothstep at x=0 is 0.5, and is flat at the ends (S-curve) */
    CK(fabsf(sh_pen_curve(SH_CURVE_SMOOTH, 0, 0.0f) - 0.5f) < 1e-6f);
    CK(sh_pen_curve(SH_CURVE_SMOOTH, 0, 0.5f) >
       sh_pen_curve(SH_CURVE_LINEAR, 0, 0.5f));  /* ease-out above the ramp */
    /* pow curve with exponent 2 is below the linear ramp in the lower half */
    CK(sh_pen_curve(SH_CURVE_POW, 2.0f, -0.5f) < 0.25f);
    /* NaN input is defined (0.5) rather than propagating. */
    CK(fabsf(sh_pen_curve(SH_CURVE_DISK, 0, NAN) - 0.5f) < 1e-6f);

    /* Monotone: sampling must never decrease as x rises, for every preset. */
    for (int c = 0; c < SH_CURVE_COUNT; ++c) {
        float prev = -1.0f;
        int mono = 1;
        for (int k = 0; k <= 200; ++k) {
            float x = -1.0f + 2.0f * (float)k / 200.0f;
            float v = sh_pen_curve(c, 2.0f, x);
            if (v < prev - 1e-6f) mono = 0;
            prev = v;
        }
        CK(mono);
    }

    /* Inverse: curve(cross(level)) == level. */
    for (int c = 0; c < SH_CURVE_COUNT; ++c) {
        for (int l = 1; l < 10; ++l) {
            float level = (float)l / 10.0f;
            float x = sh_pen_curve_cross(c, 2.0f, level);
            CK(fabsf(sh_pen_curve(c, 2.0f, x) - level) < 1e-4f);
        }
    }

    /* ---- 3. curve table: length, endpoints, monotone, exportable --------- */
    float tab[16];
    CK(sh_pen_curve_table(SH_CURVE_LINEAR, 0, 16, tab, 16) == 16);
    CK(fabsf(tab[0] - 0.0f) < 1e-6f && fabsf(tab[15] - 1.0f) < 1e-6f);
    CK(sh_pen_curve_table(SH_CURVE_DISK, 0, 4, tab, 3) == SH_PEN_ERR_RANGE); /* cap too small */
    CK(sh_pen_curve_table(SH_CURVE_DISK, 0, 1, tab, 16) == SH_PEN_ERR_RANGE); /* levels too small */
    CK(sh_pen_curve_export("build/curve_lut.tsv", SH_CURVE_DISK, 0, 16) == SH_PEN_OK);

    /* ---- 4. disabled path is byte-identical to sh_render ----------------- */
    ShadowMap legacy, pen, ref;
    CK(sh_init(&legacy, 64, 64) == 0);
    CK(sh_init(&pen, 64, 64) == 0);
    CK(sh_init(&ref, 64, 64) == 0);
    ShLight light = {32.0f, 6.0f, 40.0f, 1.0f};
    ShSeg segs[2] = {{12.0f, 30.0f, 32.0f, 30.0f}, {40.0f, 40.0f, 56.0f, 40.0f}};

    CK(sh_render(&legacy, &light, segs, 2) == 0);
    /* cfg == NULL: bypass. */
    CK(sh_render_pen(&pen, &light, segs, 2, NULL, NULL, &err) == 0);
    CK(map_equal(&legacy, &pen));
    /* cfg->enable == 0: bypass even with non-default curve fields set. */
    ShPenCfg off = cfg;
    off.enable = 0;
    off.curve = SH_CURVE_POW;
    off.curve_exp = 3.5f;
    off.src_radius = 99.0f;
    CK(sh_render_pen(&pen, &light, segs, 2, &off, NULL, &err) == 0);
    CK(map_equal(&legacy, &pen));
    /* Byte-level check on the PGM output as well. */
    CK(sh_write_pgm(&legacy, "build/legacy.pgm") == 0);
    CK(sh_write_pgm(&pen, "build/pen_off.pgm") == 0);
    {
        FILE *a = fopen("build/legacy.pgm", "rb");
        FILE *b = fopen("build/pen_off.pgm", "rb");
        CK(a && b);
        int same = 1;
        int ca, cb;
        while ((ca = fgetc(a)) != EOF) { cb = fgetc(b); if (ca != cb) { same = 0; break; } }
        CK(fgetc(b) == EOF);
        CK(same);
        fclose(a); fclose(b);
    }

    /* ---- 5. enabled: stage 1 weights + stage 2 blend reproduce the map,
     *         and the pipeline equals stage1-then-stage2 ------------------- */
    ShPenCfg on = cfg;
    on.enable = 1;
    on.src_radius = 8.0f;
    on.width_scale = 1.0f;
    on.curve = SH_CURVE_DISK;
    on.curve_levels = 256;

    ShPenStats st;
    float *weight = (float *)malloc(64 * 64 * sizeof(float));
    CK(weight != NULL);
    CK(sh_pen_weights(&pen, &light, segs, 2, &on, weight, &st, &err) == SH_PEN_OK);
    CK(st.pixels == 64 * 64);
    CK(st.penumbra_pixels > 0);                 /* there is a soft band */
    CK(st.lit_pixels + st.umbra_pixels + st.penumbra_pixels == st.pixels);
    CK(st.curve == SH_CURVE_DISK && st.curve_levels == 256);
    CK(st.curve_levels_used > 1 && st.curve_levels_used <= 256);
    CK(st.bytes == (size_t)64 * 64 * sizeof(float) + 256 * sizeof(float));

    CK(sh_pen_blend(&pen, &light, weight, &st) == SH_PEN_OK);
    ShadowMap both;
    CK(sh_init(&both, 64, 64) == 0);
    CK(sh_render_pen(&both, &light, segs, 2, &on, NULL, &err) == SH_PEN_OK);
    CK(map_equal(&pen, &both));                 /* two-stage == one-call */

    /* Every penumbra pixel is strictly between the umbra and the lit value,
     * i.e. the blend actually mixes. */
    {
        int mixed_ok = 1;
        for (int i = 0; i < 64 * 64; ++i) {
            if (weight[i] > 0.0f && weight[i] < 1.0f) {
                int x = i % 64, y = i / 64;
                float lit_v = 1.0f / (1.0f + hypotf(x + 0.5f - 32.0f, y + 0.5f - 6.0f) / 40.0f);
                if (!(both.buf[i] > 0.0f && both.buf[i] < lit_v)) mixed_ok = 0;
            }
        }
        CK(mixed_ok);
    }

    /* ---- 6. LUT vs direct per-pixel curve: the exported table reproduces
     *         the weight a pixel gets ------------------------------------- */
    {
        float max_err = 0.0f;
        for (int py = 0; py < 64; py += 3) {
            for (int px = 0; px < 64; px += 3) {
                float x = 0.0f, w = 0.0f;
                CK(sh_pen_edge_coord(&light, segs, 2, &on, px, py, &x) == SH_PEN_OK);
                CK(sh_pen_weight_pixel(&light, segs, 2, &on, px, py, &w, NULL) == SH_PEN_OK);
                /* Recompute the weight from the exported table, not from the
                 * internal LUT code path. */
                int levels = on.curve_levels;
                float *t = (float *)malloc((size_t)levels * sizeof(float));
                sh_pen_curve_table(on.curve, on.curve_exp, levels, t, levels);
                float pos = (x + 1.0f) * 0.5f * (float)(levels - 1);
                int i = (int)floorf(pos);
                float wtab;
                if (i < 0) wtab = t[0];
                else if (i >= levels - 1) wtab = t[levels - 1];
                else wtab = t[i] + (t[i + 1] - t[i]) * (pos - (float)i);
                free(t);
                float e = fabsf(w - wtab);
                if (e > max_err) max_err = e;
                /* And the direct analytic curve, independent of the LUT. */
                CK(fabsf(w - curve_direct(on.curve, on.curve_exp, x)) < 0.02f);
            }
        }
        CK(max_err < 1e-6f);
    }

    /* ---- 7. half width follows similar triangles, and grows with distance - */
    {
        ShSeg s = {20.0f, 30.0f, 44.0f, 30.0f};
        /* Light at (32, 6); blocker 24 px away.  At the receiver (32, 54) the
         * ratio is (54-6-24)/24 = 1, so h = src_radius * 1. */
        float h = sh_pen_halfwidth(&light, &s, 1, 8.0f, 1.0f, 32.0f, 54.0f);
        CK(fabsf(h - 8.0f) < 0.05f);
        float h2 = sh_pen_halfwidth(&light, &s, 1, 8.0f, 1.0f, 32.0f, 90.0f);
        CK(h2 > h);                       /* farther behind the blocker: wider */
        CK(sh_pen_halfwidth(&light, &s, 1, 0.0f, 1.0f, 32.0f, 60.0f) == 0.0f);
    }

    /* ---- 8. width measurement: measured vs analytic 10-90 width --------- */
    {
        /* A blocker centred on a portrait map, its right endpoint on the
         * centre column: the visible silhouette is a vertical edge running
         * down the middle, so a horizontal scan crosses the soft band. */
        ShadowMap mw;
        CK(sh_init(&mw, 160, 160) == 0);
        ShLight l = {80.0f, 2.0f, 200.0f, 1.0f};
        ShSeg s = {80.0f, 40.0f, 10.0f, 40.0f};
        ShPenCfg c = on;
        c.src_radius = 10.0f;
        ShPenStats s2;
        CK(sh_render_pen(&mw, &l, &s, 1, &c, &s2, &err) == SH_PEN_OK);
        CK(s2.penumbra_pixels > 0);
        CK(s2.width_px > 0.0);
        CK(s2.width_expected_px > 0.0);
        /* Measured and analytic agree to within pixel quantization. */
        CK(s2.width_err_px < 1.5);
        printf("  width: measured %.3f px, analytic %.3f px, err %.3f px\n",
               s2.width_px, s2.width_expected_px, s2.width_err_px);

        /* The disk curve is narrower at 10-90 than the linear ramp, which is
         * the whole point of not using a fixed linear transition. */
        double w_disk = sh_pen_expected_width(SH_CURVE_DISK, 0, 20.0f);
        double w_lin = sh_pen_expected_width(SH_CURVE_LINEAR, 0, 20.0f);
        double w_smooth = sh_pen_expected_width(SH_CURVE_SMOOTH, 0, 20.0f);
        printf("  10-90 width at h=20: disk %.3f, smooth %.3f, linear %.3f\n",
               w_disk, w_smooth, w_lin);
        CK(fabs(w_lin - 32.0) < 0.01);                  /* linear: 0.8 * (2h) */
        CK(w_smooth < w_lin);                           /* S-curve is steeper */
        CK(w_disk < w_lin);                             /* disk is steeper still */
        sh_free(&mw);
    }

    /* ---- 9. per-pixel reference: the disk curve tracks the sampled disk ---- */
    {
        ShadowMap fast, truth;
        CK(sh_init(&fast, 96, 96) == 0);
        CK(sh_init(&truth, 96, 96) == 0);
        ShLight l = {48.0f, 2.0f, 100.0f, 1.0f};
        ShSeg s = {0.0f, 36.0f, 96.0f, 36.0f};
        ShPenCfg c = on;
        c.src_radius = 9.0f;
        c.curve = SH_CURVE_DISK;
        CK(sh_render_pen(&fast, &l, &s, 1, &c, NULL, &err) == SH_PEN_OK);
        CK(sh_render_pen_ref(&truth, &l, &s, 1, 9.0f, 256, &err) == SH_PEN_OK);
        double max_abs = 0.0, rms = 0.0;
        CK(sh_pen_compare(&fast, &truth, &max_abs, &rms) == SH_PEN_OK);
        printf("  fast path vs %d-sample disk reference: max %.4f, rms %.4f\n",
               256, max_abs, rms);
        /* The analytic disk curve should be close to the Monte Carlo truth. */
        CK(rms < 0.06);
        sh_free(&fast);
        sh_free(&truth);
    }

    /* ---- 10. error paths name the field, bad light keeps legacy codes ---- */
    {
        ShadowMap m32;
        CK(sh_init(&m32, 32, 32) == 0);
        ShPenCfg bad = on;
        bad.src_radius = 0.0f;
        CK(sh_pen_cfg_validate(&bad, &err) == SH_PEN_ERR_RANGE);
        CK(strcmp(err.field, "src_radius") == 0);
        CK(sh_render_pen(&m32, &light, segs, 2, &bad, NULL, &err) == SH_PEN_ERR_RANGE);
        CK(strcmp(err.field, "src_radius") == 0);

        ShPenCfg badl = on;
        badl.curve_levels = SH_PEN_MAX_LEVELS + 1;
        CK(sh_pen_cfg_validate(&badl, &err) == SH_PEN_ERR_RANGE);
        CK(strcmp(err.field, "curve_levels") == 0);

        ShLight bad_light = {10.0f, 10.0f, 0.0f, 1.0f};
        CK(sh_render_pen(&m32, &bad_light, segs, 2, &on, NULL, &err) == SH_PEN_ERR_LIGHT);
        /* Disabled path still defers to the legacy renderer's -2. */
        CK(sh_render_pen(&m32, &bad_light, segs, 2, &off, NULL, &err) == -2);

        CK(sh_render_pen(NULL, &light, segs, 2, &on, NULL, &err) == SH_PEN_ERR_NULL);
        CK(sh_render_pen(&m32, &light, NULL, 1, &on, NULL, &err) == SH_PEN_ERR_NULL);
        CK(sh_pen_weights(&m32, &light, segs, 2, &on, NULL, NULL, &err) == SH_PEN_ERR_NULL);
        CK(sh_render_pen_ref(&m32, &light, segs, 2, 4.0f, 0, &err) == SH_PEN_ERR_RANGE);
        CK(sh_render_pen_ref(&m32, &light, segs, 2, -1.0f, 8, &err) == SH_PEN_ERR_RANGE);
        sh_free(&m32);
    }

    /* ---- 11. pow curve goes through the pipeline and honours curve_exp --- */
    {
        ShadowMap ma, mb;
        CK(sh_init(&ma, 64, 64) == 0);
        CK(sh_init(&mb, 64, 64) == 0);
        ShPenCfg cp = on;
        cp.curve = SH_CURVE_POW;
        cp.curve_exp = 2.0f;
        ShPenCfg cp4 = cp;
        cp4.curve_exp = 4.0f;
        CK(sh_render_pen(&ma, &light, segs, 2, &cp, NULL, &err) == SH_PEN_OK);
        CK(sh_render_pen(&mb, &light, segs, 2, &cp4, NULL, &err) == SH_PEN_OK);
        /* A larger exponent darkens the same penumbra pixel. */
        int darker = 1;
        for (int i = 0; i < 64 * 64; ++i)
            if (mb.buf[i] > ma.buf[i] + 1e-7f) darker = 0;
        CK(darker);
        ShPenStats sp;
        float *wt = (float *)malloc(64 * 64 * sizeof(float));
        CK(sh_pen_weights(&ma, &light, segs, 2, &cp, wt, &sp, &err) == SH_PEN_OK);
        CK(sp.curve_levels_used > 0 && sp.curve_levels_used <= 256);
        free(wt);
        sh_free(&ma);
        sh_free(&mb);
    }

    /* ---- 12. no blockers: everything is lit, no penumbra ----------------- */
    {
        ShadowMap mn;
        CK(sh_init(&mn, 32, 32) == 0);
        ShPenStats stn;
        CK(sh_render_pen(&mn, &light, NULL, 0, &on, &stn, &err) == SH_PEN_OK);
        CK(stn.penumbra_pixels == 0);
        CK(stn.umbra_pixels == 0);
        CK(stn.lit_pixels == 32 * 32);
        sh_free(&mn);
    }

    free(weight);
    sh_free(&legacy);
    sh_free(&pen);
    sh_free(&ref);
    sh_free(&both);
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
