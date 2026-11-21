#include "shadow.h"
#include <stdio.h>
int main(void) {
    enum { W = 128, H = 128 };
    ShadowMap m;
    if (sh_init(&m, W, H) != 0) return 1;
    float cov[W * H];
    ShPenumbra pen;
    /* 方形面光：半宽/半高 8，上方 10 处；下方两道墙。 */
    ShLight l = {64.0f, 10.0f, 40.0f, 1.0f, 8.0f, 8.0f, 0.0f};
    ShSeg segs[2] = {{20.0f, 60.0f, 60.0f, 60.0f}, {70.0f, 70.0f, 110.0f, 70.0f}};
    const ShRenderCfg *cfg = sh_preset_cfg(SH_GEAR_SLOW);
    if (sh_render_ex(&m, cov, &pen, &l, cfg, segs, 2) != 0) {
        printf("render failed: %s\n", sh_last_error());
        return 1;
    }
    float sum = 0.0f;
    for (int i = 0; i < W * H; ++i) sum += m.buf[i];
    float csum = 0.0f;
    for (int i = 0; i < W * H; ++i) csum += cov[i];
    printf("gear=SLOW points=%d mean %.4f coverage %.4f penumbra avg %.2f max %.2f px\n",
           cfg->cols * cfg->rows, sum / (float)(W * H), csum / (float)(W * H),
           pen.width_avg, pen.width_max);
    int rc = sh_write_pgm(&m, "demo.pgm");
    sh_free(&m);
    return rc == 0 ? 0 : 1;
}
