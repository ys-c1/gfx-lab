#include "mip.h"
#include <stdio.h>
#include <stdlib.h>
int main(void) {
    enum { W = 64, H = 64 };
    unsigned char *src = (unsigned char*)malloc(W*H*3);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            unsigned char *p = src + (y*W+x)*3;
            p[0] = (unsigned char)(x * 4); p[1] = (unsigned char)(y * 4); p[2] = 128;
        }

    MipPlan plan;
    if (mip_plan(W, H, 0, &plan, NULL) != 0) return 1;
    printf("full chain: levels=%d bytes=%zu\n", plan.full_levels, plan.total_bytes);

    /* 演示降级：预算只允许前 3 层 */
    size_t budget = plan.level_bytes[0] + plan.level_bytes[1] + plan.level_bytes[2];
    MipChain c;
    if (mip_build(&c, src, W, H, budget) != 0) return 1;
    printf("budget=%zu -> levels=%d cap=%d degraded=%d sizes %dx%d .. %dx%d\n",
           budget, c.levels, c.cap_levels, c.degraded,
           c.w[0], c.h[0], c.w[c.levels-1], c.h[c.levels-1]);
    mip_free(&c);

    /* 默认（无预算）结果与旧版一致：完整链直到 1x1 */
    if (mip_build(&c, src, W, H, 0) != 0) return 1;
    printf("default: levels=%d cap=%d degraded=%d sizes %dx%d %dx%d\n",
           c.levels, c.cap_levels, c.degraded, c.w[0], c.h[0], c.w[c.levels-1], c.h[c.levels-1]);
    mip_free(&c);
    free(src);
    return 0;
}
