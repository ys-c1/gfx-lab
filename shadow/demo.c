#define _POSIX_C_SOURCE 199309L
#include "shadow.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
}

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

static long pgm_data_offset(const unsigned char *b, long n) {
    long i = 0;
    for (int tok = 0; tok < 4; ++tok) {
        while (i < n && (b[i] == ' ' || b[i] == '\t' || b[i] == '\n' || b[i] == '\r')) ++i;
        if (tok == 0) { if (i + 1 >= n || b[i] != 'P' || b[i+1] != '5') return -1; i += 2; }
        else { while (i < n && b[i] >= '0' && b[i] <= '9') ++i; }
    }
    if (i >= n) return -1;
    return i + 1;
}

/* scene geometry scales with map size so both tiers render the same picture */
static void build_scene(int N, ShLight *l, ShSeg *segs) {
    l->x = N * 0.5f; l->y = N * 0.078125f;
    l->radius = N * 0.3125f; l->intensity = 1.0f;
    segs[0] = (ShSeg){N * 0.15625f, N * 0.46875f, N * 0.46875f, N * 0.46875f};
    segs[1] = (ShSeg){N * 0.546875f, N * 0.546875f, N * 0.859375f, N * 0.546875f};
}

static const char *path_for(int N, int tag) {
    static char buf[64];
    /* tag: 0 legacy, 1 disabled, 2 q8, 3 q16 */
    const char *name[] = {"legacy", "off", "q8", "q16"};
    snprintf(buf, sizeof buf, "demo_%d_%s.pgm", N, name[tag]);
    return buf;
}

static double legacy_noise_rms(const ShadowMap *m) {
    /* measure the noise the old fixed 8-bit path actually produces;
       the writer casts (unsigned char)(v*255+0.5), i.e. truncates to integer */
    long n = (long)m->w * m->h;
    double sum_sq = 0.0;
    for (long i = 0; i < n; ++i) {
        float v = m->buf[i];
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        double code = floor((double)v * 255.0 + 0.5);
        if (code > 255.0) code = 255.0;
        double recon = code / 255.0;
        double e = recon - (double)v;
        sum_sq += e * e;
    }
    return sqrt(sum_sq / (double)n);
}

