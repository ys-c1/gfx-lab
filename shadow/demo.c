#include "shadow.h"
#include <stdio.h>
int main(void) {
    const int W = 128, H = 128;
    ShLight l = {64.0f, 10.0f, 40.0f, 1.0f};
    ShSeg segs[2] = {{30.0f, 20.0f, 30.0f, 110.0f},
                     {90.0f, 20.0f, 90.0f, 110.0f}};

    ShadowMap m;
    if (sh_init(&m, W, H) != 0) return 1;

    /* 规则采样（旧行为）：边缘呈 1 像素周期的网格纹。 */
    if (sh_render(&m, &l, segs, 2) != 0) return 1;
    if (sh_write_pgm(&m, "demo_regular.pgm") != 0) return 1;

    /* 低速档：2x2 = 4 采样点。 */
    int ns = 0;
    ShJitterCfg fast = {1, SH_JITTER_BAYER, SH_TIER_FAST, 2, NULL};
    if (sh_render_jit(&m, &l, segs, 2, &fast, &ns) != 0) return 1;
    if (sh_write_pgm(&m, "demo_jitter_fast.pgm") != 0) return 1;
    printf("fast tier: %d samples/pixel\n", ns);

    /* 高速档：8x8 = 64 采样点。 */
    ShJitterCfg high = {1, SH_JITTER_ROTATED, SH_TIER_HIGH, 8, NULL};
    if (sh_render_jit(&m, &l, segs, 2, &high, &ns) != 0) return 1;
    if (sh_write_pgm(&m, "demo_jitter_high.pgm") != 0) return 1;
    printf("high tier: %d samples/pixel\n", ns);

    /* 导出两档使用的抖动矩阵。 */
    float mat4[4], mat64[64];
    sh_jitter_build(SH_JITTER_BAYER, 2, mat4);
    sh_jitter_build(SH_JITTER_ROTATED, 8, mat64);
    if (sh_jitter_export("demo_jitter_2x2.txt", SH_JITTER_BAYER, 2, mat4) != 0) return 1;
    if (sh_jitter_export("demo_jitter_8x8.txt", SH_JITTER_ROTATED, 8, mat64) != 0) return 1;

    float sum = 0.0f;
    for (int i = 0; i < W * H; ++i) sum += m.buf[i];
    printf("mean %.4f\n", sum / (float)(W * H));
    sh_free(&m);
    return 0;
}
