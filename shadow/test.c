#include "shadow.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

int main(void) {
    ShadowMap m;
    CK(sh_init(&m, 64, 64) == 0);
    ShLight l = {32.0f, 8.0f, 20.0f, 1.0f, 0.0f}; /* src_radius 0: hard only */
    ShSeg s = {10.0f, 20.0f, 54.0f, 20.0f};

    /* --- legacy API still works and returns the old -2 code --- */
    CK(sh_render(&m, &l, &s, 1) == 0);
    CK(m.buf[(size_t)32 * 64 + 32] < 0.01f);
    CK(m.buf[(size_t)2 * 64 + 32] > 0.5f);
    CK(sh_render(&m, &l, NULL, 0) == 0);
    ShLight bad = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    CK(sh_render(&m, &bad, &s, 1) == -2);

    /* --- penumbra disabled: sh_render_ex must be byte-identical to sh_render --- */
    ShadowMap ref;
    CK(sh_init(&ref, 64, 64) == 0);
    CK(sh_render(&ref, &l, &s, 1) == 0);
    ShPenumbra off = {0, 8}; /* enable=0: samples ignored */
    ShError err;
    CK(sh_render_ex(&m, &l, &s, 1, &off, &err) == 0);
    CK(memcmp(m.buf, ref.buf, (size_t)64 * 64 * sizeof(float)) == 0);
    CK(sh_render_ex(&m, &l, &s, 1, NULL, &err) == 0);
    CK(memcmp(m.buf, ref.buf, (size_t)64 * 64 * sizeof(float)) == 0);

    /* --- error: zero source radius must name light.src_radius --- */
    ShPenumbra on8 = {1, 8};
    CK(sh_render_ex(&m, &l, &s, 1, &on8, &err) == SH_ERR_LIGHT);
    CK(strcmp(err.field, "light.src_radius") == 0);
    printf("  err: [%s] %s\n", err.field, err.message);

    /* --- error: zero sample count must name pen.samples --- */
    ShLight sl = {32.0f, 8.0f, 20.0f, 1.0f, 3.0f};
    ShPenumbra zero = {1, 0};
    CK(sh_render_ex(&m, &sl, &s, 1, &zero, &err) == SH_ERR_SAMPLES);
    CK(strcmp(err.field, "pen.samples") == 0);
    printf("  err: [%s] %s\n", err.field, err.message);
    CK(sh_render_ref(&m, &sl, &s, 1, 0, &err) == SH_ERR_SAMPLES);
    CK(strcmp(err.field, "samples") == 0);

    /* --- error: degenerate segment must name segs[i] --- */
    ShSeg deg = {10.0f, 20.0f, 10.0f, 20.0f};
    CK(sh_render_ex(&m, &sl, &deg, 1, &on8, &err) == SH_ERR_SEGMENT);
    CK(strcmp(err.field, "segs[0]") == 0);
    printf("  err: [%s] %s\n", err.field, err.message);
    CK(sh_render(&m, &sl, &deg, 1) == SH_ERR_SEGMENT);
    ShSeg two[2] = {{1, 1, 2, 2}, {5, 5, 5, 5}};
    CK(sh_render_ex(&m, &sl, two, 2, &on8, &err) == SH_ERR_SEGMENT);
    CK(strcmp(err.field, "segs[1]") == 0);

    /* --- legacy radius<=0 keeps code -2 and names light.radius --- */
    CK(sh_render_ex(&m, &bad, &s, 1, &off, &err) == SH_ERR_LIGHT);
    CK(strcmp(err.field, "light.radius") == 0);

    /* --- stage 1 analytic penumbra bandwidth ---
       light (32,8), blocker row y=20 (a=12), receiver y=44 (b=36, t=3):
       W = 2*r*(1-t)/t = 2*3*(1-3)/3 = -4 -> t=3 means receiver behind seg:
       recompute: t=(20-8)/(44-8)=12/36=1/3, W=2*3*(2/3)/(1/3)=12 */
    float w = sh_penumbra_width(&sl, &s, 32.0f, 44.0f);
    CK(fabsf(w - 12.0f) < 1e-4f);
    CK(sh_penumbra_width(&sl, &s, 32.0f, 5.0f) == 0.0f);  /* receiver in front */

    /* --- soft render sanity: graded band, umbra deep, penumbra between --- */
    CK(sh_render_ex(&m, &sl, &s, 1, &on8, &err) == 0);
    CK(sh_render(&ref, &sl, &s, 1) == 0);
    float deepS = m.buf[(size_t)44 * 64 + 32];
    float deepH = ref.buf[(size_t)44 * 64 + 32];
    CK(deepH < 1e-6f);
    CK(deepS < 0.05f);
    int graded = 0;
    for (size_t i = 0; i < (size_t)64 * 64; ++i)
        if (m.buf[i] > 0.02f && m.buf[i] < 0.95f) { graded = 1; break; }
    CK(graded);

    /* adaptive sampling derived from stage-1 bandwidth */
    ShPenumbra ada = {1, SH_SAMPLES_ADAPTIVE};
    CK(sh_render_ex(&m, &sl, &s, 1, &ada, &err) == 0);

    /* reference renderer and convergence toward it */
    CK(sh_render_ref(&ref, &sl, &s, 1, 256, &err) == 0);
    double se = 0.0;
    for (size_t i = 0; i < (size_t)64 * 64; ++i) {
        double d = (double)m.buf[i] - ref.buf[i];
        se += d * d;
    }
    double rmse = sqrt(se / (64.0 * 64.0));
    printf("  adaptive vs per-pixel(256) RMSE = %.5f\n", rmse);
    CK(rmse < 0.12);

    sh_free(&m);
    sh_free(&ref);
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