static void run_tier(int N) {
    ShadowMap m;
    if (sh_init(&m, N, N) != 0) { printf("sh_init(%d) failed\n", N); exit(1); }
    ShLight l; ShSeg segs[2];
    build_scene(N, &l, segs);

    double t0 = now_ms();
    if (sh_render(&m, &l, segs, 2) != 0) { printf("render failed\n"); exit(1); }
    double render_ms = now_ms() - t0;

    long pixels = (long)N * N;
    printf("================ map %d x %d = %ld pixels, render %.3f ms ================\n",
           N, N, pixels, render_ms);
    printf("%-28s %6s %12s %14s %14s %s\n",
           "path", "depth", "step", "noise_rms", "time(ms)", "output");

    /* ---- old path: fixed 8-bit output ---- */
    t0 = now_ms();
    int rc = sh_write_pgm(&m, path_for(N, 0));
    double t_legacy = now_ms() - t0;
    if (rc != 0) { printf("legacy write failed\n"); exit(1); }
    double rms_legacy = legacy_noise_rms(&m);
    printf("%-28s %6d %12.6e %14.6e %14.3f %s\n",
           "old path (fixed 8-bit)", 8, 1.0 / 255.0, rms_legacy,
           t_legacy, path_for(N, 0));

    /* ---- new path, switch OFF: must return to the current output ---- */
    ShQuantStats st;
    t0 = now_ms();
    rc = sh_write_pgm_q(&m, path_for(N, 1), &(ShQuantCfg){8, 0}, &st);
    double t_off = now_ms() - t0;
    if (rc != 0) { printf("disabled write failed rc=%d\n", rc); exit(1); }

    unsigned char *a = NULL, *b = NULL; long an = 0, bn = 0;
    if (file_bytes(path_for(N, 0), &a, &an) || file_bytes(path_for(N, 1), &b, &bn)) {
        printf("readback failed\n"); exit(1);
    }
    int identical = (an == bn && memcmp(a, b, (size_t)an) == 0);
    free(a); free(b);
    printf("%-28s %6s %12s %14s %14.3f %s  [vs old: %s]\n",
           "new path, switch OFF", "n/a", "-", "-", t_off,
           path_for(N, 1), identical ? "BYTE-IDENTICAL" : "MISMATCH!!");

    /* ---- new path enabled, two depth tiers listed separately ---- */
    struct { int bits; int tag; const char *label; } tiers[2] = {
        {8, 2, "new path, target  8-bit"},
        {16, 3, "new path, target 16-bit"},
    };
    for (int k = 0; k < 2; ++k) {
        ShQuantCfg cfg = {tiers[k].bits, 1};
        t0 = now_ms();
        rc = sh_write_pgm_q(&m, path_for(N, tiers[k].tag), &cfg, &st);
        double t = now_ms() - t0;
        if (rc != 0) { printf("quant write failed rc=%d\n", rc); exit(1); }
        printf("%-28s %6d %12.6e %14.6e %14.3f %s\n",
               tiers[k].label, st.bits, st.step, st.noise_rms, t,
               path_for(N, tiers[k].tag));
    }

    /* 8-bit enabled must also match the old path's pixel bytes */
    unsigned char *q8 = NULL, *lg = NULL; long qn = 0, ln = 0;
    if (file_bytes(path_for(N, 2), &q8, &qn) || file_bytes(path_for(N, 0), &lg, &ln)) {
        printf("readback failed\n"); exit(1);
    }
    long ho = pgm_data_offset(q8, qn), hl = pgm_data_offset(lg, ln);
    int px_same = ho > 0 && hl > 0 && (qn - ho == ln - hl) &&
                  memcmp(q8 + ho, lg + hl, (size_t)(qn - ho)) == 0;
    free(q8); free(lg);
    printf("8-bit enabled vs old path pixel bytes: %s\n",
           px_same ? "IDENTICAL" : "MISMATCH!!");

    printf("theoretical uniform rms (step/sqrt(3)): 8-bit %.3e  16-bit %.3e\n\n",
           (1.0 / 255.0) / sqrt(3.0), (1.0 / 65535.0) / sqrt(3.0));
    sh_free(&m);
}

static void negative_demo(void) {
    ShadowMap m;
    if (sh_init(&m, 16, 16) != 0) exit(1);
    ShLight l = {8.0f, 2.0f, 5.0f, 1.0f};
    sh_render(&m, &l, NULL, 0);
    unsigned char *out = NULL; long nb = 0; ShQuantStats st;

    struct { ShQuantCfg cfg; const char *which; } cases[] = {
        {{-1, 1}, "cfg.bits (=-1, negative depth)"},
        {{8, -1}, "cfg.enabled (=-1, negative switch)"},
        {{-8, 0}, "cfg.bits (=-8, rejected even while OFF)"},
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        int rc = sh_quantize(&m, &cases[i].cfg, &out, &nb, &st);
        printf("negative param %-42s -> rc=%d (%s)\n", cases[i].which, rc,
               rc == SH_E_BITS_NEG ? "SH_E_BITS_NEG" :
               rc == SH_E_FLAG_NEG ? "SH_E_FLAG_NEG" : "unexpected");
        free(out); out = NULL;
    }
    int rc = sh_quantize(&m, &(ShQuantCfg){17, 1}, &out, &nb, &st);
    printf("out-of-range depth cfg.bits=17 (enabled)            -> rc=%d (SH_E_BITS_RANGE)\n",
           rc);
    sh_free(&m);
    printf("\n");
}

int main(void) {
    negative_demo();
    run_tier(128);
    run_tier(1024);
    return 0;
}
