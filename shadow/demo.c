#include "shadow.h"
#include <stdio.h>

int main(void) {
    const int W = 128, H = 128;
    ShLight l = {64.0f, 10.0f, 40.0f, 1.0f};
    ShSeg segs[2] = {{20.0f, 60.0f, 60.0f, 60.0f},
                     {70.0f, 70.0f, 110.0f, 70.0f}};

    /* ---- legacy grayscale (tint switch off) ---- */
    ShFrame g;
    if (sh_frame_init(&g, W, H) != 0) return 1;
    ShTune off = {0};
    if (sh_render_tinted(&g, &l, segs, 2, &off, 0) != SH_OK) return 1;
    if (sh_write_ppm_f(&g, "demo_gray.ppm") != SH_OK) return 1;
    sh_frame_free(&g);

    /* ---- tinted, warm light: stage1 light-color base, stage2 occlusion mix ---- */
    ShFrame t;
    if (sh_frame_init(&t, W, H) != 0) return 1;
    sh_frame_fill(&t, 205, 205, 205);
    ShTune warm = {0};
    warm.enable = 1;
    warm.color_source = SH_SRC_RGB;
    warm.color = (ShColor){255, 150, 50};
    warm.shadow_strength = 0.6f;
    warm.penumbra = 2.5f;
    if (sh_render_tinted(&t, &l, segs, 2, &warm, 1) != SH_OK) return 1;
    if (sh_write_ppm_f(&t, "demo_warm.ppm") != SH_OK) return 1;

    /* ---- same scene, cool swatch light ---- */
    ShFrame c;
    if (sh_frame_init(&c, W, H) != 0) return 1;
    sh_frame_fill(&c, 205, 205, 205);
    ShTune cool = {0};
    cool.enable = 1;
    cool.color_source = SH_SRC_SWATCH;
    cool.swatch_id = 1;
    cool.shadow_strength = 0.6f;
    cool.penumbra = 2.5f;
    if (sh_render_tinted(&c, &l, segs, 2, &cool, 1) != SH_OK) return 1;
    if (sh_write_ppm_f(&c, "demo_cool.ppm") != SH_OK) return 1;

    printf("wrote demo_gray.ppm demo_warm.ppm demo_cool.ppm\n");
    sh_frame_free(&t);
    sh_frame_free(&c);
    return 0;
}
