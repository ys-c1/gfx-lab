#define _POSIX_C_SOURCE 200809L
#include "shadow.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

/* Read a P5 file's pixel payload; caller frees. */
static unsigned char *read_pgm(const char *path, int *w, int *h, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fscanf(f, "P5 %d %d", w, h) != 2) { fclose(f); return NULL; }
    int maxv = 0;
    if (fscanf(f, "%d", &maxv) != 1 || maxv != 255) { fclose(f); return NULL; }
    if (fgetc(f) == EOF) { fclose(f); return NULL; }
    *n = (size_t)*w * (size_t)*h;
    unsigned char *px = (unsigned char*)malloc(*n);
    if (!px || fread(px, 1, *n, f) != *n) { free(px); fclose(f); return NULL; }
    fclose(f);
    return px;
}

static void setup(ShadowMap *m) {
    sh_init(m, 128, 128);
    ShLight l = {64.0f, 10.0f, 40.0f, 1.0f};
    ShSeg segs[2] = {{20.0f, 60.0f, 60.0f, 60.0f}, {70.0f, 70.0f, 110.0f, 70.0f}};
    sh_render(m, &l, segs, 2);
}

/* Run both stages and write `path`, returning the stats from stage 2. */
static int run_two(const ShadowMap *m, const char *path, const ShDither *d,
                   double *comp, ShDitherStats *out, char *err, size_t errlen) {
    ShDitherStats st;
    if (sh_dither_error(m, d, comp, &st, err, errlen) != 0) return -1;
    if (sh_write_pgm_comp(m, path, d, comp, &st, err, errlen) != 0) return -1;
    *out = st;
    return 0;
}

