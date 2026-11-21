#include "shadow.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

/* Set a float field through the typed API. */
static int setf(ShTruncCfg *c, ShField f, float v) {
    return sh_cfg_set(c, f, SH_TY_FLOAT, &v, NULL);
}
static int seti(ShTruncCfg *c, ShField f, int v) {
    return sh_cfg_set(c, f, SH_TY_INT, &v, NULL);
}

static int same_bits(const ShadowMap *a, const ShadowMap *b) {
    if (a->w != b->w || a->h != b->h) return 0;
    return memcmp(a->buf, b->buf, (size_t)a->w * (size_t)a->h * sizeof(float)) == 0;
}

/* Independent per-pixel reference: for each pixel, shadowed iff some original
 * segment blocks it AND the pixel is within `cutoff` of the light; brightness
 * from the receiver tier.  Mirrors the documented semantics, not the code. */
static int reference_render(const ShadowMap *m, const ShLight *l,
                            const ShSeg *segs, int nseg, const ShTruncCfg *cfg,
                            float *out) {
    for (int y = 0; y < m->h; ++y)
        for (int x = 0; x < m->w; ++x) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float dx = px - l->x, dy = py - l->y;
            float d = sqrtf(dx * dx + dy * dy);
            int shadowed = 0;
            if ((double)d <= (double)cfg->cutoff) {
                for (int i = 0; i < nseg; ++i) {
                    const ShSeg *s = &segs[i];
                    double rx = (double)px - l->x, ry = (double)py - l->y;
                    double sx = (double)s->x1 - s->x0, sy = (double)s->y1 - s->y0;
                    double den = rx * sy - ry * sx;
                    if (fabs(den) < 1e-18) continue;
                    double t = (((double)s->x0 - l->x) * sy - ((double)s->y0 - l->y) * sx) / den;
                    double u = (((double)s->x0 - l->x) * ry - ((double)s->y0 - l->y) * rx) / den;
                    if (t > 0.0 && t < 1.0 && u >= 0.0 && u <= 1.0) { shadowed = 1; break; }
                }
            }
            float fall = 1.0f / (1.0f + d / l->radius);
            float v = l->intensity * fall;
            if (shadowed) {
                int tier = cfg->tier_count - 1;
                for (int k = 0; k < cfg->tier_count - 1; ++k)
                    if ((double)d <= (double)cfg->split[k]) { tier = k; break; }
                v *= cfg->bright[tier];
            }
            out[(size_t)y * m->w + x] = v;
        }
    return 0;
}

/* A few scenes with blockers at different distances from the light. */
static int scene_count(const int which, ShSeg *segs) {
    switch (which) {
    case 0: /* the original test: one near horizontal blocker */
        segs[0] = (ShSeg){10.0f, 20.0f, 54.0f, 20.0f};
        return 1;
    case 1: /* near and far blockers: truncation should drop the far one */
        segs[0] = (ShSeg){10.0f, 20.0f, 54.0f, 20.0f};
        segs[1] = (ShSeg){12.0f, 50.0f, 52.0f, 50.0f};
        return 2;
    case 2: /* long segment crossing several tier boundaries (off-diagonal so
             * no pixel centre lies exactly on the blocker: that is a float
             * knife-edge of the original renderer, not of truncation) */
        segs[0] = (ShSeg){5.0f, 7.0f, 58.0f, 59.0f};
        return 1;
    case 3: /* two segments end to end, split at different tiers */
        segs[0] = (ShSeg){8.0f, 30.0f, 30.0f, 30.0f};
        segs[1] = (ShSeg){30.0f, 30.0f, 56.0f, 30.0f};
        return 2;
    default:
        return 0;
    }
}

static ShTruncCfg tiered_cfg(void) {
    ShTruncCfg c;
    sh_cfg_default(&c);
    seti(&c, SH_F_ENABLE, 1);
    seti(&c, SH_F_TIER_COUNT, 3);
    setf(&c, SH_F_CUTOFF, 40.0f);
    setf(&c, SH_F_SPLIT_0, 14.0f);
    setf(&c, SH_F_SPLIT_1, 26.0f);
    setf(&c, SH_F_BRIGHT_0, 0.0f);
    setf(&c, SH_F_BRIGHT_1, 0.5f);
    setf(&c, SH_F_BRIGHT_2, 0.85f);
    return c;
}

