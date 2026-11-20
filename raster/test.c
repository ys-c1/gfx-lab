#include "raster.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

/* ---- reference: the original one-pass rasteriser, kept verbatim ------------
 * Same edge function, same depth test, same float -> buffer stores.  With
 * blending disabled the new two-pass rasteriser must match it exactly. */
static double ref_edge(const RVert *a, const RVert *b, double px, double py) {
    return (b->x - a->x) * (py - a->y) - (b->y - a->y) * (px - a->x);
}

static int ref_triangle(RTarget *t, const RTri *tri) {
    double area = ref_edge(&tri->v[0], &tri->v[1], tri->v[2].x, tri->v[2].y);
    if (fabs(area) < 1e-12) return -2;
    int minx = (int)floor(fmin(fmin(tri->v[0].x, tri->v[1].x), tri->v[2].x));
    int maxx = (int)ceil(fmax(fmax(tri->v[0].x, tri->v[1].x), tri->v[2].x));
    int miny = (int)floor(fmin(fmin(tri->v[0].y, tri->v[1].y), tri->v[2].y));
    int maxy = (int)ceil(fmax(fmax(tri->v[0].y, tri->v[1].y), tri->v[2].y));
    if (minx < 0) { minx = 0; }
    if (miny < 0) { miny = 0; }
    if (maxx >= t->w) maxx = t->w - 1;
    if (maxy >= t->h) maxy = t->h - 1;
    int written = 0;
    for (int y = miny; y <= maxy; ++y) {
        for (int x = minx; x <= maxx; ++x) {
            double px = x + 0.5, py = y + 0.5;
            double w0 = ref_edge(&tri->v[1], &tri->v[2], px, py);
            double w1 = ref_edge(&tri->v[2], &tri->v[0], px, py);
            double w2 = ref_edge(&tri->v[0], &tri->v[1], px, py);
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            double sum = w0 + w1 + w2;
            double z = (w0 * tri->v[0].z + w1 * tri->v[1].z + w2 * tri->v[2].z) / sum;
            size_t i = (size_t)y * (size_t)t->w + (size_t)x;
            if ((float)z < t->z[i]) {
                t->z[i] = (float)z;
                t->rgb[i*3+0] = tri->rgb[0];
                t->rgb[i*3+1] = tri->rgb[1];
                t->rgb[i*3+2] = tri->rgb[2];
                ++written;
            }
        }
    }
    return written;
}

static void scene(RTri out[5]) {
    /* deliberately overlapping in depth and in screen space, at 64x64 */
    RTri s[5] = {
        {{ {0,0,0.9}, {63,0,0.9}, {0,63,0.9} }, {10, 20, 30}, 1.0f},
        {{ {0,0,0.3}, {40,0,0.3}, {0,40,0.3} }, {90, 80, 70}, 1.0f},
        {{ {5,5,0.6}, {60,10,0.55}, {20,60,0.4} }, {200, 120, 60}, 1.0f},
        {{ {32,0,0.2}, {63,30,0.2}, {40,63,0.2} }, {0, 255, 0}, 1.0f},
        {{ {8,8,0.85}, {56,20,0.8}, {30,55,0.75} }, {240, 240, 240}, 1.0f},
    };
    memcpy(out, s, sizeof(s));
}

/* ---- 1. blending off: pixel-for-pixel identical to the original ----------- */
static void test_off_matches_reference(void) {
    RTarget a, b;
    CK(rt_init(&a, 64, 64) == 0);
    CK(rt_init(&b, 64, 64) == 0);
    RTri s[5]; scene(s);
    int wa = 0, wb = 0;
    for (int i = 0; i < 5; ++i) { wa += rt_triangle(&a, &s[i]); wb += ref_triangle(&b, &s[i]); }
    CK(wa == wb);
    CK(memcmp(a.rgb, b.rgb, (size_t)64*64*3) == 0);
    CK(memcmp(a.z, b.z, sizeof(float) * 64 * 64) == 0);
    /* an explicit "no blending" request also matches, even for alpha < 1:
     * legacy behaviour ignores alpha entirely when blending is off */
    CK(rt_set_blend(&a, 0) == RT_OK);
    RTri extra = {{ {2,2,0.05}, {50,2,0.05}, {2,50,0.05} }, {1, 2, 3}, 0.25f};
    CK(rt_triangle(&a, &extra) == ref_triangle(&b, &extra));
    CK(memcmp(a.rgb, b.rgb, (size_t)64*64*3) == 0);
    CK(memcmp(a.z, b.z, sizeof(float) * 64 * 64) == 0);
    rt_free(&a); rt_free(&b);
}

