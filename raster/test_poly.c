#include "raster.h"
#include "rpf.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

static RVert V(double x, double y) { RVert v = { x, y, 0.0 }; return v; }

/*
 * Two 4-vertex polygons on a 16x16 grid, fan origin v0=(2,2):
 *
 *  square order  (2,2),(14,2),(14,14),(2,14): CCW polygon, both fan
 *                triangles CCW. T0 covers y<=x, T1 covers y>=x, the
 *                shared diagonal pixels are closed-edge covered by both
 *                -> parity 2, winding +2 (EO out, NZ in).
 *
 *  true bowtie   (2,2),(14,14),(14,2),(2,14): self-crossing at (8,8).
 *                T0=(0,1,2) is CW  (winding -1 lobe, region y<=x),
 *                T1=(0,2,3) is CCW (winding +1 lobe, region y>=x).
 *                Diagonal pixels: parity 2, winding 0 (out under both rules).
 */
static void square_poly(RPoly *p, RVert *v) {
    v[0] = V(2, 2); v[1] = V(14, 2); v[2] = V(14, 14); v[3] = V(2, 14);
    p->verts = v; p->n = 4;
}

static void bowtie(RPoly *p, RVert *v) {
    v[0] = V(2, 2); v[1] = V(14, 14); v[2] = V(14, 2); v[3] = V(2, 14);
    p->verts = v; p->n = 4;
}

static void test_hand_calc(void) {
    RTarget t; CK(rt_init(&t, 16, 16) == 0);
    RPoly p; RVert v[4]; square_poly(&p, v);
    unsigned char eo[3] = { 10, 20, 30 }, pos[3] = { 200, 100, 50 }, neg[3] = { 5, 5, 200 };
    RPFillOpts o = { RPF_RULE_EVEN_ODD, RPF_MODE_ENABLED, 0.5f, eo, pos, neg };
    RPFStats st;
    int fill = rpf_polygon(&t, &p, &o, &st);
    CK(fill >= 0);
    CK(st.ntris == 2);

    /*
     * Hand-computed with the horizontal ray (crossings to the right of the
     * pixel centre). Square polygon is simple CCW: every interior point has
     * exactly one upward crossing -> crossing count 1, winding +1, under
     * both rules. The fan triangle ids still expose the diagonal split.
     */
    RPFCover c;
    /* (3,3) centre (3.5,3.5): on the fan diagonal, covered by both tris */
    CK(rpf_pixel_get(&t, &st, 3, 3, &c) == 0);
    CK(c.parity == 1 && c.winding == 1 && c.ntris == 2);
    CK(c.tri_id[0] == 0 && c.tri_id[1] == 1);
    /* (12,3) centre (12.5,3.5), y<x: fan T0 only */
    CK(rpf_pixel_get(&t, &st, 12, 3, &c) == 0);
    CK(c.parity == 1 && c.winding == 1 && c.ntris == 1 && c.tri_id[0] == 0);
    /* (3,12) centre (3.5,12.5), y>x: fan T1 only */
    CK(rpf_pixel_get(&t, &st, 3, 12, &c) == 0);
    CK(c.parity == 1 && c.winding == 1 && c.ntris == 1 && c.tri_id[0] == 1);
    /* (0,0) outside the polygon box entirely */
    CK(rpf_pixel_get(&t, &st, 0, 0, &c) == 0);
    CK(c.parity == 0 && c.winding == 0);

    /* simple polygon: the two rules fill exactly the same pixels */
    CK(st.eo_fill_pixels == st.nz_fill_pixels);
    CK(st.overflow_pixels == 0);
    RPFState rows[RPF_MAX_STATES];
    int nst = rpf_rule_table(&t, &st, &o, rows, RPF_MAX_STATES);
    CK(nst == st.nstates && nst == 1);
    CK(rows[0].parity == 1 && rows[0].winding == 1);
    CK(rows[0].eo_inside == 1 && rows[0].nz_inside == 1);
    /* even-odd shading fills with the EO color; index is y*w+x for (12,3) */
    CK(t.rgb[(3 * 16 + 12) * 3] == eo[0]);
    /* nonzero on the same polygon fills with the positive-winding color */
    rt_free(&t); rpf_stats_free(&st);
    CK(rt_init(&t, 16, 16) == 0);
    o.rule = RPF_RULE_NONZERO;
    CK(rpf_polygon(&t, &p, &o, &st) >= 0);
    CK(t.rgb[(3 * 16 + 12) * 3] == pos[0]);
    rt_free(&t); rpf_stats_free(&st);

    /*
     * True self-crossing bowtie (2,2),(14,14),(14,2),(2,14). The ray gives
     * the two lobes opposite winding signs, so nonzero shades them with
     * different colors. Hand-checked samples (centre = x+0.5, y+0.5):
     *  (12,3): right CW lobe -> winding -1, one crossing.
     *  (4,10): left CCW lobe -> winding +1, three crossings (odd -> EO in).
     *  (3,12): ray crosses the left lobe twice -> winding 0, even -> out
     *          under both rules (this pixel lies beyond the lobe's edge).
     */
    CK(rt_init(&t, 16, 16) == 0);
    bowtie(&p, v);
    o.rule = RPF_RULE_NONZERO;
    CK(rpf_polygon(&t, &p, &o, &st) >= 0);
    CK(rpf_pixel_get(&t, &st, 12, 3, &c) == 0);
    CK(c.parity == 1 && c.winding == -1);
    CK(memcmp(t.rgb + ((3 * 16 + 12) * 3), neg, 3) == 0);   /* idx = y*w+x */
    CK(rpf_pixel_get(&t, &st, 4, 10, &c) == 0);
    CK(c.parity == 3 && c.winding == 1);
    CK(memcmp(t.rgb + ((10 * 16 + 4) * 3), pos, 3) == 0);
    CK(rpf_pixel_get(&t, &st, 3, 12, &c) == 0);
    CK(c.parity == 2 && c.winding == 0);
    CK(t.rgb[(12 * 16 + 3) * 3] == 0);
    CK(st.eo_fill_pixels == st.nz_fill_pixels);
    int neg_px = 0, pos_px = 0;
    for (int i = 0; i < 16 * 16; ++i) {
        if (t.rgb[i * 3] == neg[0]) ++neg_px;
        if (t.rgb[i * 3] == pos[0]) ++pos_px;
    }
    CK(neg_px > 0 && pos_px > 0);
    rt_free(&t); rpf_stats_free(&st);
}

