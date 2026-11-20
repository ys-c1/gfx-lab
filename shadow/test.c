#include "shadow.h"
#include <math.h>
#include <stdio.h>
static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

int main(void) {
    ShadowMap m;
    CK(sh_init(&m, 64, 64) == 0);
    ShLight l = {32.0f, 8.0f, 20.0f, 1.0f};
    ShSeg s = {10.0f, 20.0f, 54.0f, 20.0f};
    CK(sh_render(&m, &l, &s, 1) == 0);
    CK(m.buf[(size_t)32 * 64 + 32] < 0.01f);
    CK(m.buf[(size_t)2 * 64 + 32] > 0.5f);
    CK(sh_render(&m, &l, NULL, 0) == 0);
    ShLight bad = {0.0f, 0.0f, 0.0f, 1.0f};
    CK(sh_render(&m, &bad, &s, 1) == -2);

    /* --- incremental path --- */
    ShBlocks b;
    CK(sh_blocks_init(&b, 64, 64, 0) == -3);      /* block size zero must be rejected */
    CK(sh_blocks_init(&b, 64, 64, 8) == 0);
    CK(b.bw == 8 && b.bh == 8);

    ShLight l2 = {40.0f, 12.0f, 20.0f, 1.0f};
    ShIncrStats st;
    /* non-finite light positions must be rejected (parameter named on stderr) */
    ShLight nanl = {NAN, 8.0f, 20.0f, 1.0f};
    ShLight infl = {32.0f, INFINITY, 20.0f, 1.0f};
    CK(sh_render_incr(&m, &b, &nanl, &l2, &s, 1, &st) == -4);
    CK(sh_render_incr(&m, &b, &l, &infl, &s, 1, &st) == -4);

    /* full render with the old light as the starting frame */
    CK(sh_render(&m, &l, &s, 1) == 0);
    CK(sh_render_incr(&m, &b, &l, &l2, &s, 1, &st) == 0);
    CK(st.bw == 8 && st.bh == 8 && st.tab != 0);
    CK(st.hit > 0 && st.hit <= 64);
    int tabsum = 0;
    for (int i = 0; i < st.bw * st.bh; ++i) tabsum += st.tab[i];
    CK(tabsum == st.hit);                        /* block table matches hit count */

    /* incremental result approximates a full re-render with the new light */
    ShadowMap ref;
    CK(sh_init(&ref, 64, 64) == 0);
    CK(sh_render(&ref, &l2, &s, 1) == 0);
    float maxd = 0.0f;
    for (int i = 0; i < 64 * 64; ++i) {
        float d = fabsf(m.buf[i] - ref.buf[i]);
        if (d > maxd) maxd = d;
    }
    CK(maxd < 0.02f);

    /* identical consecutive lights: nothing is marked, buffer untouched */
    CK(sh_render_incr(&m, &b, &l2, &l2, &s, 1, &st) == 0);
    CK(st.hit == 0);
    maxd = 0.0f;
    for (int i = 0; i < 64 * 64; ++i) {
        float d = fabsf(m.buf[i] - ref.buf[i]);
        if (d > maxd) maxd = d;
    }
    CK(maxd < 0.02f);

    /* incremental disabled: sh_render must stay bit-identical to itself */
    ShadowMap a, c;
    CK(sh_init(&a, 64, 64) == 0);
    CK(sh_init(&c, 64, 64) == 0);
    CK(sh_render(&a, &l2, &s, 1) == 0);
    CK(sh_render(&c, &l2, &s, 1) == 0);
    int same = 1;
    for (int i = 0; i < 64 * 64; ++i) if (a.buf[i] != c.buf[i]) same = 0;
    CK(same);

    sh_free(&a); sh_free(&c); sh_free(&ref);
    sh_blocks_free(&b);
    sh_free(&m);
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
