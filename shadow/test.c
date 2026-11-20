#include "shadow.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

/* Hand-computed, occlusion-free single-light radiance at pixel centre. */
static float hand_one(const ShLight *l, float px, float py) {
    float dx = px - l->x, dy = py - l->y;
    float d = sqrtf(dx * dx + dy * dy);
    return l->intensity * (1.0f / (1.0f + d / l->radius));
}

/* Hand-computed normalised superposition of n lights, no occluders. */
static float hand_super(const ShLight *l, int n, float px, float py) {
    float num = 0.0f, den = 0.0f;
    for (int i = 0; i < n; ++i) {          /* ascending index, as specified */
        num += hand_one(&l[i], px, py);
        den += l[i].intensity;
    }
    return den > 0.0f ? num / den : 0.0f;
}

static int bit_equal(const ShadowMap *a, const ShadowMap *b) {
    return a->w == b->w && a->h == b->h &&
           memcmp(a->buf, b->buf, (size_t)a->w * (size_t)a->h * sizeof(float)) == 0;
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

    /* ---- multi-light: legacy path is bit-identical ---------------------- */
    {
        ShadowMap a, b;
        CK(sh_init(&a, 64, 64) == 0 && sh_init(&b, 64, 64) == 0);
        ShLight two[2] = {{32.0f, 8.0f, 20.0f, 1.0f}, {10.0f, 40.0f, 15.0f, 0.5f}};
        ShStats st;
        ShError e;
        CK(sh_render(&a, &two[0], &s, 1) == 0);
        CK(sh_render_lights(&b, two, 2, &s, 1, /*enable_multi=*/0, &e, &st) == SH_OK);
        CK(bit_equal(&a, &b));
        CK(st.passes == 1 && st.temp_pixels == 0 && st.blend_ops == 0);
        sh_free(&a); sh_free(&b);
    }

    /* ---- multi-light: both lights contribute --------------------------- */
    {
        ShadowMap a, b;
        CK(sh_init(&a, 64, 64) == 0 && sh_init(&b, 64, 64) == 0);
        /* L0 sees the wall between its position and the far-right pixels,
         * L1 sits on the other side and lights them. */
        ShLight two[2] = {{8.0f, 8.0f, 20.0f, 1.0f}, {56.0f, 8.0f, 20.0f, 1.0f}};
        ShSeg wall = {32.0f, 0.0f, 32.0f, 30.0f};
        ShadowMap t;
        CK(sh_init(&t, 64, 64) == 0);
        CK(sh_render(&t, &two[0], &wall, 1) == 0);          /* only L0 */
        float only_l0 = t.buf[(size_t)5 * 64 + 56];
        sh_free(&t);
        CK(sh_render_lights(&a, two, 2, &wall, 1, 1, NULL, NULL) == SH_OK);
        float both = a.buf[(size_t)5 * 64 + 56];
        float want = hand_one(&two[1], 56.5f, 5.5f) / 2.0f;  /* L0 blocked */
        CK(only_l0 < 0.01f);          /* L0 alone: in shadow           */
        CK(fabsf(both - want) < 1e-6f);
        CK(both > 0.4f);              /* L1 rescues the pixel          */
        (void)b;

        /* Peak error against the hand-computed superposition (here the
         * wall shades nothing that both lights see; pick a clear row). */
        ShadowMap c;
        CK(sh_init(&c, 64, 64) == 0);
        CK(sh_render_lights(&c, two, 2, NULL, 0, 1, NULL, NULL) == SH_OK);
        float peak_err = 0.0f, peak_val = 0.0f, peak_ref = 0.0f;
        for (int yy = 0; yy < 64; ++yy)
            for (int xx = 0; xx < 64; ++xx) {
                float ref = hand_super(two, 2, (float)xx + 0.5f, (float)yy + 0.5f);
                float got = c.buf[(size_t)yy * 64 + xx];
                float d = fabsf(got - ref);
                if (d > peak_err) peak_err = d;
                if (got > peak_val) { peak_val = got; peak_ref = ref; }
            }
        CK(peak_err <= 1e-6f);
        CK(fabsf(peak_val - peak_ref) <= 1e-6f);
        sh_free(&a); sh_free(&c);
    }

    /* ---- stats on the two-stage path ----------------------------------- */
    {
        ShStats st;
        ShLight two[2] = {{8.0f, 8.0f, 20.0f, 1.0f}, {50.0f, 8.0f, 20.0f, 0.5f}};
        CK(sh_render_lights(&m, two, 2, &s, 1, 1, NULL, &st) == SH_OK);
        CK(st.passes == 2);
        CK(st.temp_pixels == 2u * 64u * 64u);
        CK(st.blend_ops == 2u * 64u * 64u);
        CK(sh_render_lights(&m, two, 1, &s, 1, 1, NULL, &st) == SH_OK);
        CK(st.passes == 1 && st.temp_pixels == 64u * 64u && st.blend_ops == 64u * 64u);
    }

    /* ---- errors: zero lights, negative intensity, coincident lights ----- */
    {
        ShError e;
        CK(sh_render_lights(&m, NULL, 0, &s, 1, 1, &e, NULL) == SH_ENOLIGHT);
        CK(e.light == -1);
        CK(strstr(e.msg, "no lights") != NULL);

        ShLight neg[2] = {{8.0f, 8.0f, 20.0f, 1.0f}, {50.0f, 8.0f, 20.0f, -0.25f}};
        CK(sh_render_lights(&m, neg, 2, &s, 1, 1, &e, NULL) == SH_EBADLIGHT);
        CK(e.light == 1);
        CK(strstr(e.msg, "#1") != NULL);
        /* legacy mode validates too, even though it only draws lights[0] */
        CK(sh_render_lights(&m, neg, 2, &s, 1, 0, &e, NULL) == SH_EBADLIGHT);
        CK(e.light == 1);

        ShLight dup[3] = {{8.0f, 8.0f, 20.0f, 1.0f},
                          {50.0f, 8.0f, 20.0f, 1.0f},
                          {8.0f, 8.0f, 30.0f, 2.0f}};
        CK(sh_render_lights(&m, dup, 3, &s, 1, 1, &e, NULL) == SH_EDUP);
        CK(e.light == 2 && e.other == 0);
        CK(strstr(e.msg, "#2") != NULL && strstr(e.msg, "#0") != NULL);

        ShLight zero_r[2] = {{8.0f, 8.0f, 20.0f, 1.0f}, {50.0f, 8.0f, 0.0f, 1.0f}};
        CK(sh_render_lights(&m, zero_r, 2, &s, 1, 1, &e, NULL) == SH_EBADLIGHT);
        CK(e.light == 1);

        /* zero total intensity is valid, yields an all-zero map, no NaN */
        ShLight dark[2] = {{8.0f, 8.0f, 20.0f, 0.0f}, {50.0f, 8.0f, 20.0f, 0.0f}};
        CK(sh_render_lights(&m, dark, 2, &s, 1, 1, NULL, NULL) == SH_OK);
        int all_zero = 1;
        for (int i = 0; i < 64 * 64; ++i)
            if (!(m.buf[i] == 0.0f)) { all_zero = 0; break; }
        CK(all_zero);
    }

    sh_free(&m);
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
