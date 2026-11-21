#include "shadow.h"
#include <stdio.h>
int main(void) {
    ShadowMap m;
    if (sh_init(&m, 128, 128) != 0) return 1;
    ShSeg segs[2] = {{20.0f, 60.0f, 60.0f, 60.0f}, {70.0f, 70.0f, 110.0f, 70.0f}};

    /* legacy point light path */
    ShLight l = {64.0f, 10.0f, 40.0f, 1.0f};
    if (sh_render(&m, &l, segs, 2) != 0) return 1;
    float sum = 0.0f;
    for (int i = 0; i < 128 * 128; ++i) sum += m.buf[i];
    printf("point  mean %.4f\n", sum / (128.0f * 128.0f));

    /* shaped strip light: directional soft shadows */
    ShShapeCfg cfg = {0};
    cfg.enable = 1;
    cfg.base = l;
    cfg.shape = SH_SHAPE_SEGMENT;
    cfg.mask_mode = SH_MASK_STRATIFIED;
    cfg.tier = SH_TIER_HIGH;
    cfg.theta = 0.0f;
    cfg.half_len = 12.0f;
    if (sh_render_cfg(&m, &cfg, segs, 2) != 0) return 1;
    sum = 0.0f;
    for (int i = 0; i < 128 * 128; ++i) sum += m.buf[i];
    printf("strip  mean %.4f\n", sum / (128.0f * 128.0f));

    int rc = sh_write_pgm(&m, "demo.pgm");
    sh_free(&m);
    return rc == 0 ? 0 : 1;
}
