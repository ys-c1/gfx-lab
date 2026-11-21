#include "shadow.h"
#include <math.h>
#include <stdio.h>
static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

static int max_abs_diff(const ShadowMap *a, const ShadowMap *b) {
    int n = a->w * a->h;
    float worst = 0.0f;
    for (int i = 0; i < n; ++i) {
        float d = fabsf(a->buf[i] - b->buf[i]);
        if (d > worst) worst = d;
    }
    return worst < 1e-6f;
}

/* independent brute-force reference: every emitter sample, every pixel.
 * same physical model as the renderer: mean emission-cosine-weighted visibility */
static void exhaustive_ref(ShadowMap *m, const ShShapeCfg *cfg, const ShSeg *segs, int nseg, int S) {
    float ax = cosf(cfg->theta), ay = sinf(cfg->theta);
    for (int y = 0; y < m->h; ++y) {
        for (int x = 0; x < m->w; ++x) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float wvis = 0.0f;
            for (int s = 0; s < S; ++s) {
                float t = ((float)s + 0.5f) * 2.0f / (float)S - 1.0f;
                float ex = cfg->base.x + t * cfg->half_len * ax;
                float ey = cfg->base.y + t * cfg->half_len * ay;
                float rx = px - ex, ry = py - ey;
                float rlen = sqrtf(rx * rx + ry * ry);
                float cos_e = rlen > 1e-9f ? fabsf(ax * ry - ay * rx) / rlen : 0.0f;
                int occ = 0;
                for (int i = 0; i < nseg; ++i) {
                    float sx = segs[i].x1 - segs[i].x0, sy = segs[i].y1 - segs[i].y0;
                    float den = rx * sy - ry * sx;
                    if (fabsf(den) < 1e-9f) continue;
                    float tt = ((segs[i].x0 - ex) * sy - (segs[i].y0 - ey) * sx) / den;
                    float uu = ((segs[i].x0 - ex) * ry - (segs[i].y0 - ey) * rx) / den;
                    if (tt > 0.0f && tt < 1.0f && uu >= 0.0f && uu <= 1.0f) { occ = 1; break; }
                }
                if (!occ) wvis += cos_e / (float)S;
            }
            float dx = px - cfg->base.x, dy = py - cfg->base.y;
            float d = sqrtf(dx * dx + dy * dy);
            float fall = 1.0f / (1.0f + d / cfg->base.radius);
            m->buf[(size_t)y * m->w + x] = cfg->base.intensity * fall * wvis;
        }
    }
}

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

    /* switch off: sh_render_cfg must reproduce sh_render bit for bit */
    ShadowMap a, b;
    CK(sh_init(&a, 64, 64) == 0 && sh_init(&b, 64, 64) == 0);
    ShShapeCfg off = {0};
    off.enable = 0; off.base = l;
    off.shape = SH_SHAPE_SEGMENT; off.mask_mode = SH_MASK_STRATIFIED;
    off.tier = SH_TIER_LOW; off.theta = 0.0f; off.half_len = 6.0f;
    CK(sh_render(&a, &l, &s, 1) == 0);
    CK(sh_render_cfg(&b, &off, &s, 1) == 0);
    CK(max_abs_diff(&a, &b));

    /* conflict: shape flags name both parties */
    ShShapeCfg c1 = off; c1.enable = 1;
    c1.shape = SH_SHAPE_POINT | SH_SHAPE_SEGMENT;
    CK(sh_render_cfg(&m, &c1, &s, 1) == -3);
    CK(sh_errmsg() != NULL);
    printf("  conflict(shape): %s\n", sh_errmsg());
    ShShapeCfg c2 = off; c2.enable = 1;
    c2.mask_mode = SH_MASK_STRATIFIED | SH_MASK_EXHAUSTIVE;
    CK(sh_render_cfg(&m, &c2, &s, 1) == -3);
    printf("  conflict(mask):  %s\n", sh_errmsg());
    ShShapeCfg c3 = off; c3.enable = 1;
    c3.tier = SH_TIER_LOW | SH_TIER_HIGH;
    CK(sh_render_cfg(&m, &c3, &s, 1) == -3);
    printf("  conflict(tier):  %s\n", sh_errmsg());

    /* segment without half_len is invalid, not a crash */
    ShShapeCfg c4 = off; c4.enable = 1; c4.half_len = 0.0f;
    CK(sh_render_cfg(&m, &c4, &s, 1) == -4);

    /* stage 1: mask build, selection counts, normalization and CSV export */
    ShShapeCfg low = off; low.enable = 1;
    ShDirMask mk;
    CK(sh_mask_build(&low, &mk) == 0);
    CK(mk.bins == 8 && mk.samples == 12);
    for (int b = 0; b < mk.bins; ++b) {
        int cnt = 0; float wsum = 0.0f;
        for (int s2 = 0; s2 < mk.samples; ++s2) {
            cnt += mk.sel[(size_t)b * mk.samples + s2];
            wsum += mk.weight[(size_t)b * mk.samples + s2];
        }
        CK(cnt == 3);
        CK(fabsf(wsum - 1.0f) < 1e-5f);
    }
    CK(sh_mask_write_csv(&mk, "build/mask_low.csv") == 0);

    /* directionality: horizontal strip, no blocker — at equal distance from
       the emitter center, a broadside pixel must receive more light than an
       end-on pixel (both bins pick k samples, falloff is identical) */
    ShadowMap dm;
    CK(sh_init(&dm, 96, 96) == 0);
    ShShapeCfg dir = off; dir.enable = 1;
    dir.base.x = 48.0f; dir.base.y = 48.0f; dir.base.radius = 40.0f;
    dir.theta = 0.0f; dir.half_len = 14.0f; dir.tier = SH_TIER_HIGH;
    CK(sh_render_shaped(&dm, &dir, NULL, 0) == 0);
    float side = dm.buf[(size_t)(48 - 20) * 96 + 48];  /* 20px broadside */
    float endp = dm.buf[(size_t)48 * 96 + (48 + 20)];  /* 20px end-on */
    CK(side > endp);
    printf("  directionality side=%.4f end=%.4f\n", side, endp);
    /* rotating the strip 90 degrees must swap the two sides */
    dir.theta = (float)M_PI / 2.0f;
    CK(sh_render_shaped(&dm, &dir, NULL, 0) == 0);
    float side_r = dm.buf[(size_t)(48 - 20) * 96 + 48];
    float end_r  = dm.buf[(size_t)48 * 96 + (48 + 20)];
    CK(end_r > side_r);
    printf("  rotated 90deg   side=%.4f end=%.4f\n", side_r, end_r);

    /* exhaustive mode equals brute-force reference within round-off */
    ShShapeCfg exh = low; exh.mask_mode = SH_MASK_EXHAUSTIVE; exh.samples = 32; exh.bins = 8;
    ShadowMap e1, e2;
    CK(sh_init(&e1, 64, 64) == 0 && sh_init(&e2, 64, 64) == 0);
    CK(sh_render_shaped(&e1, &exh, &s, 1) == 0);
    exhaustive_ref(&e2, &exh, &s, 1, 32);
    CK(max_abs_diff(&e1, &e2));
    CK(sh_mask_write_csv(&(ShDirMask){0}, "build/none.csv") == -1);

    sh_mask_free(&mk);
    sh_free(&m); sh_free(&a); sh_free(&b); sh_free(&dm); sh_free(&e1); sh_free(&e2);
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