/* ---- 2. alpha 1 under blending on is still an overwrite ------------------- */
static void test_blend_opaque_alpha(void) {
    RTarget a, b;
    rt_init(&a, 32, 32); rt_init(&b, 32, 32);
    RTri s[5]; scene(s);
    for (int i = 0; i < 5; ++i) { rt_triangle(&a, &s[i]); ref_triangle(&b, &s[i]); }
    CK(rt_set_blend(&b, 1) == RT_OK);
    RTri over = {{ {0,0,0.1}, {31,0,0.1}, {0,31,0.1} }, {7, 200, 90}, 1.0f};
    RtStats st;
    CK(rt_triangle_at(&b, &over, 0, &st) == ref_triangle(&a, &over));
    CK(st.blended == 0);                 /* opaque: nothing mixed */
    CK(memcmp(a.rgb, b.rgb, (size_t)32*32*3) == 0);
    CK(memcmp(a.z, b.z, sizeof(float) * 32 * 32) == 0);
    rt_free(&a); rt_free(&b);
}

/* ---- 3. blending on, hand-computed mix on a known pixel ------------------ */
static void test_blend_hand_computed(void) {
    RTarget t;
    rt_init(&t, 8, 8);
    CK(rt_set_blend(&t, 1) == RT_OK);
    /* background pixel (2,2) holds a known destination colour and depth */
    size_t i = 2*8 + 2;
    t.rgb[i*3+0] = 100; t.rgb[i*3+1] = 50; t.rgb[i*3+2] = 200;
    t.z[i] = 0.9f;
    /* triangle covers (2.5,2.5) at depth 0.4, src (200,100,0), alpha 0.25 */
    RTri tri = {{ {0,0,0.4}, {7,0,0.4}, {0,7,0.4} }, {200, 100, 0}, 0.25f};
    RtStats st;
    CK(rt_triangle_at(&t, &tri, 0, &st) == 28); /* x+y < 6, half-open centres */
    /* expected: round(src*.25 + dst*.75)
     *   r = 200*.25 + 100*.75 = 50 + 75     = 125
     *   g = 100*.25 +  50*.75 = 25 + 37.5   = 62.5 -> 63
     *   b =   0*.25 + 200*.75 = 0  + 150    = 150 */
    CK(t.rgb[i*3+0] == 125);
    CK(t.rgb[i*3+1] == 63);
    CK(t.rgb[i*3+2] == 150);
    CK(t.z[i] == 0.4f);
    CK(st.written == 28 && st.blended == 28);

    /* over untouched black the mix is round(src * alpha): 50, 25, 0 */
    RTri tri2 = {{ {5,5,0.4}, {7,5,0.4}, {5,7,0.4} }, {200, 100, 0}, 0.25f};
    size_t j = 5*8 + 5;
    CK(t.z[j] > 1e29f);
    CK(rt_triangle_at(&t, &tri2, 1, &st) == 3); /* centres 11,12,12 satisfy x+y<=12 */
    CK(t.rgb[j*3+0] == 50 && t.rgb[j*3+1] == 25 && t.rgb[j*3+2] == 0);
    CK(t.z[j] == 0.4f);
    rt_free(&t);
}

/* ---- 4. depth-rejected fragments are never mixed ------------------------ */
static void test_blend_depth_reject(void) {
    RTarget t;
    rt_init(&t, 8, 8);
    rt_set_blend(&t, 1);
    size_t i = 1*8 + 1;
    t.rgb[i*3+0] = 10; t.rgb[i*3+1] = 20; t.rgb[i*3+2] = 30;
    t.z[i] = 0.1f;                       /* existing surface is nearer */
    RTri tri = {{ {0,0,0.5}, {7,0,0.5}, {0,7,0.5} }, {255, 255, 255}, 0.5f};
    RtStats st;
    /* (1,1) loses the depth test; (1,2) and (2,1) are untouched and pass */
    CK(rt_triangle_at(&t, &tri, 0, &st) == 27);
    CK(st.written == 27 && st.blended == 27);
    CK(t.rgb[i*3+0] == 10 && t.rgb[i*3+1] == 20 && t.rgb[i*3+2] == 30);
    CK(t.z[i] == 0.1f);
    rt_free(&t);
}

/* ---- 5. alpha 0 leaves colour and depth untouched ----------------------- */
static void test_blend_alpha_zero(void) {
    RTarget t;
    rt_init(&t, 8, 8);
    rt_set_blend(&t, 1);
    size_t i = 3*8 + 3;
    t.rgb[i*3+0] = 11; t.rgb[i*3+1] = 22; t.rgb[i*3+2] = 33;
    t.z[i] = 0.7f;
    RTri tri = {{ {0,0,0.2}, {7,0,0.2}, {0,7,0.2} }, {255, 255, 255}, 0.0f};
    RtStats st;
    CK(rt_triangle_at(&t, &tri, 0, &st) == 0);
    CK(st.written == 0 && st.blended == 0);
    CK(t.rgb[i*3+0] == 11 && t.rgb[i*3+1] == 22 && t.rgb[i*3+2] == 33);
    CK(t.z[i] == 0.7f);
    /* alpha == 0 with blending off is still a plain legacy overwrite */
    rt_set_blend(&t, 0);
    CK(rt_triangle_at(&t, &tri, 1, &st) > 0);
    CK(t.rgb[i*3+0] == 255);
    rt_free(&t);
}

