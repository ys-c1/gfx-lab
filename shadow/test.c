#include "shadow.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

static int file_bytes(const char *path, unsigned char **buf, long *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = (unsigned char*)malloc((size_t)sz);
    if (!b) { fclose(f); return -1; }
    if (fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); fclose(f); return -1; }
    fclose(f);
    *buf = b; *n = sz;
    return 0;
}

/* skip a P5 header: magic + width + height + maxval, then one whitespace byte */
static long pgm_data_offset(const unsigned char *b, long n) {
    long i = 0;
    for (int tok = 0; tok < 4; ++tok) {
        while (i < n && (b[i] == ' ' || b[i] == '\t' || b[i] == '\n' || b[i] == '\r')) ++i;
        if (tok == 0) { if (i + 1 >= n || b[i] != 'P' || b[i+1] != '5') return -1; i += 2; }
        else { while (i < n && b[i] >= '0' && b[i] <= '9') ++i; }
    }
    if (i >= n) return -1;
    return i + 1; /* single whitespace after maxval */
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

    /* ---- configurable quantization path ---- */
    unsigned char *q = NULL; long qn = 0; ShQuantStats st;

    /* negative params: each offending parameter has its own error code */
    CK(sh_quantize(&m, &(ShQuantCfg){-1, 1}, &q, &qn, &st) == SH_E_BITS_NEG);
    CK(sh_quantize(&m, &(ShQuantCfg){8, -1}, &q, &qn, &st) == SH_E_FLAG_NEG);
    /* negatives are rejected even while the switch is off */
    CK(sh_quantize(&m, &(ShQuantCfg){-1, 0}, &q, &qn, &st) == SH_E_BITS_NEG);
    /* out-of-range depth (non-negative) only matters when enabled */
    CK(sh_quantize(&m, &(ShQuantCfg){17, 1}, &q, &qn, &st) == SH_E_BITS_RANGE);
    CK(sh_quantize(&m, &(ShQuantCfg){0, 1}, &q, &qn, &st) == SH_E_BITS_RANGE);
    CK(sh_quantize(&m, &(ShQuantCfg){17, 0}, &q, &qn, &st) == 0);
    free(q); q = NULL;

    /* stats fields at the two benchmark depths */
    CK(sh_quantize(&m, &(ShQuantCfg){8, 1}, &q, &qn, &st) == 0);
    CK(st.bits == 8 && st.samples == 64 * 64 && qn == 64 * 64);
    /* step = 1/(2^bits-1) */
    CK(st.step > 1.0/255.0 - 1e-12 && st.step < 1.0/255.0 + 1e-12);
    /* uniform mid-tread quantization: rms noise bounded by step/sqrt(3) */
    CK(st.noise_rms >= 0.0 && st.noise_rms <= st.step / 1.7);
    free(q); q = NULL;

    CK(sh_quantize(&m, &(ShQuantCfg){16, 1}, &q, &qn, &st) == 0);
    CK(st.bits == 16 && qn == 64L * 64 * 2);
    CK(st.step > 1.0/65535.0 - 1e-15 && st.step < 1.0/65535.0 + 1e-15);
    CK(st.noise_rms >= 0.0 && st.noise_rms <= st.step / 1.7);
    free(q); q = NULL;

    /* disabled sh_quantize must reproduce the legacy 8-bit bytes exactly */
    CK(sh_quantize(&m, &(ShQuantCfg){8, 0}, &q, &qn, &st) == 0);
    CK(st.bits == 8 && qn == 64L * 64);
    CK(sh_write_pgm(&m, "build/legacy.pgm") == 0);
    unsigned char *legacy = NULL; long ln = 0;
    CK(file_bytes("build/legacy.pgm", &legacy, &ln) == 0);
    long hdr = pgm_data_offset(legacy, ln);
    CK(hdr > 0);
    CK(ln - hdr == qn);
    CK(memcmp(legacy + hdr, q, (size_t)qn) == 0);
    free(q); free(legacy); q = NULL;

    /* sh_write_pgm_q with the switch off must be byte-identical to the old path */
    CK(sh_write_pgm_q(&m, "build/off.pgm", &(ShQuantCfg){8, 0}, &st) == 0);
    CK(sh_write_pgm(&m, "build/legacy2.pgm") == 0);
    unsigned char *a = NULL, *b2 = NULL; long an = 0, bn = 0;
    CK(file_bytes("build/off.pgm", &a, &an) == 0);
    CK(file_bytes("build/legacy2.pgm", &b2, &bn) == 0);
    CK(an == bn && memcmp(a, b2, (size_t)an) == 0);
    free(a); free(b2);

    /* enabled writer headers carry the right maxval; 16-bit is big-endian */
    CK(sh_write_pgm_q(&m, "build/q16.pgm", &(ShQuantCfg){16, 1}, &st) == 0);
    unsigned char *p = NULL; long pn = 0;
    CK(file_bytes("build/q16.pgm", &p, &pn) == 0);
    CK(memcmp(p, "P5\n64 64\n65535\n", 15) == 0);
    long po = pgm_data_offset(p, pn);
    CK(po == 15);
    CK(pn - po == 64L * 64 * 2);
    free(p);
    CK(sh_write_pgm_q(&m, "build/q8.pgm", &(ShQuantCfg){8, 1}, NULL) == 0);
    CK(file_bytes("build/q8.pgm", &p, &pn) == 0);
    CK(memcmp(p, "P5\n64 64\n255\n", 13) == 0);
    po = pgm_data_offset(p, pn);
    CK(po == 13);
    CK(pn - po == 64L * 64);
    free(p);

    sh_free(&m);
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
