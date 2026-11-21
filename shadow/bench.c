#include "shadow.h"
#include <stdio.h>
#include <stdlib.h>

/*
 * benchmark: slow (brute force) and fast (spatial grid) gears, each run
 * with the tint switch off (gray baseline) and on (recolored). Reports
 * per run: tint error, mean blend ratio, pixel counts, wall time, and the
 * recolor color difference (CIE76 delta E) against the gray output.
 * Every tinted run is cross-checked against the exhaustive reference.
 */

static void print_header(void) {
    printf("== shadow tint benchmark ==\n");
    printf("swatch cards available (%d):\n", sh_swatch_count());
    for (int i = 0; i < sh_swatch_count(); ++i) {
        ShColor c; char name[32];
        sh_swatch_get(i, &c, name, sizeof name);
        printf("  [%d] %-10s rgb=(%3u,%3u,%3u)\n", i, name, c.r, c.g, c.b);
    }
    printf("\n");
    printf("%-7s %-8s %8s %10s %12s %10s %12s %12s %12s %10s %10s\n",
           "gear", "tint", "time(ms)", "pixels", "shadow_px",
           "mean_occ", "mean_mix", "tint_err", "max_err", "dE_mean", "dE_max");
}

static void run_one(int gear, const ShTune *tune, const ShLight *light,
                    int w, int h, int nseg, const unsigned char bg[3]) {
    ShFrame f;
    if (sh_frame_init(&f, w, h) != SH_OK) return;
    ShBench b;
    int rc = sh_bench_run(&f, light, nseg, tune, gear, &b);
    if (rc != SH_OK) {
        printf("bench failed: %s\n", sh_strerror(rc));
        sh_frame_free(&f);
        return;
    }

    long mismatch = -1;
    double maxdiff = -1.0;
    if (tune->enable) {
        /* rebuild the exact deterministic scene for the cross-check */
        ShSeg *segs = malloc(sizeof(ShSeg) * (size_t)nseg);
        sh_test_scene(w, h, nseg, segs);
        rc = sh_bench_verify_exhaustive(&f, light, segs, nseg, tune, bg,
                                        &mismatch, &maxdiff);
        free(segs);
        if (rc != SH_OK) printf("verify failed: %s\n", sh_strerror(rc));
    }

    printf("%-7s %-8s %8.3f %10ld %12ld %10.4f %12.4f %12.4f %12.2f %10.3f %10.3f",
           gear == 0 ? "slow" : "fast",
           tune->enable ? "on" : "off",
           b.time_ms, b.pixels, b.shadow_pixels,
           b.mean_occlusion, b.mean_mix, b.tint_err, b.max_tint_err,
           b.recolor_de, b.recolor_de_max);
    if (tune->enable)
        printf("  | exhaustive mismatch=%ld max_ch_diff=%.2f", mismatch, maxdiff);
    printf("\n");
    sh_frame_free(&f);
}

int main(void) {
    const int W = 192, H = 192, NSEG = 120;
    ShLight light = {96.0f, 20.0f, 60.0f, 1.0f};
    unsigned char bg[3] = {210, 210, 210};

    ShTune off = {0};
    ShTune warm = {0};
    warm.enable = 1;
    warm.color_source = SH_SRC_RGB;
    warm.color = (ShColor){255, 140, 40};   /* warm lamp */
    warm.shadow_strength = 0.55f;
    warm.penumbra = 3.0f;

    ShTune cool = {0};
    cool.enable = 1;
    cool.color_source = SH_SRC_SWATCH;
    cool.swatch_id = 1;                     /* cool-white card */
    cool.shadow_strength = 0.55f;
    cool.penumbra = 3.0f;

    print_header();
    for (int gear = 0; gear <= 1; ++gear) {
        run_one(gear, &off,  &light, W, H, NSEG, bg);
        run_one(gear, &warm, &light, W, H, NSEG, bg);
        run_one(gear, &cool, &light, W, H, NSEG, bg);
    }

    /* conflict demo: two color sources configured at once */
    printf("\nconflict check:\n");
    ShTune broken = warm;
    broken.preset = SH_PRESET_DUSK;       /* also sets a preset */
    char a[32], b[32];
    int rc = sh_tune_check(&broken, a, sizeof a, b, sizeof b);
    printf("  rc=%d (%s): side A='%s' side B='%s'\n", rc, sh_strerror(rc), a, b);
    return 0;
}
