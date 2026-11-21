#include "shadow.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

/* hand-built scene: 64x64 map, light directly above column 32, one horizontal
   blocker at y=20 spanning x in [10,54]. */
#define W 64
#define H 64
static const ShLight L = {32.5f, 8.0f, 20.0f, 1.0f};
static const ShSeg SEG = {10.0f, 20.0f, 54.0f, 20.0f};

int main(void) {
    ShadowMap m, ref;
    CK(sh_init(&m, W, H) == 0);
    CK(sh_init(&ref, W, H) == 0);

    /* ---- hard-edge baseline (original behaviour) ---- */
    CK(sh_render(&m, &L, &SEG, 1) == 0);
    CK(m.buf[(size_t)32 * W + 32] < 0.01f);
    CK(m.buf[(size_t)2 * W + 32] > 0.5f);
    CK(sh_render(&m, &L, NULL, 0) == 0);
    ShLight bad = {0.0f, 0.0f, 0.0f, 1.0f};
    CK(sh_render(&m, &bad, &SEG, 1) == SHADOW_ELIGHT);

    /* ---- no filter: NULL pcf and (1,1) must reproduce the hard image bit-for-bit ---- */
    CK(sh_render(&ref, &L, &SEG, 1) == 0);
    CK(sh_render_pcf(&m, &L, &SEG, 1, NULL) == 0);
    CK(memcmp(m.buf, ref.buf, (size_t)W * H * sizeof(float)) == 0);
    ShPCF one = {1, 1};
    CK(sh_render_pcf(&m, &L, &SEG, 1, &one) == 0);
    CK(memcmp(m.buf, ref.buf, (size_t)W * H * sizeof(float)) == 0);

    /* ---- validation: field names must appear in error strings ---- */
    ShPCF zero = {0, 5};
    CK(sh_render_pcf(&m, &L, &SEG, 1, &zero) == SHADOW_ESAMPLES);
    CK(strstr(sh_errstr(SHADOW_ESAMPLES), "pcf.samples") != NULL);
    ShPCF neg = {-2, 5};
    CK(sh_render_pcf(&m, &L, &SEG, 1, &neg) == SHADOW_ESAMPLES);
    ShPCF even = {3, 4};
    CK(sh_render_pcf(&m, &L, &SEG, 1, &even) == SHADOW_EKERNEL);
    CK(strstr(sh_errstr(SHADOW_EKERNEL), "pcf.kernel") != NULL);
    ShPCF k0 = {3, 0};
    CK(sh_render_pcf(&m, &L, &SEG, 1, &k0) == SHADOW_EKERNEL);
    ShPCF kneg = {3, -3};
    CK(sh_render_pcf(&m, &L, &SEG, 1, &kneg) == SHADOW_EKERNEL);
    ShKernel kb;
    CK(sh_kernel_build(&kb, 3, 4) == SHADOW_EKERNEL);
    CK(sh_kernel_build(&kb, 0, 5) == SHADOW_ESAMPLES);

    /* ==================================================================
       hand calculation, S=3 samples/axis, K=5 pixel kernel
       offsets o(i) = (i-1)*5/3 = -1.666667, 0, +1.666667
       column x=32 has centre px = 32.5 = light.x, so dx = 0 and the
       falloff only depends on y. a probe is blocked iff py >= 20.

       row  sample py's                      lit/9  coverage
       17   15.833, 17.500, 19.167           9/9    1
       18   16.833, 18.500, 20.167           6/9    2/3
       19   17.833, 19.500, 21.167           6/9    2/3
       20   18.833, 20.500, 22.167           3/9    1/3
       21   19.833, 21.500, 23.167           3/9    1/3
       22   20.833, 22.500, 24.167           0/9    0
       v = coverage * 1/(1 + (cy-8)/20)
       ================================================================== */
    ShPCF pcf = {3, 5};
    CK(sh_render_pcf(&m, &L, &SEG, 1, &pcf) == 0);

    static const float cov[6] = {1.0f, 2.0f / 3.0f, 2.0f / 3.0f,
                                 1.0f / 3.0f, 1.0f / 3.0f, 0.0f};
    printf("per-pixel hand check (x=32, S=3 K=5):\n");
    printf("  row   hand_value     rendered       abs_err\n");
    float maxerr = 0.0f;
    for (int r = 0; r < 6; ++r) {
        int y = 17 + r;
        float cy = (float)y + 0.5f;
        float fall = 1.0f / (1.0f + (cy - 8.0f) / 20.0f);
        float hand = cov[r] * fall;
        float got = m.buf[(size_t)y * W + 32];
        float e = fabsf(got - hand);
        if (e > maxerr) maxerr = e;
        printf("  %3d  %12.6f  %12.6f  %.2e\n", y, hand, got, e);
        CK(e < 1e-5f);
    }
    CK(maxerr < 1e-5f);

    /* penumbra band: rows 18..21 are partial, rows 17 fully lit, 22 fully dark */
    CK(m.buf[(size_t)17 * W + 32] > 0.6f);
    CK(m.buf[(size_t)22 * W + 32] == 0.0f);
    CK(m.buf[(size_t)18 * W + 32] > m.buf[(size_t)20 * W + 32]);
    CK(m.buf[(size_t)21 * W + 32] > 0.0f);
    /* soft width = K-1 = 4 px: exactly four partial rows */
    int partial = 0;
    for (int y = 0; y < H; ++y) {
        float v = m.buf[(size_t)y * W + 32];
        float rv = ref.buf[(size_t)y * W + 32];
        if (v > 0.0f && (rv == 0.0f || v < rv)) ++partial;
    }
    CK(partial == 4);

    /* ---- kernel table build + export ---- */
    ShKernel k;
    CK(sh_kernel_build(&k, 3, 5) == 0);
    CK(k.samples == 3 && k.kernel == 5 && k.soft_width == 4.0f);
    CK(fabsf(k.ox[0] + 5.0f / 3.0f) < 1e-6f);
    CK(k.ox[1] == 0.0f && k.ox[2] == 5.0f / 3.0f);
    CK(sh_kernel_write(&k, "build/kernel_s3_k5.txt") == 0);
    FILE *f = fopen("build/kernel_s3_k5.txt", "r");
    CK(f != NULL);
    if (f) {
        char line[256]; int got_header = 0, got_off = 0;
        while (fgets(line, sizeof line, f)) {
            if (strstr(line, "samples=3")) got_header = 1;
            if (strstr(line, "soft_width_px=4.0")) got_header |= 2;
            if (strstr(line, "-1.666667 -1.666667")) got_off = 1;
        }
        fclose(f);
        CK(got_header == 3 && got_off);
    }
    sh_kernel_free(&k);

    sh_free(&m);
    sh_free(&ref);
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