/* ---- 6. alpha out of range: error names the triangle -------------------- */
static void test_alpha_errors(void) {
    RTarget t;
    rt_init(&t, 16, 16);
    rt_set_blend(&t, 1);
    RTri good = {{ {0,0,0.5}, {15,0,0.5}, {0,15,0.5} }, {1,2,3}, 0.5f};
    RTri hi   = {{ {0,0,0.5}, {15,0,0.5}, {0,15,0.5} }, {1,2,3}, 1.5f};
    RTri lo   = {{ {0,0,0.5}, {15,0,0.5}, {0,15,0.5} }, {1,2,3}, -0.001f};
    RTri nan_ = {{ {0,0,0.5}, {15,0,0.5}, {0,15,0.5} }, {1,2,3}, (float)NAN};
    RTri s[4] = { good, hi, good, lo };
    RtStats st;
    CK(rt_draw(&t, s, 4, &st) == RT_ERR_ALPHA);
    CK(t.err_code == RT_ERR_ALPHA);
    CK(t.err_tri == 1);                  /* the second triangle */
    CK(rt_triangle_at(&t, &nan_, 7, &st) == RT_ERR_ALPHA);
    CK(t.err_tri == 7);
    CK(rt_triangle_at(&t, &good, 8, &st) >= 0);
    CK(t.err_code == RT_OK && t.err_tri == -1);
    /* errors also fire with blending off: alpha is validated regardless */
    rt_set_blend(&t, 0);
    CK(rt_triangle_at(&t, &lo, 9, &st) == RT_ERR_ALPHA);
    CK(t.err_tri == 9);
    /* a target holding only the failing triangle reports id 0 */
    RTarget u; rt_init(&u, 4, 4);
    CK(rt_triangle(&u, &hi) == RT_ERR_ALPHA);
    CK(u.err_tri == 0 && u.tris == 0);   /* failed submissions do not advance ids */
    rt_free(&u);
    rt_free(&t);
}

/* ---- 7. empty target: error names the triangle ------------------------- */
static void test_empty_target_errors(void) {
    RTri tri = {{ {0,0,0.5}, {15,0,0.5}, {0,15,0.5} }, {1,2,3}, 1.0f};
    RTri s[3] = { tri, tri, tri };
    RtStats st;
    RTarget t;
    memset(&t, 0, sizeof(t));
    CK(rt_triangle(&t, &tri) == RT_ERR_TARGET);
    CK(t.err_tri == 0);
    CK(rt_draw(&t, s, 3, &st) == RT_ERR_TARGET);
    CK(t.err_tri == 0);                  /* first triangle of the batch */
    /* a target whose buffers were released behaves the same */
    RTarget v; rt_init(&v, 8, 8); rt_free(&v);
    CK(rt_triangle(&v, &tri) == RT_ERR_TARGET);
    CK(v.err_tri == 0);
    /* a partially built target: rgb present, depth missing */
    RTarget w; rt_init(&w, 8, 8);
    free(w.z); w.z = NULL;
    CK(rt_triangle(&w, &tri) == RT_ERR_TARGET);
    CK(w.err_tri == 0);
    CK(strcmp(rt_error_string(w.err_code), "target has no colour/depth storage") == 0);
    rt_free(&w);
}

/* ---- 8. legacy behaviours preserved ------------------------------------- */
static void test_legacy(void) {
    RTarget t;
    CK(rt_init(&t, 32, 32) == 0);
    RTri tri = {{ {4,4,0.5}, {28,4,0.5}, {16,28,0.5} }, {255, 0, 0}, 1.0f};
    int n = rt_triangle(&t, &tri);
    CK(n == 288);
    CK(t.z[16*32+16] < 1.0f);
    CK(t.rgb[(16*32+16)*3] == 255);
    RTri flat = {{ {0,0,0}, {8,8,0}, {16,16,0} }, {0,255,0}, 1.0f};
    CK(rt_triangle(&t, &flat) == RT_ERR_DEGENERATE);
    CK(t.err_tri == 1);
    CK(strcmp(rt_error_string(RT_ERR_DEGENERATE), "degenerate triangle (zero area)") == 0);
    rt_free(&t);
}

int main(void) {
    test_off_matches_reference();
    test_blend_opaque_alpha();
    test_blend_hand_computed();
    test_blend_depth_reject();
    test_blend_alpha_zero();
    test_alpha_errors();
    test_empty_target_errors();
    test_legacy();
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
