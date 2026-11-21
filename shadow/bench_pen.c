/* Penumbra benchmark: 256 and 1024 maps.
 *
 * For each resolution and each curve preset it reports
 *   - per-pixel error of the fast path against the sampled-disk reference
 *     (max absolute, RMS) plus the same figures for the legacy hard edge,
 *   - the measured 10-90 penumbra width and its analytic expectation,
 *   - the number of pixels in each band (lit / penumbra / umbra),
 *   - the number of curve LUT levels and how many the field actually uses,
 *   - peak pipeline bytes and elapsed time,
 * and writes the curve tables it used to build/.
 */
#define _POSIX_C_SOURCE 199309L
#include "shadow.h"
#include "penumbra.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

/* The hard renderer's distance falloff, so coverage can be recovered from a
 * shaded value. */
static float falloff_at(const ShLight *l, float px, float py) {
    float dx = px - l->x, dy = py - l->y;
    return l->intensity / (1.0f + sqrtf(dx * dx + dy * dy) / l->radius);
}

static const char *curve_name(int c) {
    switch (c) {
    case SH_CURVE_LINEAR: return "linear";
    case SH_CURVE_SMOOTH: return "smooth";
    case SH_CURVE_DISK:   return "disk";
    case SH_CURVE_POW:    return "pow";
    default:              return "?";
    }
}

/* Scene scaled to a resolution: a light above the map and two blockers at mid
 * depth, so the penumbra band is a well-conditioned 10-40 px wide rather than
 * overwhelming the whole map.  Both the geometry and the light radius scale
 * with the resolution, so the same world is rendered at either size. */
static void scene(int n, ShLight *l, ShSeg *segs) {
    float s = (float)n / 256.0f;
    l->x = 128.0f * s; l->y = -560.0f * s;
    l->radius = 900.0f * s; l->intensity = 1.0f;
    segs[0].x0 = 40.0f * s;  segs[0].y0 = 150.0f * s;
    segs[0].x1 = 120.0f * s; segs[0].y1 = 150.0f * s;
    segs[1].x0 = 150.0f * s; segs[1].y0 = 170.0f * s;
    segs[1].x1 = 230.0f * s; segs[1].y1 = 200.0f * s;
}

int main(void) {
    const int res[2] = {256, 1024};
    const int curves[SH_CURVE_COUNT] = {
        SH_CURVE_LINEAR, SH_CURVE_SMOOTH, SH_CURVE_DISK, SH_CURVE_POW
    };
    ShError err;

    printf("curve-weighted penumbra benchmark\n");
    printf("reference: per-pixel sunflower sampling of the disk light, 512 samples\n\n");

    for (int ri = 0; ri < 2; ++ri) {
        int n = res[ri];
        ShadowMap fast, ref, hard;
        if (sh_init(&fast, n, n) || sh_init(&ref, n, n) || sh_init(&hard, n, n)) {
            fprintf(stderr, "alloc failed at %d\n", n);
            return 1;
        }
        ShLight l;
        ShSeg segs[2];
        scene(n, &l, segs);
        double s = (double)n / 256.0;

        /* Reference and legacy hard edge once per resolution. */
        double t0 = now_ms();
        if (sh_render_pen_ref(&ref, &l, segs, 2, 18.0f * (float)s, 512, &err) != SH_PEN_OK) {
            fprintf(stderr, "ref failed: %s\n", err.msg);
            return 1;
        }
        double t_ref = now_ms() - t0;
        if (sh_render(&hard, &l, segs, 2) != 0) return 1;

        printf("=== %d x %d ===\n", n, n);
        printf("  pixels            : %ld\n", (long)n * n);
        printf("  reference         : 512 samples/pixel, %.1f ms\n", t_ref);

        for (int ci = 0; ci < SH_CURVE_COUNT; ++ci) {
            for (int li = 0; li < 2; ++li) {
                int levels = li == 0 ? 256 : 1024;
                ShPenCfg cfg;
                sh_pen_cfg_default(&cfg);
                cfg.enable = 1;
                cfg.src_radius = 18.0f * (float)s;
                cfg.width_scale = 1.0f;
                cfg.curve = curves[ci];
                cfg.curve_exp = 2.0f;
                cfg.curve_levels = levels;

                ShPenStats st;
                double ta = now_ms();
                int rc = sh_render_pen(&fast, &l, segs, 2, &cfg, &st, &err);
                double t = now_ms() - ta;
                if (rc != SH_PEN_OK) {
                    fprintf(stderr, "render failed: %s\n", err.msg);
                    return 1;
                }

                double mx = 0.0, rms = 0.0, hmx = 0.0, hrms = 0.0;
                sh_pen_compare(&fast, &ref, &mx, &rms);
                sh_pen_compare(&hard, &ref, &hmx, &hrms);

                /* Error restricted to pixels where it actually matters: a
                 * reference pixel that is neither fully lit nor fully dark.
                 * Measured on coverage (value / falloff), so distance does not
                 * inflate it. */
                double pmx = 0.0, prms = 0.0, phmx = 0.0, phrms = 0.0;
                long pn = 0;
                for (int y = 0; y < n; ++y) {
                    for (int x = 0; x < n; ++x) {
                        size_t i = (size_t)y * (size_t)n + (size_t)x;
                        float fo = falloff_at(&l, (float)x + 0.5f, (float)y + 0.5f);
                        if (fo <= 0.0f) continue;
                        double rc = ref.buf[i] / fo;
                        if (rc <= 0.02 || rc >= 0.98) continue;
                        double df = (fast.buf[i] - ref.buf[i]) / fo;
                        double dh = (hard.buf[i] - ref.buf[i]) / fo;
                        ++pn;
                        if (fabs(df) > pmx) pmx = fabs(df);
                        if (fabs(dh) > phmx) phmx = fabs(dh);
                        prms += df * df;
                        phrms += dh * dh;
                    }
                }
                if (pn > 0) { prms = sqrt(prms / (double)pn); phrms = sqrt(phrms / (double)pn); }

                printf("  %-7s lv=%-4d  all: max %.4f rms %.4f  (hard: %.4f/%.4f)\n",
                       curve_name(cfg.curve), levels, mx, rms, hmx, hrms);
                printf("           band(%ld px): max %.4f rms %.4f  (hard: %.4f/%.4f)\n",
                       pn, pmx, prms, phmx, phrms);
                printf("           width %.2f px / analytic %.2f px / err %.2f px\n",
                       st.width_px, st.width_expected_px, st.width_err_px);
                printf("           pixels lit %ld, penumbra %ld, umbra %ld\n",
                       st.lit_pixels, st.penumbra_pixels, st.umbra_pixels);
                printf("           curve levels %d, used %ld, bytes %zu, %.1f ms\n",
                       st.curve_levels, st.curve_levels_used, st.bytes, t);

                if (li == 1) {
                    char path[64];
                    snprintf(path, sizeof(path), "build/curve_%s_%d.tsv",
                             curve_name(cfg.curve), levels);
                    sh_pen_curve_export(path, cfg.curve, cfg.curve_exp, levels);
                }
            }
        }
        printf("\n");
        sh_free(&fast);
        sh_free(&ref);
        sh_free(&hard);
    }

    printf("Saved curve tables: build/curve_<preset>_<levels>.tsv\n");
    return 0;
}