int main(void) {
    /* ---- legacy behaviour, unchanged ---- */
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

    /* keep a copy of the legacy map of the big scene for the disabled check */
    ShadowMap legacy;
    CK(sh_init(&legacy, 96, 96) == 0);
    ShLight l2 = {30.0f, 6.0f, 24.0f, 0.9f};
    ShSeg sc[2];
    int nsc = scene_count(1, sc);
    CK(sh_render(&legacy, &l2, sc, nsc) == 0);

    /* ---- disabled: byte-identical to the legacy path ---- */
    {
        ShTruncCfg c;
        sh_cfg_default(&c);          /* enable = 0 */
        CK(c.enable == 0 && c.tier_count == 1);
        ShadowMap out;
        CK(sh_init(&out, 96, 96) == 0);
        ShCastSet cs;
        ShRenderStats st;
        ShError e;
        CK(sh_render_cast(&out, &l2, sc, nsc, &c, &cs, &st, &e) == SH_OK);
        CK(same_bits(&out, &legacy));
        CK(cs.count == 0 && cs.pieces == NULL);   /* disabled allocates nothing */
        CK(st.pixels_total == 0);
        /* enable explicitly set to 0 behaves the same */
        seti(&c, SH_F_ENABLE, 0);
        CK(sh_render_cast(&out, &l2, sc, nsc, &c, NULL, NULL, &e) == SH_OK);
        CK(same_bits(&out, &legacy));
        sh_free(&out);
    }

    /* ---- typed setter: type mismatch names the field ---- */
    {
        ShError e;
        ShTruncCfg c;
        sh_cfg_default(&c);
        int iv = 8;
        float fv = 8.0f;
        CK(sh_cfg_set(&c, SH_F_CUTOFF, SH_TY_INT, &iv, &e) == SH_ERR_TYPE);
        CK(e.code == SH_ERR_TYPE && e.field && strcmp(e.field, "cutoff") == 0);
        CK(c.cutoff == 0.0f);                     /* rejected: not stored */
        CK(sh_cfg_set(&c, SH_F_CUTOFF, SH_TY_NONE, &iv, &e) == SH_ERR_TYPE);
        CK(strcmp(e.field, "cutoff") == 0);
        CK(sh_cfg_set(&c, SH_F_TIER_COUNT, SH_TY_FLOAT, &fv, &e) == SH_ERR_TYPE);
        CK(strcmp(e.field, "tier_count") == 0);
        CK(sh_cfg_set(&c, SH_F_BRIGHT_2, SH_TY_INT, &iv, &e) == SH_ERR_TYPE);
        CK(strcmp(e.field, "bright_2") == 0);
        CK(sh_cfg_set(&c, SH_F_ENABLE, SH_TY_FLOAT, &fv, &e) == SH_ERR_TYPE);
        CK(strcmp(e.field, "enable") == 0);
        /* correct types are accepted */
        CK(sh_cfg_set(&c, SH_F_CUTOFF, SH_TY_FLOAT, &fv, &e) == SH_OK);
        int tc = 3, en = 1;
        CK(sh_cfg_set(&c, SH_F_TIER_COUNT, SH_TY_INT, &tc, &e) == SH_OK);
        CK(sh_cfg_set(&c, SH_F_ENABLE, SH_TY_INT, &en, &e) == SH_OK);
        CK(sh_cfg_set(&c, SH_F_ENABLE, SH_TY_FLOAT, &fv, &e) == SH_ERR_TYPE);
        CK(e.field && strcmp(e.field, "enable") == 0);
        CK(sh_cfg_set(&c, (ShField)99, SH_TY_INT, &iv, &e) == SH_ERR_FIELD);
        CK(sh_cfg_set(&c, SH_F_CUTOFF, SH_TY_FLOAT, NULL, &e) == SH_ERR_NULL);
        CK(e.field && strcmp(e.field, "cutoff") == 0);
    }

    /* ---- range errors name the field too ---- */
    {
        ShError e;
        ShTruncCfg c;
        sh_cfg_default(&c);
        seti(&c, SH_F_ENABLE, 1);
        float neg = -1.0f, big = 1.5f, zero = 0.0f, nan = NAN;
        int bad_enable = 7, bad_tiers = 5;
        CK(seti(&c, SH_F_ENABLE, bad_enable) == SH_ERR_RANGE);
        CK(sh_cfg_set(&c, SH_F_ENABLE, SH_TY_INT, &bad_enable, &e) == SH_ERR_RANGE);
        CK(e.field && strcmp(e.field, "enable") == 0);
        CK(sh_cfg_set(&c, SH_F_TIER_COUNT, SH_TY_INT, &bad_tiers, &e) == SH_ERR_RANGE);
        CK(strcmp(e.field, "tier_count") == 0);
        CK(sh_cfg_set(&c, SH_F_CUTOFF, SH_TY_FLOAT, &neg, &e) == SH_ERR_RANGE);
        CK(strcmp(e.field, "cutoff") == 0);
        CK(sh_cfg_set(&c, SH_F_CUTOFF, SH_TY_FLOAT, &nan, &e) == SH_ERR_RANGE);
        CK(sh_cfg_set(&c, SH_F_SPLIT_0, SH_TY_FLOAT, &neg, &e) == SH_ERR_RANGE);
        CK(strcmp(e.field, "split_0") == 0);
        CK(sh_cfg_set(&c, SH_F_BRIGHT_1, SH_TY_FLOAT, &big, &e) == SH_ERR_RANGE);
        CK(strcmp(e.field, "bright_1") == 0);
        CK(sh_cfg_set(&c, SH_F_BRIGHT_0, SH_TY_FLOAT, &zero, &e) == SH_OK);
    }

    /* ---- cross-field validation names the field ---- */
    {
        ShError e;
        ShTruncCfg c = tiered_cfg();
        CK(sh_cfg_validate(&c, &e) == SH_OK);
        c.split[0] = 30.0f; c.split[1] = 20.0f;      /* not ascending */
        CK(sh_cfg_validate(&c, &e) == SH_ERR_ORDER);
        CK(strcmp(e.field, "split_1") == 0);
        c = tiered_cfg();
        c.split[1] = 60.0f;                          /* above cutoff */
        CK(sh_cfg_validate(&c, &e) == SH_ERR_ORDER);
        CK(strcmp(e.field, "split_1") == 0);
        c = tiered_cfg();
        c.cutoff = 0.0f;                             /* enabled needs cutoff */
        CK(sh_cfg_validate(&c, &e) == SH_ERR_RANGE);
        CK(strcmp(e.field, "cutoff") == 0);
        c = tiered_cfg();
        c.enable = 0;                                /* disabled: ignored */
        c.cutoff = 0.0f; c.split[0] = 99.0f;
        CK(sh_cfg_validate(&c, &e) == SH_OK);
    }

    /* ---- stage A: truncation statistics ---- */
    {
        ShCastSet cs;
        ShError e;
        ShTruncCfg c = tiered_cfg();
        ShSeg segs[2];
        int n = scene_count(1, segs);
        CK(sh_truncate(&cs, &l2, segs, n, &c, &e) == SH_OK);
        CK(cs.input_count == 2);
        CK(cs.cutoff == 40.0f && cs.tier_count == 3);
        CK(cs.count >= cs.input_count);            /* pieces >= inputs */
        CK(cs.dropped + cs.split + cs.clipped <= cs.input_count);
        int sum = 0;
        for (int k = 0; k < c.tier_count; ++k) sum += cs.per_tier[k];
        CK(sum == cs.count);
        /* every piece lies within the cutoff (small tolerance for the pad) */
        int inside = 1;
        for (int i = 0; i < cs.count; ++i)
            if (cs.pieces[i].d0 > 40.0f + 1e-3f || cs.pieces[i].d1 > 40.0f + 1e-3f) inside = 0;
        CK(inside);
        CK(cs.len_after <= cs.len_before + 1e-3);
        /* the far blocker is at light-distance ~44-46 > cutoff 40 */
        CK(cs.dropped >= 1);
        int far_seen = 0;
        for (int i = 0; i < cs.count; ++i)
            if (cs.pieces[i].src == 1 && cs.pieces[i].seg.y0 > 40.0f) far_seen = 1;
        CK(!far_seen);
        sh_castset_free(&cs);

        /* a disabled config cannot be truncated */
        ShTruncCfg off;
        sh_cfg_default(&off);
        CK(sh_truncate(&cs, &l2, segs, n, &off, &e) == SH_ERR_STATE);
        CK(e.field && strcmp(e.field, "enable") == 0);
        /* NULL args */
        CK(sh_truncate(&cs, NULL, segs, n, &c, &e) == SH_ERR_NULL);
        CK(sh_truncate(NULL, &l2, segs, n, &c, &e) == SH_ERR_NULL);
        /* non-finite light */
        ShLight nl = l2; nl.x = NAN;
        CK(sh_truncate(&cs, &nl, segs, n, &c, &e) == SH_ERR_LIGHT);
        /* empty input is valid */
        CK(sh_truncate(&cs, &l2, NULL, 0, &c, &e) == SH_OK);
        CK(cs.count == 0 && cs.pieces != NULL);
        sh_castset_free(&cs);
    }

    /* ---- pieces match the analytic kept length ---- */
    {
        /* A horizontal blocker through the light's height is fully inside,
         * so pieces must add up to its length; a vertical segment crossing
         * one split is checked against the exact chord length. */
        ShCastSet cs;
        ShError e;
        ShTruncCfg c;
        sh_cfg_default(&c);
        seti(&c, SH_F_ENABLE, 1);
        seti(&c, SH_F_TIER_COUNT, 2);
        setf(&c, SH_F_CUTOFF, 100.0f);       /* chord length of the split */
        setf(&c, SH_F_SPLIT_0, 20.0f);
        ShLight ll = {50.0f, 50.0f, 10.0f, 1.0f};
        ShSeg vs = {30.0f, 10.0f, 30.0f, 90.0f};  /* crosses both 20 and 100 */
        CK(sh_truncate(&cs, &ll, &vs, 1, &c, &e) == SH_OK);
        /* The vertical line sits 20 px from the light, so the split circle
         * r=20 touches it tangentially (two roots at y=50, coalesced to one
         * breakpoint).  The r=100 circle contains the whole segment. */
        CK(cs.count == 2);
        CK(cs.split == 1 && cs.clipped == 0 && cs.dropped == 0);
        CK(cs.pieces[0].seg.y0 == 10.0f && cs.pieces[1].seg.y1 == 90.0f);
        CK(fabsf(cs.pieces[0].seg.y1 - 50.0f) < 1e-3f);
        CK(fabsf(cs.pieces[1].seg.y0 - 50.0f) < 1e-3f);
        CK(cs.per_tier[0] + cs.per_tier[1] == 2);
        sh_castset_free(&cs);

        /* cutting a segment that pokes out of the circle clips exactly the
         * part beyond the analytic half-chord */
        ShSeg poking = {50.0f, 40.0f, 50.0f, 200.0f};   /* r=20 around (50,50) */
        ShTruncCfg c20; sh_cfg_default(&c20);
        seti(&c20, SH_F_ENABLE, 1); seti(&c20, SH_F_TIER_COUNT, 1);
        setf(&c20, SH_F_CUTOFF, 20.0f);
        CK(sh_truncate(&cs, &ll, &poking, 1, &c20, &e) == SH_OK);
        CK(cs.count == 1 && cs.clipped == 1);
        /* starts at d=10, circle r=20 leaves at y=70: exactly 30 kept */
        CK(fabs(cs.len_after - 30.0) < 1e-3);
        CK(fabsf(cs.pieces[0].seg.y1 - 70.0f) < 1e-3f);
        sh_castset_free(&cs);

        /* segment wholly outside cutoff -> dropped */
        ShSeg faraway = {500.0f, 500.0f, 520.0f, 500.0f};
        CK(sh_truncate(&cs, &ll, &faraway, 1, &c, &e) == SH_OK);
        CK(cs.count == 0 && cs.dropped == 1);
        sh_castset_free(&cs);

        /* degenerate point inside / outside */
        ShSeg pt_in = {55.0f, 55.0f, 55.0f, 55.0f};
        CK(sh_truncate(&cs, &ll, &pt_in, 1, &c, &e) == SH_OK);
        CK(cs.count == 1);
        sh_castset_free(&cs);
        ShSeg pt_out = {500.0f, 55.0f, 500.0f, 55.0f};
        CK(sh_truncate(&cs, &ll, &pt_out, 1, &c, &e) == SH_OK);
        CK(cs.count == 0);
        sh_castset_free(&cs);
    }

    /* ---- stage B vs per-pixel reference, every scene ---- */
    {
        const int sizes[2] = {64, 192};
        ShError e;
        for (int sz = 0; sz < 2; ++sz) {
            int S = sizes[sz];
            ShadowMap mm, ref;
            CK(sh_init(&mm, S, S) == 0);
            CK(sh_init(&ref, S, S) == 0);
            double scale = (double)S / 64.0;
            float *r = (float*)malloc((size_t)S * (size_t)S * sizeof(float));
            for (int w = 0; w < 4; ++w) {
                ShSeg segs[2];
                int n = scene_count(w, segs);
                for (int i = 0; i < n; ++i) {
                    segs[i].x0 = (float)(segs[i].x0 * scale);
                    segs[i].y0 = (float)(segs[i].y0 * scale);
                    segs[i].x1 = (float)(segs[i].x1 * scale);
                    segs[i].y1 = (float)(segs[i].y1 * scale);
                }
                ShLight ll = {(float)(32.0 * scale), (float)(8.0 * scale),
                              (float)(20.0 * scale), 1.0f};
                ShTruncCfg c = tiered_cfg();
                c.cutoff = (float)(40.0 * scale);
                c.split[0] = (float)(14.0 * scale);
                c.split[1] = (float)(26.0 * scale);
                ShCastSet cs;
                ShRenderStats st;
                CK(sh_truncate(&cs, &ll, segs, n, &c, &e) == SH_OK);
                CK(sh_render_truncated(&mm, &ll, &cs, &c, &st, &e) == SH_OK);
                CK(st.pixels_total == (long)S * S);
                long pt = 0, ps = 0;
                for (int k = 0; k < c.tier_count; ++k) { pt += st.pixels[k]; ps += st.shadowed[k]; }
                CK(pt == (long)S * S);
                CK(ps == st.shadowed_total);
                CK(st.beyond_cutoff >= 0 && st.beyond_cutoff < st.pixels_total);
                reference_render(&mm, &ll, segs, n, &c, r);
                int bad = 0;
                for (long i = 0; i < (long)S * S; ++i) {
                    float a = mm.buf[i], b = r[i];
                    if (fabsf(a - b) > 1e-5f) ++bad;
                }
                CK(bad == 0);
                sh_castset_free(&cs);
            }
            free(r);
            sh_free(&mm);
            sh_free(&ref);
        }
    }

    /* ---- brightness tiers are visible in the output ---- */
    {
        ShError e;
        ShTruncCfg c = tiered_cfg();
        c.split[0] = 20.0f;      /* so the near shadow spans all three tiers */
        ShSeg segs[2];
        int n = scene_count(1, segs);
        ShadowMap mm;
        CK(sh_init(&mm, 64, 64) == 0);
        CK(sh_render_cast(&mm, &l2, segs, n, &c, NULL, NULL, &e) == SH_OK);
        /* Scan a column behind the near blocker and compare each shadowed
         * pixel with lit * bright[tier], recomputing the tier independently. */
        int seen_t0 = 0, seen_t1 = 0, bad_tier = 0;
        for (int y = 0; y < 64; ++y) {
            float px = 30.5f, py = (float)y + 0.5f;
            float dx = px - l2.x, dy = py - l2.y;
            float d = sqrtf(dx * dx + dy * dy);
            float lit = l2.intensity / (1.0f + d / l2.radius);
            float v = mm.buf[(size_t)y * 64 + 30];
            if (d < c.cutoff && v < lit * 0.99f) {   /* shadowed */
                int tier = 2;
                if (d <= c.split[0]) tier = 0;
                else if (d <= c.split[1]) tier = 1;
                if (fabsf(v - lit * c.bright[tier]) > 1e-5f) bad_tier = 1;
                if (tier == 0) seen_t0 = 1;
                if (tier == 1) seen_t1 = 1;
            }
        }
        CK(bad_tier == 0);
        CK(seen_t0);
        CK(seen_t1);
        /* nothing shadowed past the cutoff: truncation really stops there */
        int far_shadow = 0;
        for (int y = 0; y < 64; ++y) {
            float py = (float)y + 0.5f, px = 30.5f;
            float dx = px - l2.x, dy = py - l2.y, d = sqrtf(dx * dx + dy * dy);
            float lit = l2.intensity / (1.0f + d / l2.radius);
            if (d > c.cutoff + 1.0f && mm.buf[(size_t)y * 64 + 30] < lit - 1e-4f) far_shadow = 1;
        }
        CK(!far_shadow);
        sh_free(&mm);
    }

    /* ---- stage B rejects a mismatched cast set ---- */
    {
        ShError e;
        ShTruncCfg c = tiered_cfg();
        ShSeg segs[2];
        int n = scene_count(1, segs);
        ShCastSet cs;
        ShadowMap mm;
        CK(sh_init(&mm, 64, 64) == 0);
        CK(sh_truncate(&cs, &l2, segs, n, &c, &e) == SH_OK);
        ShTruncCfg c2 = c;
        c2.cutoff = 30.0f;
        CK(sh_render_truncated(&mm, &l2, &cs, &c2, NULL, &e) == SH_ERR_STATE);
        c2 = c;
        c2.split[1] = 20.0f;
        CK(sh_render_truncated(&mm, &l2, &cs, &c2, NULL, &e) == SH_ERR_STATE);
        CK(e.field && strcmp(e.field, "split_1") == 0);
        c2 = c;
        c2.enable = 0;
        CK(sh_render_truncated(&mm, &l2, &cs, &c2, NULL, &e) == SH_ERR_STATE);
        CK(e.field && strcmp(e.field, "enable") == 0);
        /* a different brightness table is legal */
        ShTruncCfg c3 = c;
        c3.bright[1] = 0.25f;
        CK(sh_render_truncated(&mm, &l2, &cs, &c3, NULL, &e) == SH_OK);
        CK(sh_render_truncated(NULL, &l2, &cs, &c, NULL, &e) == SH_ERR_NULL);
        ShLight badl = l2; badl.radius = 0.0f;
        CK(sh_render_truncated(&mm, &badl, &cs, &c, NULL, &e) == SH_ERR_LIGHT);
        sh_castset_free(&cs);
        sh_free(&mm);
    }

    /* ---- enabled path rejects invalid lights with the legacy code ---- */
    {
        ShError e;
        ShTruncCfg c = tiered_cfg();
        ShLight badl = {0.0f, 0.0f, 0.0f, 1.0f};
        ShadowMap mm;
        CK(sh_init(&mm, 32, 32) == 0);
        CK(sh_render_cast(&mm, &badl, &s, 1, &c, NULL, NULL, &e) == SH_ERR_LIGHT);
        CK(sh_render_cast(&mm, &badl, &s, 1, &c, NULL, NULL, &e) == -2);
        /* NULL map still reports null on the disabled path via sh_render() */
        ShTruncCfg off; sh_cfg_default(&off);
        CK(sh_render_cast(NULL, &l, &s, 1, &off, NULL, NULL, &e) == SH_ERR_NULL);
        sh_free(&mm);
    }

    /* ---- truncation is idempotent and order-independent ---- */
    {
        ShError e;
        ShTruncCfg c = tiered_cfg();
        ShSeg segs[2];
        int n = scene_count(3, segs);
        ShCastSet a, b;
        CK(sh_truncate(&a, &l2, segs, n, &c, &e) == SH_OK);
        CK(sh_truncate(&b, &l2, segs, n, &c, &e) == SH_OK);
        CK(a.count == b.count);
        int same = 1;
        for (int i = 0; i < a.count; ++i)
            if (memcmp(&a.pieces[i], &b.pieces[i], sizeof(ShCastPiece)) != 0) same = 0;
        CK(same);
        /* truncating the pieces again is stable (they are inside cutoff) */
        ShSeg *again = (ShSeg*)malloc(sizeof(ShSeg) * (size_t)a.count);
        for (int i = 0; i < a.count; ++i) again[i] = a.pieces[i].seg;
        ShCastSet d;
        CK(sh_truncate(&d, &l2, again, a.count, &c, &e) == SH_OK);
        CK(d.count == a.count);         /* truncation is closed */
        free(again);
        sh_castset_free(&a); sh_castset_free(&b); sh_castset_free(&d);
    }

    sh_free(&m);
    sh_free(&legacy);
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
