/* debug: locate the max-error pixel and classify its cause */
#define _POSIX_C_SOURCE 199309L
#include "shadow.h"
#include <math.h>
#include <stdio.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

int main(void) {
    int sz = 128, block = 8;
    ShadowMap full, incr;
    ShBlocks blk;
    sh_init(&full, sz, sz); sh_init(&incr, sz, sz);
    sh_blocks_init(&blk, sz, sz, block);
    float s = (float)sz;
    ShSeg segs[5] = {
        {0.15f*s, 0.45f*s, 0.45f*s, 0.45f*s},
        {0.55f*s, 0.55f*s, 0.85f*s, 0.55f*s},
        {0.30f*s, 0.70f*s, 0.30f*s, 0.90f*s},
        {0.70f*s, 0.20f*s, 0.70f*s, 0.35f*s},
        {0.40f*s, 0.80f*s, 0.60f*s, 0.80f*s},
    };
    float cx = 0.5f*s, cy = 0.5f*s, orb = 0.30f*s, lr = 0.25f*s;
    ShLight prev = {cx + orb, cy, lr, 1.0f};
    sh_render(&incr, &prev, segs, 5);
    ShIncrStats st;
    float worst = 0;
    for (int f = 1; f < 128; ++f) {
        float a = 2.0f * (float)M_PI * (float)f / 128.0f;
        ShLight cur = {cx + orb * cosf(a), cy + orb * sinf(a), lr, 1.0f};
        sh_render(&full, &cur, segs, 5);
        sh_render_incr(&incr, &blk, &prev, &cur, segs, 5, &st);
        for (int i = 0; i < sz * sz; ++i) {
            float d = fabsf(incr.buf[i] - full.buf[i]);
            if (d > worst + 1e-6f) {
                worst = d;
                int x = i % sz, y = i / sz;
                printf("frame %d pixel (%d,%d) block (%d,%d) dirty=%d incr=%.4f full=%.4f\n",
                       f, x, y, x / block, y / block,
                       st.tab[(y / block) * st.bw + x / block], incr.buf[i], full.buf[i]);
            }
        }
        prev = cur;
    }
    printf("worst %.5f\n", worst);
    return 0;
}