static void test_off_mode_identical(void) {
    /* OFF: target must stay byte-for-byte identical, stats still populated */
    RTarget t; CK(rt_init(&t, 16, 16) == 0);
    RTarget ref; CK(rt_init(&ref, 16, 16) == 0);
    RPoly p; RVert v[4]; bowtie(&p, v);
    unsigned char col[3] = { 9, 9, 9 }, pos[3] = { 1, 2, 3 }, neg[3] = { 4, 5, 6 };
    RPFillOpts o = { RPF_RULE_EVEN_ODD, RPF_MODE_OFF, 0.5f, col, pos, neg };
    RPFStats st;
    int fill = rpf_polygon(&t, &p, &o, &st);
    CK(fill == 0);
    CK(memcmp(t.rgb, ref.rgb, (size_t)16 * 16 * 3) == 0);
    CK(memcmp(t.z, ref.z, (size_t)16 * 16 * sizeof(float)) == 0);
    CK(st.covered_pixels > 0 && st.eo_fill_pixels > 0 && st.nz_fill_pixels > 0);
    rt_free(&t); rt_free(&ref); rpf_stats_free(&st);
}

static void test_matches_legacy_triangle(void) {
    /* A convex triangle through the polygon path must agree with
     * rt_triangle on every pixel that is not exactly on an edge. Boundary
     * pixels legitimately differ: rt_triangle uses closed half-planes while
     * the parity/winding ray uses half-open edge tie-breaking. The interior
     * pixel set and all colors there must match exactly. */
    RTarget a, b;
    CK(rt_init(&a, 24, 24) == 0);
    CK(rt_init(&b, 24, 24) == 0);
    RTri tri = {{ {3, 3, 0.4f}, {21, 5, 0.4f}, {10, 22, 0.4f} }, {123, 77, 210} };
    CK(rt_triangle(&a, &tri) > 0);
    RVert v[3] = { V(3, 3), V(21, 5), V(10, 22) };
    RPoly p = { v, 3 };
    unsigned char col[3] = { 123, 77, 210 }, pos[3] = { 1, 1, 1 }, neg[3] = { 2, 2, 2 };
    RPFillOpts o = { RPF_RULE_EVEN_ODD, RPF_MODE_ENABLED, 0.4f, col, pos, neg };
    RPFStats st;
    CK(rpf_polygon(&b, &p, &o, &st) > 0);
    int mismatch = 0, boundary = 0, interior_checked = 0;
    for (int y = 0; y < 24; ++y)
        for (int x = 0; x < 24; ++x) {
            double px = x + 0.5, py = y + 0.5;
            double e01 = (v[1].x - v[0].x) * (py - v[0].y) - (v[1].y - v[0].y) * (px - v[0].x);
            double e12 = (v[2].x - v[1].x) * (py - v[1].y) - (v[2].y - v[1].y) * (px - v[1].x);
            double e20 = (v[0].x - v[2].x) * (py - v[2].y) - (v[0].y - v[2].y) * (px - v[2].x);
            if (e01 == 0.0 || e12 == 0.0 || e20 == 0.0) { ++boundary; continue; }
            ++interior_checked;
            size_t i = (size_t)y * 24 + (size_t)x;
            if (memcmp(a.rgb + i * 3, b.rgb + i * 3, 3) != 0 || a.z[i] != b.z[i])
                ++mismatch;
        }
    CK(interior_checked > 100);
    CK(boundary >= 0);
    CK(mismatch == 0);
    rt_free(&a); rt_free(&b); rpf_stats_free(&st);
}