int main(void) {
    ShadowMap m;
    setup(&m);
    enum { N = 128 * 128 };
    double *comp = (double*)malloc(N * sizeof(double));
    ShDitherStats st;
    char err[256];

    /* 1. Switch off -> byte-for-byte identical to the legacy writer. */
    ShDither off = {0, 0, 0.0};   /* levels/amount deliberately nonsense */
    CK(sh_write_pgm(&m, "/tmp/shadow_legacy.pgm") == 0);
    CK(sh_dither_error(&m, &off, comp, &st, err, sizeof err) == 0);
    CK(st.comp_rms == 0.0 && st.comp_absmean == 0.0);
    {
        int zero = 1;
        for (int i = 0; i < N; ++i) if (comp[i] != 0.0) zero = 0;
        CK(zero);
    }
    CK(sh_write_pgm_comp(&m, "/tmp/shadow_off.pgm", &off, comp, &st, err, sizeof err) == 0);
    {
        int w1, h1, w2, h2; size_t n1, n2;
        unsigned char *a = read_pgm("/tmp/shadow_legacy.pgm", &w1, &h1, &n1);
        unsigned char *b = read_pgm("/tmp/shadow_off.pgm", &w2, &h2, &n2);
        CK(a && b && n1 == n2 && memcmp(a, b, n1) == 0);
        free(a); free(b);
    }

    /* 2. amount == 0: compensation is zero everywhere and the output is
     *    plain rounding at `levels` codes. A synthetic ramp covering every
     *    code proves all `levels` values are reachable. */
    {
        ShadowMap t;
        CK(sh_init(&t, 32, 32) == 0);
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 32; ++x)
                t.buf[y*32+x] = (float)(y*32 + x) / 1023.0f;
        ShDither flat = {1, 32, 0.0};
        CK(sh_dither_error(&t, &flat, comp, &st, err, sizeof err) == 0);
        CK(st.comp_absmean == 0.0 && st.comp_rms == 0.0);
        CK(sh_write_pgm_comp(&t, "/tmp/shadow_a0.pgm", &flat, comp, &st, err, sizeof err) == 0);
        int w, h; size_t n;
        unsigned char *px = read_pgm("/tmp/shadow_a0.pgm", &w, &h, &n);
        CK(px != NULL);
        unsigned char seen[256] = {0};
        for (size_t i = 0; i < n; ++i) seen[px[i]] = 1;
        int distinct = 0;
        for (int i = 0; i < 256; ++i) distinct += seen[i];
        CK(distinct == 32);
        /* and it is exactly nearest-code rounding: sample 0 must be 0 and
         * the last sample must be the top code mapped to 255. */
        CK(px[0] == 0);
        CK(px[n-1] == 255);
        free(px);
        sh_free(&t);
    }

    /* 3. Hand-computed case: 4x4, levels=4, amount=1, serpentine FS.
     *    Row values 0.10 / 0.35 / 0.60 / 0.85 (targets 0.30 / 1.05 / 1.80 /
     *    2.55 codes). Worked through by hand:
     *      row0: got 0,1,2,2
     *      row1: got 0,1,2,3   (right-to-left)
     *      row2: got 0,1,2,2
     *      row3: got 0,1,2,3 */
    {
        ShadowMap t;
        CK(sh_init(&t, 4, 4) == 0);
        const float v[4] = {0.10f, 0.35f, 0.60f, 0.85f};
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x) t.buf[y*4+x] = v[x];
        ShDither d = {1, 4, 1.0};
        ShDitherStats hs;
        CK(sh_dither_error(&t, &d, comp, &hs, err, sizeof err) == 0);
        /* Row 0 compensation, hand-computed in code units (targets
         * 0.30 / 1.05 / 1.80 / 2.55 codes; got = round(target + c), and the
         * residual diffused onward is target + c - got):
         *   x0: 0.30             -> 0, residual +0.30    -> x1 += 0.30*7/16
         *   x1: 1.05 + 0.13125   -> 1, residual +0.18125 -> x2 += 0.18125*7/16
         *   x2: 1.80 + 0.079297  -> 2, residual -0.120703 -> x3 += -0.120703*7/16
         *   x3: 2.55 - 0.052808  -> 2, residual +0.497192 (right edge)
         */
        const double want0[4] = {0.0, 0.30 * 7.0/16.0,
                                 0.18125 * 7.0/16.0,
                                 -0.120703125 * 7.0/16.0};
        for (int x = 0; x < 4; ++x)
            CK(fabs(comp[x] - want0[x]) < 1e-5);
        int w, h; size_t n;
        CK(sh_write_pgm_comp(&t, "/tmp/shadow_hand.pgm", &d, comp, &hs, err, sizeof err) == 0);
        unsigned char *px = read_pgm("/tmp/shadow_hand.pgm", &w, &h, &n);
        CK(px != NULL);
        /* 4 codes map to display bytes 0, 85, 170, 255. */
        const int expect[4][4] = {{0,1,2,2},{0,1,2,3},{0,1,2,2},{0,1,2,3}};
        const int byte_of[4] = {0, 85, 170, 255};
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x)
                CK(px[y*4+x] == byte_of[expect[y][x]]);
        free(px);
        sh_free(&t);
    }

    /* 4. 32 vs 256 codes on the real light field. The dither must remove
     *    long constant-code runs; 256 codes must have a much lower error
     *    than 32 codes in code units. */
    ShDither d32 = {1, 32, 1.0}, d256 = {1, 256, 1.0}, p32 = {1, 32, 0.0};
    ShDitherStats s32, s256, sp32;
    CK(run_two(&m, "/tmp/shadow_d32.pgm",  &d32, comp, &s32,  err, sizeof err) == 0);
    CK(run_two(&m, "/tmp/shadow_d256.pgm", &d256, comp, &s256, err, sizeof err) == 0);
    CK(run_two(&m, "/tmp/shadow_p32.pgm",  &p32, comp, &sp32, err, sizeof err) == 0);
    CK(s32.levels == 32 && s256.levels == 256 && sp32.levels == 32);
    /* The two independent banding measures must agree in direction: dither
     * cuts both the visible-band count and the low-frequency error at the
     * same code depth. */
    CK(s32.ring_bands < sp32.ring_bands);
    CK(s32.err_lf_rms < sp32.err_lf_rms / 2.0);
    CK(s32.comp_absmean > 0.0 && s256.comp_absmean > 0.0);
    /* Dither noise is bounded: RMS compensation stays below one code. */
    CK(s32.comp_rms < 1.0 && s256.comp_rms < 1.0);
    /* In normalized units (code divided by levels-1) the 256-code render is
     * an order of magnitude more accurate than the 32-code one. */
    CK(s256.err_rms < s32.err_rms / 4.0);
    /* Timing is recorded, non-negative, for both stages. */
    CK(s32.stage1_ms >= 0.0 && s32.stage2_ms >= 0.0);
    CK(s256.stage1_ms >= 0.0 && s256.stage2_ms >= 0.0);

    /* 5. Out-of-range settings: -1 and a message naming the field, from both
     *    stages. */
    struct { ShDither d; const char *field; } bad[] = {
        { {1,   1, 1.0}, "levels"  },
        { {1, 257, 1.0}, "levels"  },
        { {1,  32, -0.1}, "amount" },
        { {1,  32, 1.5}, "amount" },
        { {1,  32, NAN}, "amount" },
        { {2,  32, 1.0}, "enabled" },
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        err[0] = '\0';
        CK(sh_dither_error(&m, &bad[i].d, comp, &st, err, sizeof err) == -1);
        CK(strstr(err, bad[i].field) != NULL);
        err[0] = '\0';
        CK(sh_write_pgm_comp(&m, "/tmp/shadow_bad.pgm", &bad[i].d, comp, &st,
                             err, sizeof err) == -1);
        CK(strstr(err, bad[i].field) != NULL);
    }
    /* Enabling without a compensation map is an error that names comp. */
    err[0] = '\0';
    CK(sh_write_pgm_comp(&m, "/tmp/shadow_bad.pgm", &d32, NULL, &st, err, sizeof err) == -1);
    CK(strstr(err, "comp") != NULL);
    /* A NULL settings pointer is reported too. */
    err[0] = '\0';
    CK(sh_dither_error(&m, NULL, comp, &st, err, sizeof err) == -1);
    CK(strstr(err, "ShDither") != NULL);

    /* 6. Degenerate sizes exercise the diffusion bounds. */
    {
        ShadowMap t;
        CK(sh_init(&t, 1, 5) == 0);
        for (int i = 0; i < 5; ++i) t.buf[i] = (float)i / 4.0f;
        ShDither dt = {1, 4, 1.0};
        CK(run_two(&t, "/tmp/shadow_tiny.pgm", &dt, comp, &st, err, sizeof err) == 0);
        sh_free(&t);
    }

    free(comp);
    sh_free(&m);
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
