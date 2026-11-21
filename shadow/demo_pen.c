#include "shadow.h"
#include "penumbra.h"

#include <stdio.h>

int main(void) {
    ShadowMap m;
    if (sh_init(&m, 128, 128) != 0) return 1;

    /* Same scene as the legacy demo, so the hard image is unchanged. */
    ShLight l = {64.0f, 10.0f, 40.0f, 1.0f};
    ShSeg segs[2] = {{20.0f, 60.0f, 60.0f, 60.0f}, {70.0f, 70.0f, 110.0f, 70.0f}};

    /* 1. Legacy hard edge, written exactly as before. */
    if (sh_render(&m, &l, segs, 2) != 0) return 1;
    float sum = 0.0f;
    for (int i = 0; i < 128 * 128; ++i) sum += m.buf[i];
    printf("hard   mean %.4f\n", sum / (128.0f * 128.0f));
    if (sh_write_pgm(&m, "demo.pgm") != 0) return 1;

    /* 2. Curve-weighted penumbra in two stages. */
    ShPenCfg cfg;
    sh_pen_cfg_default(&cfg);
    cfg.enable = 1;
    cfg.src_radius = 8.0f;
    cfg.curve = SH_CURVE_DISK;
    cfg.curve_levels = 256;

    ShError err;
    ShPenStats st;
    if (sh_render_pen(&m, &l, segs, 2, &cfg, &st, &err) != SH_PEN_OK) {
        printf("soft failed: %s (%s)\n", err.msg, err.field ? err.field : "-");
        return 1;
    }
    sum = 0.0f;
    for (int i = 0; i < 128 * 128; ++i) sum += m.buf[i];
    printf("soft   mean %.4f\n", sum / (128.0f * 128.0f));
    printf("       pixels lit %ld, penumbra %ld, umbra %ld\n",
           st.lit_pixels, st.penumbra_pixels, st.umbra_pixels);
    printf("       width %.2f px / analytic %.2f px / err %.2f px, curve %d lv %d\n",
           st.width_px, st.width_expected_px, st.width_err_px,
           st.curve, st.curve_levels);
    if (sh_write_pgm(&m, "demo_soft.pgm") != 0) return 1;
    if (sh_pen_curve_export("demo_curve.tsv", cfg.curve, cfg.curve_exp,
                            cfg.curve_levels) != SH_PEN_OK) return 1;

    sh_free(&m);
    return 0;
}