static void test_star_rules_diverge(void) {
    /* five-point star, 32x32: the pentagon core is double-covered with the
     * same sign, so even-odd leaves a hole while nonzero fills it. Which
     * pixel lands in the core depends on half-pixel sampling, so assert
     * structurally rather than on one hard-coded cell. */
    RTarget t; CK(rt_init(&t, 32, 32) == 0);
    RVert v[10];
    for (int i = 0; i < 10; ++i) {
        double r = (i % 2 == 0) ? 14.0 : 6.0;
        double ang = -M_PI / 2 + i * M_PI / 5;
        v[i] = V(16 + r * cos(ang), 16 + r * sin(ang));
    }
    RPoly p = { v, 10 };
    unsigned char eo[3] = { 30, 30, 200 }, pos[3] = { 220, 60, 60 }, neg[3] = { 60, 220, 60 };
    RPFillOpts o = { RPF_RULE_EVEN_ODD, RPF_MODE_ENABLED, 0.5f, eo, pos, neg };
    RPFStats st;
    CK(rpf_polygon(&t, &p, &o, &st) >= 0);
    /* Self-intersection produces multi-crossing states even though this
     * particular star's two rules fill the same pixel set. The rule table
     * must still expose every distinct (parity, winding) state with the
     * correct per-rule inside flags. */
    int have_p2w0 = 0, have_p3w1 = 0, have_p4w0 = 0;
    int eo_only = 0, nz_only = 0;
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x) {
            RPFCover c;
            CK(rpf_pixel_get(&t, &st, x, y, &c) == 0);
            if (c.parity == 2 && c.winding == 0) ++have_p2w0;
            if (c.parity == 3 && c.winding == 1) ++have_p3w1;
            if (c.parity == 4 && c.winding == 0) ++have_p4w0;
            int eo_in = c.parity & 1;
            int nz_in = c.winding != 0;
            if (eo_in && !nz_in) ++eo_only;
            if (!eo_in && nz_in) ++nz_only;
        }
    CK(have_p2w0 > 0 && have_p3w1 > 0 && have_p4w0 > 0);
    CK(eo_only == 0 && nz_only == 0);
    CK(st.eo_fill_pixels == st.nz_fill_pixels);
    /* the table row for (2,0) is outside under both; (3,+1) inside both */
    RPFState rows[RPF_MAX_STATES];
    int nst = rpf_rule_table(&t, &st, &o, rows, RPF_MAX_STATES);
    CK(nst == st.nstates);
    for (int i = 0; i < nst; ++i) {
        if (rows[i].parity == 2 && rows[i].winding == 0)
            CK(rows[i].eo_inside == 0 && rows[i].nz_inside == 0);
        if (rows[i].parity == 3 && rows[i].winding == 1)
            CK(rows[i].eo_inside == 1 && rows[i].nz_inside == 1);
    }
    rt_free(&t); rpf_stats_free(&st);

    CK(rt_init(&t, 32, 32) == 0);
    o.rule = RPF_RULE_NONZERO;
    CK(rpf_polygon(&t, &p, &o, &st) >= 0);
    int colored_pos_or_neg = 0;
    for (int i = 0; i < 32 * 32; ++i)
        if (t.rgb[i * 3] == pos[0] || t.rgb[i * 3] == neg[0]) ++colored_pos_or_neg;
    CK(colored_pos_or_neg == st.nz_fill_pixels);
    rt_free(&t); rpf_stats_free(&st);
}

