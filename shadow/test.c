#include "shadow.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

static ShLight L = {512.0f, 128.0f, 220.0f, 1.0f};

int main(void) {
    /* ---- legacy single-map behavior unchanged ---- */
    ShadowMap m;
    CK(sh_init(&m, 64, 64) == 0);
    ShSeg s = {10.0f, 20.0f, 54.0f, 20.0f};
    ShLight legacy = {32.0f, 8.0f, 20.0f, 1.0f};
    CK(sh_render(&m, &legacy, &s, 1) == 0);
    CK(m.buf[(size_t)32 * 64 + 32] < 0.01f);
    CK(m.buf[(size_t)2 * 64 + 32] > 0.5f);
    CK(sh_render(&m, &legacy, NULL, 0) == 0);
    ShLight bad = {0.0f, 0.0f, 0.0f, 1.0f};
    CK(sh_render(&m, &bad, &s, 1) == -2);
    sh_free(&m);

    /* ---- error: zero tiers, message names the field ---- */
    ShCascade c;
    ShTierCfg dummy = {0.0f, 100.0f, 64};
    CK(sh_csm_build(&c, 0.0f, 0.0f, &dummy, 0) == SH_ERR_TIERS);
    CK(strstr(sh_csm_strerror(SH_ERR_TIERS), "tiers") != NULL);
    CK(sh_csm_build(&c, 0.0f, 0.0f, NULL, 2) == SH_ERR_ARG);

    /* ---- error: inverted distance band, message names near/far ---- */
    ShTierCfg inv1[2] = {{0.0f, 200.0f, 256}, {200.0f, 150.0f, 256}};
    CK(sh_csm_build(&c, 0.0f, 0.0f, inv1, 2) == SH_ERR_RANGE);
    CK(strstr(sh_csm_strerror(SH_ERR_RANGE), "near/far") != NULL);
    ShTierCfg inv2[1] = {{50.0f, 50.0f, 256}};          /* far == near */
    CK(sh_csm_build(&c, 0.0f, 0.0f, inv2, 1) == SH_ERR_RANGE);
    ShTierCfg inv3[1] = {{-1.0f, 50.0f, 256}};          /* negative near */
    CK(sh_csm_build(&c, 0.0f, 0.0f, inv3, 1) == SH_ERR_RANGE);

    /* ---- error: bad resolution, message names res ---- */
    ShTierCfg badres[2] = {{0.0f, 200.0f, 256}, {200.0f, 400.0f, 0}};
    CK(sh_csm_build(&c, 0.0f, 0.0f, badres, 2) == SH_ERR_RES);
    CK(strstr(sh_csm_strerror(SH_ERR_RES), "res") != NULL);

    /* ---- phase 1: build the 256/1024 two-tier table, queryable ---- */
    ShTierCfg cfg2[2] = {{0.0f, 256.0f, 1024}, {256.0f, 512.0f, 256}};
    CK(sh_csm_build(&c, 512.0f, 512.0f, cfg2, 2) == SH_OK);
    const ShTier *tab = NULL; int nt = 0;
    CK(sh_csm_table(&c, &tab, &nt) == SH_OK && nt == 2 && tab != NULL);
    CK(tab[0].res == 1024 && tab[1].res == 256);
    CK(tab[0].near == 0.0f && tab[0].far == 256.0f);
    CK(tab[1].near == 256.0f && tab[1].far == 512.0f);
    CK(fabsf(tab[0].texel - 0.5f) < 1e-6f);             /* 512/1024 */
    CK(fabsf(tab[1].texel - 4.0f) < 1e-6f);             /* 1024/256 */
    CK(fabsf(tab[0].x0 - 256.0f) < 1e-6f && fabsf(tab[0].x1 - 768.0f) < 1e-6f);

    /* ---- boundary convention: [near, far), exact boundary -> farther tier */
    CK(sh_csm_pick(&c, 0.0f) == 0);
    CK(sh_csm_pick(&c, 255.99f) == 0);
    CK(sh_csm_pick(&c, 256.0f) == 1);                   /* boundary point */
    CK(sh_csm_pick(&c, 511.99f) == 1);
    CK(sh_csm_pick(&c, 512.0f) == -1);                  /* beyond last far */
    CK(sh_csm_pick(&c, -1.0f) == -1);

    /* ---- phase 2: render + compose ---- */
    ShSeg segs[3] = {
        {300.0f, 380.0f, 480.0f, 380.0f},               /* near-field wall  */
        {600.0f, 560.0f, 760.0f, 560.0f},               /* far-field wall   */
        {420.0f, 600.0f, 420.0f, 760.0f}
    };
    ShCsmStats st = {0};
    CK(sh_csm_render(&c, &L, segs, 3, &st) == SH_OK);
    CK(st.ray_tests > 0 && st.texels == 1024L * 1024L + 256L * 256L);

    ShadowMap out;
    CK(sh_init(&out, 512, 512) == 0);
    CK(sh_csm_compose(&c, &out, 0.0f, 0.0f, 1024.0f, 1024.0f, &st) == SH_OK);
    CK(st.composed + st.uncovered == 512L * 512L);
    CK(st.uncovered > 0);                               /* corners past r=512 */
    sh_csm_free(&c);
    sh_free(&out);

    /* ---- single-tier CSM reproduces sh_render bit for bit ----
     * Camera at the box center; far=64 covers every corner (max distance
     * 45.25) and res=128 makes the texel exactly 1.0 world unit, so output
     * texels map 1:1 onto tier texels sampling the same world points. */
    ShTierCfg cfg1[1] = {{0.0f, 64.0f, 128}};
    CK(sh_csm_build(&c, 32.0f, 32.0f, cfg1, 1) == SH_OK);
    CK(sh_csm_render(&c, &legacy, &s, 1, NULL) == SH_OK);
    ShadowMap ref, got;
    CK(sh_init(&ref, 64, 64) == 0 && sh_init(&got, 64, 64) == 0);
    CK(sh_render(&ref, &legacy, &s, 1) == 0);
    ShCsmStats st1 = {0};
    CK(sh_csm_compose(&c, &got, 0.0f, 0.0f, 64.0f, 64.0f, &st1) == SH_OK);
    CK(st1.uncovered == 0);
    int same = 1;
    for (int i = 0; i < 64 * 64; ++i)
        if (ref.buf[i] != got.buf[i]) { same = 0; break; }
    CK(same);
    CK(sh_csm_table(&c, &tab, &nt) == SH_OK && nt == 1);
    sh_csm_free(&c);
    sh_free(&ref); sh_free(&got);

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
