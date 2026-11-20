#include "shadow.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

static int read_file(const char *path, unsigned char **buf, size_t *sz) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    *buf = (unsigned char*)malloc((size_t)n);
    if (!*buf) { fclose(f); return -1; }
    *sz = fread(*buf, 1, (size_t)n, f);
    fclose(f);
    return 0;
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

    /*** P1: transfer then quantize, in that order *************************/
    unsigned int c = 0;
    /* sRGB encode first: linear 0.5 -> 0.7354 -> code 188 (linear-only would
       have given 128, the "gray" output the viewer saw). */
    CK(sh_quantize_code(0.5f, 8, 1, SH_SRGB_GAMMA, &c) == SH_OK && c == 188);
    CK(sh_quantize_code(0.5f, 8, 0, SH_SRGB_GAMMA, &c) == SH_OK && c == 128);
    CK(sh_quantize_code(0.25f, 8, 1, SH_SRGB_GAMMA, &c) == SH_OK && c == 137);
    CK(sh_quantize_code(0.5f, 16, 1, SH_SRGB_GAMMA, &c) == SH_OK && c == 48192);
    CK(sh_quantize_code(1.0f, 8, 1, SH_SRGB_GAMMA, &c) == SH_OK && c == 255);
    /* Out-of-range linear values clamp after the transfer. */
    CK(sh_quantize_code(-3.0f, 8, 1, SH_SRGB_GAMMA, &c) == SH_OK && c == 0);
    CK(sh_quantize_code(7.0f, 8, 1, SH_SRGB_GAMMA, &c) == SH_OK && c == 255);
    CK(sh_quantize_code(NAN, 8, 1, SH_SRGB_GAMMA, &c) == SH_OK && c == 0);

    /* The old no-transfer path is untouched: quantize then compare with the
       legacy formula directly, so the two are the same operation. */
    size_t n8 = 0;
    unsigned char *q8 = NULL;
    CK(sh_quantize_u8(&m, 0, SH_SRGB_GAMMA, &q8, &n8) == SH_OK);
    CK(n8 == 64 * 64);
    int same = 1;
    for (int i = 0; i < 64 * 64; ++i) {
        float v = m.buf[i];
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        unsigned char expect = (unsigned char)(v * 255.0f + 0.5f);
        if (q8[i] != expect) same = 0;
    }
    CK(same);
    free(q8);

    /* sRGB round trip through the table: decode(table[code]) ~= input. */
    float *tab = (float*)malloc(256 * sizeof(float));
    CK(tab != NULL);
    CK(sh_quant_table(8, 1, SH_SRGB_GAMMA, tab) == SH_OK);
    CK(fabsf(tab[188] - 0.5028865f) < 1e-5f);
    CK(fabsf(tab[0] - 0.0f) < 1e-7f && fabsf(tab[255] - 1.0f) < 1e-7f);
    /* Mid-gray: sRGB 128 is linear 0.2159, far from 0.5 -> quantization in
       display space, not linear space. */
    CK(fabsf(tab[128] - 0.2158605f) < 1e-5f);
    CK(sh_quant_table_size(8) == 256 && sh_quant_table_size(16) == 65536);
    CK(sh_quant_table_size(12) == SH_E_BITS);

    /* A gamma of exactly 2.4 uses the sRGB curve, not a bare power. */
    CK(fabsf(sh_srgb_encode(0.5f) - 0.735357f) < 1e-5f);
    CK(fabsf(sh_encode(0.5f, 1, 2.2f) - powf(0.5f, 1.0f / 2.2f)) < 1e-6f);

    /* Order matters: convert first, then quantize. Quantizing the linear
     * value first would land near the dark end on a different code; at
     * 0.00197 linear the two orders give 6 and 13. */
    CK(fabsf(sh_srgb_encode(1.0f / 255.0f) - 0.0498f) < 1e-3f);
    CK(sh_quantize_code(0.00197f, 8, 1, SH_SRGB_GAMMA, &c) == SH_OK && c == 6);
    CK((unsigned)(sh_srgb_encode(1.0f / 255.0f) * 255.0f + 0.5f) == 13);
    /* Every published code survives a round trip through the table. */
    {
        int rt = 1;
        for (int i = 0; i < 256; ++i) {
            unsigned rc = 0;
            if (sh_quantize_code(tab[i], 8, 1, SH_SRGB_GAMMA, &rc) != SH_OK ||
                rc != (unsigned)i) rt = 0;
        }
        CK(rt);
    }
    free(tab);

    /*** P2: validation, with the offending field named ********************/
    void *p = NULL;
    size_t sz = 0;
    CK(sh_quantize(&m, 10, 1, SH_SRGB_GAMMA, &p, &sz) == SH_E_BITS);
    CK(p == NULL);
    CK(strcmp(sh_field_name(SH_E_BITS), "bits") == 0);
    CK(sh_quantize(&m, 0, 1, SH_SRGB_GAMMA, &p, &sz) == SH_E_BITS);
    CK(sh_quantize(&m, 32, 1, SH_SRGB_GAMMA, &p, &sz) == SH_E_BITS);
    CK(sh_quantize(&m, 8, 1, 0.0f, &p, &sz) == SH_E_GAMMA);
    CK(sh_quantize(&m, 8, 1, -1.0f, &p, &sz) == SH_E_GAMMA);
    CK(sh_quantize(&m, 8, 1, INFINITY, &p, &sz) == SH_E_GAMMA);
    CK(sh_quantize(&m, 8, 1, NAN, &p, &sz) == SH_E_GAMMA);
    CK(strcmp(sh_field_name(SH_E_GAMMA), "gamma") == 0);
    CK(sh_field_name(SH_E_IO) == NULL);
    /* gamma is checked even when the transfer is disabled. */
    CK(sh_quantize(&m, 8, 0, -2.0f, &p, &sz) == SH_E_GAMMA);
    CK(sh_write_pgm(&m, "build/x.pgm", 12, 1, SH_SRGB_GAMMA) == SH_E_BITS);
    CK(sh_write_pgm(&m, "build/x.pgm", 8, 1, 0.0f) == SH_E_GAMMA);
    CK(sh_strerror(SH_E_BITS) != NULL && sh_strerror(SH_E_GAMMA) != NULL);
    /* bits is reported before gamma, so a request with both broken names bits. */
    CK(sh_quantize(&m, 12, 1, -1.0f, &p, &sz) == SH_E_BITS);
    /* And nothing was written by any rejected call. */
    CK(sh_write_pgm(&m, "build/x.pgm", 12, 1, SH_SRGB_GAMMA) == SH_E_BITS);
    {
        FILE *f = fopen("build/x.pgm", "rb");
        CK(f == NULL); /* earlier write never happened */
        if (f) fclose(f);
    }

    /*** P3: 8 vs 16 bit, file sizes and error *****************************/
    unsigned short *q16 = NULL;
    size_t n16 = 0;
    CK(sh_quantize_u16(&m, 1, SH_SRGB_GAMMA, &q16, &n16) == SH_OK);
    CK(n16 == 2 * 64 * 64);
    /* srgb 0 leaves linear 0 at 0 in both depths; 16 bit must reproduce each
       8 bit sample to within one 8 bit step after decode. */
    unsigned int c16 = 0;
    sh_quantize_code(0.5f, 16, 1, SH_SRGB_GAMMA, &c16);
    CK(c16 == 48192);
    int step_ok = 1;
    for (int i = 0; i < 64 * 64; ++i) {
        unsigned int c8 = 0;
        sh_quantize_code(m.buf[i], 8, 1, SH_SRGB_GAMMA, &c8);
        double d = sh_srgb_decode((float)(q16[i] / 65535.0)) -
                   sh_srgb_decode((float)(c8 / 255.0));
        if (fabs(d) > 1.0 / 255.0) step_ok = 0;
    }
    CK(step_ok);
    free(q16);

    CK(sh_write_pgm(&m, "build/a8.pgm", 8, 1, SH_SRGB_GAMMA) == SH_OK);
    CK(sh_write_pgm(&m, "build/a16.pgm", 16, 1, SH_SRGB_GAMMA) == SH_OK);
    unsigned char *f8 = NULL, *f16 = NULL;
    size_t s8 = 0, s16 = 0;
    CK(read_file("build/a8.pgm", &f8, &s8) == 0);
    CK(read_file("build/a16.pgm", &f16, &s16) == 0);
    CK(memcmp(f8, "P5\n64 64\n255\n", 13) == 0);
    CK(memcmp(f16, "P5\n64 64\n65535\n", 15) == 0);
    CK(s8 == 13 + 64 * 64);
    CK(s16 == 15 + 2 * 64 * 64);
    free(f8);
    free(f16);

    /*** P4: srgb off is byte-identical to the legacy writer **************/
    /* build/legacy.pgm is produced by the pre-change sh_write_pgm formula
       written out by hand, so the comparison does not lean on the code under
       test. The committed demo.pgm was generated the same way; srgb off must
       reproduce it byte for byte (checked by running the demo). */
    {
        FILE *f = fopen("build/legacy.pgm", "wb");
        CK(f != NULL);
        fprintf(f, "P5\n64 64\n255\n");
        for (int i = 0; i < 64 * 64; ++i) {
            float v = m.buf[i];
            if (v < 0.0f) v = 0.0f;
            if (v > 1.0f) v = 1.0f;
            unsigned char b = (unsigned char)(v * 255.0f + 0.5f);
            fwrite(&b, 1, 1, f);
        }
        fclose(f);
    }
    CK(sh_write_pgm(&m, "build/new_off.pgm", 8, 0, SH_SRGB_GAMMA) == SH_OK);
    unsigned char *a = NULL, *b = NULL;
    size_t sa = 0, sb = 0;
    CK(read_file("build/legacy.pgm", &a, &sa) == 0);
    CK(read_file("build/new_off.pgm", &b, &sb) == 0);
    CK(sa == sb && memcmp(a, b, sa) == 0);

    /* srgb on must differ: the whole point of the change. */
    CK(sh_write_pgm(&m, "build/new_on.pgm", 8, 1, SH_SRGB_GAMMA) == SH_OK);
    unsigned char *on = NULL;
    size_t son = 0;
    CK(read_file("build/new_on.pgm", &on, &son) == 0);
    CK(son == sa);
    size_t diff_bytes = 0;
    for (size_t i = 0; i < son && i < sb; ++i) if (on[i] != b[i]) ++diff_bytes;
    printf("srgb on vs off: %zu of %zu pixel bytes differ\n", diff_bytes, son - 13);
    CK(diff_bytes > son / 4);

    /* Error, measured in the display (encoded) domain because that is where
       the eye and the viewer work. Linear 8 bit wastes codes in a dark tail;
       sRGB 8 bit puts them where the ramp actually is. Also report the same
       pair in the linear domain to show the tradeoff is deliberate. */
    {
        double lin_disp = 0, srgb_disp = 0, lin_lin = 0, srgb_lin = 0;
        for (int i = 0; i < 64 * 64; ++i) {
            float v = m.buf[i];
            unsigned int cl = 0, cs = 0;
            sh_quantize_code(v, 8, 0, SH_SRGB_GAMMA, &cl);
            sh_quantize_code(v, 8, 1, SH_SRGB_GAMMA, &cs);
            double e = sh_srgb_encode(v);
            double dl = fabs(cl / 255.0 - e), ds = fabs(cs / 255.0 - e);
            if (dl > lin_disp) lin_disp = dl;
            if (ds > srgb_disp) srgb_disp = ds;
            double vl = fabs(cl / 255.0 - v), vs = fabs(sh_srgb_decode((float)(cs / 255.0)) - v);
            if (vl > lin_lin) lin_lin = vl;
            if (vs > srgb_lin) srgb_lin = vs;
        }
        printf("8-bit max |error| display domain: linear store %.6f, srgb store %.6f\n",
               lin_disp, srgb_disp);
        printf("8-bit max |error| linear domain : linear store %.6f, srgb store %.6f\n",
               lin_lin, srgb_lin);
        CK(srgb_disp < lin_disp / 10.0);
        CK(srgb_disp <= 1.0 / 255.0);
    }
    free(on);
    free(a);
    free(b);

    sh_free(&m);
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