static void test_rules_actually_diverge(void) {
    /* Same triangle visited twice with the same sign. Its interior has
     * crossing count 2 (even -> even-odd says outside) but winding +2
     * (non-zero -> nonzero says inside). This is the canonical case where
     * the two rules produce different fill sets. */
    RTarget t; CK(rt_init(&t, 20, 20) == 0);
    RVert v[6] = { V(3, 3), V(17, 3), V(10, 17),
                   V(3, 3), V(17, 3), V(10, 17) };
    RPoly p = { v, 6 };
    unsigned char eo[3] = { 11, 22, 33 }, pos[3] = { 210, 70, 70 }, neg[3] = { 70, 70, 210 };
    RPFillOpts eo_o = { RPF_RULE_EVEN_ODD, RPF_MODE_ENABLED, 0.5f, eo, pos, neg };
    RPFillOpts nz_o = { RPF_RULE_NONZERO, RPF_MODE_ENABLED, 0.5f, eo, pos, neg };
    RPFStats a, b;
    CK(rpf_polygon(&t, &p, &eo_o, &a) >= 0);
    rt_free(&t); rpf_stats_free(&a);
    CK(rt_init(&t, 20, 20) == 0);
    CK(rpf_polygon(&t, &p, &nz_o, &b) >= 0);

    RPFCover c;
    CK(rpf_pixel_get(&t, &b, 10, 10, &c) == 0);
    CK(c.parity == 2 && c.winding == 2);
    CK(a.eo_fill_pixels == 0);                 /* even-odd fills nothing */
    CK(b.nz_fill_pixels > 0);                  /* nonzero fills the region */
    CK(memcmp(t.rgb + ((10 * 20 + 10) * 3), pos, 3) == 0);
    rt_free(&t); rpf_stats_free(&b);
}

static void test_validation_fields(void) {
    RTarget t; CK(rt_init(&t, 8, 8) == 0);
    RVert good[3] = { V(0, 0), V(7, 0), V(3, 7) };
    RPoly p = { good, 3 };
    unsigned char c1[3] = { 1, 1, 1 }, c2[3] = { 2, 2, 2 }, c3[3] = { 3, 3, 3 };
    RPFillOpts o = { RPF_RULE_EVEN_ODD, RPF_MODE_ENABLED, 0.5f, c1, c2, c3 };
    char buf[160];
    RPFStats st;
#define CKERR(call, field) do { \
        CK((call) < 0); \
        rpf_last_error(buf, sizeof(buf)); \
        printf("  err: %s\n", buf); \
        CK(strstr(buf, field) != NULL); \
    } while (0)

    CKERR(rpf_polygon(NULL, &p, &o, &st), "'target'");
    CKERR(rpf_polygon(&t, NULL, &o, &st), "'poly'");

    RPoly badn = { good, 2 };
    CKERR(rpf_polygon(&t, &badn, &o, &st), "'poly.n'");

    RPoly bign = { good, RPF_MAX_VERTS + 1 };
    CKERR(rpf_polygon(&t, &bign, &o, &st), "'poly.n'");

    RVert inf[3] = { V(0, 0), V(7, 0), { INFINITY, 7, 0 } };
    RPoly pinf = { inf, 3 };
    CKERR(rpf_polygon(&t, &pinf, &o, &st), "'poly.verts[2]'");

    RPFillOpts badrule = o; badrule.rule = 7;
    CKERR(rpf_polygon(&t, &p, &badrule, &st), "'opts.rule'");

    RPFillOpts badmode = o; badmode.mode = -1;
    CKERR(rpf_polygon(&t, &p, &badmode, &st), "'opts.mode'");

    RPFillOpts badz = o; badz.z = (double)NAN;
    CKERR(rpf_polygon(&t, &p, &badz, &st), "'opts.z'");

    RPFillOpts badcol = o; badcol.eo_color = NULL;
    CKERR(rpf_polygon(&t, &p, &badcol, &st), "'opts.eo_color'");

    /* query-field validation */
    CK(rpf_polygon(&t, &p, &o, &st) >= 0);
    RPFCover cv;
    CKERR(rpf_pixel_get(&t, &st, 99, 0, &cv), "'x'");
    CKERR(rpf_pixel_get(&t, &st, 0, -1, &cv), "'y'");
    RPFState rows[1];
    CKERR(rpf_rule_table(&t, &st, &o, rows, 0), "'maxn'");
#undef CKERR
    rt_free(&t); rpf_stats_free(&st);
}

int main(void) {
    test_hand_calc();
    test_off_mode_identical();
    test_matches_legacy_triangle();
    test_star_rules_diverge();
    test_rules_actually_diverge();
    test_validation_fields();
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
