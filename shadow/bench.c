#define _POSIX_C_SOURCE 199309L
#include "shadow.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Side-by-side runs at 64 and 256 square: storage width, srgb on/off,
 * quantization error, file bytes, wall time. The hand-computed section is
 * the independent reference: the constants below were evaluated with a
 * calculator, not by calling the library. */

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double*)a, y = *(const double*)b;
    return (x > y) - (x < y);
}

static long file_size(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n;
}

static int files_equal(const char *a, const char *b) {
    FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
    if (!fa || !fb) { if (fa) fclose(fa); if (fb) fclose(fb); return 0; }
    int same = 1, ca, cb;
    for (;;) {
        ca = fgetc(fa);
        cb = fgetc(fb);
        if (ca != cb) { same = 0; break; }
        if (ca == EOF) break;
    }
    fclose(fa);
    fclose(fb);
    return same;
}

/* Hand-evaluated reference points. Each row: linear, sRGB encoded,
 * code@8, code@16, which branch of the curve.
 * Worked example, v = 0.5:
 *   e = 1.055 * 0.5^(1/2.4) - 0.055 = 1.055 * 0.7491535 - 0.055 = 0.7353570
 *   8  bit: 0.7353570 * 255   = 187.5160 -> 188
 *   16 bit: 0.7353570 * 65535 = 48192.12 -> 48192
 * v = 0.001 is below the 0.0031308 knee:
 *   e = 12.92 * 0.001 = 0.01292
 *   8  bit: 0.01292 * 255   = 3.2946  -> 3
 *   16 bit: 0.01292 * 65535 = 846.71  -> 847 */
static const struct {
    float linear, encoded;
    unsigned int code8, code16;
    const char *branch;
} HAND[] = {
    {0.001f, 0.0129200f,     3,   847, "12.92*v (knee)"},
    {0.010f, 0.0998528f,    25,  6544, "power"},
    {0.100f, 0.3491902f,    89, 22884, "power"},
    {0.250f, 0.5370987f,   137, 35199, "power"},
    {0.500f, 0.7353570f,   188, 48192, "power"},
    {0.750f, 0.8808250f,   225, 57725, "power"},
    {1.000f, 1.0000000f,   255, 65535, "endpoint"},
};
#define NHAND ((int)(sizeof(HAND) / sizeof(HAND[0])))

static int hand_check(const float *tab8, const float *tab16) {
    int bad = 0;
    printf("hand-computed reference (calculator vs library):\n");
    printf("  %-6s %-9s %-12s %-12s %-13s %-13s %s\n",
           "linear", "encoded", "code8", "want8", "code16", "want16", "branch");
    for (int i = 0; i < NHAND; ++i) {
        unsigned int c8 = 0, c16 = 0;
        sh_quantize_code(HAND[i].linear, 8, 1, SH_SRGB_GAMMA, &c8);
        sh_quantize_code(HAND[i].linear, 16, 1, SH_SRGB_GAMMA, &c16);
        int ok = c8 == HAND[i].code8 && c16 == HAND[i].code16;
        float want_enc = HAND[i].encoded;
        if (fabsf(sh_srgb_encode(HAND[i].linear) - want_enc) > 5e-7f) ok = 0;
        if (!ok) ++bad;
        printf("  %-6.4f %-9.7f %-12u %-12u %-13u %-13u %s%s\n",
               HAND[i].linear, want_enc, c8, HAND[i].code8, c16, HAND[i].code16,
               HAND[i].branch, ok ? "" : "   <-- MISMATCH");
    }
    /* Decode the reference codes back through the published table. */
    printf("  decode check: table8[188]=%.6f (want 0.502887) "
           "table8[128]=%.6f (want 0.215861)\n", tab8[188], tab8[128]);
    if (fabsf(tab8[188] - 0.5028865f) > 1e-5f) ++bad;
    if (fabsf(tab8[128] - 0.2158605f) > 1e-5f) ++bad;
    /* 16 bit code 48192 is 48192/65535 = 0.7353570 encoded, which decodes back
     * to linear 0.5 within half a 16 bit step. (8 bit code 188 carries a
     * different encoded value, 188/255 = 0.7372549 -> 0.5028865, so the two
     * depths must not be compared at the same code index.) */
    printf("  table16[48192]=%.8f (want 0.5 within 1/65535)\n", tab16[48192]);
    if (fabsf(tab16[48192] - 0.5f) > 1.0f / 65535.0f) ++bad;
    return bad;
}

static void render_scene(ShadowMap *m, int n) {
    float k = (float)n / 128.0f;
    ShLight l = {64.0f * k, 10.0f * k, 40.0f * k, 1.0f};
    ShSeg segs[2] = {{20.0f * k, 60.0f * k, 60.0f * k, 60.0f * k},
                     {70.0f * k, 70.0f * k, 110.0f * k, 70.0f * k}};
    if (sh_init(m, n, n) != 0) exit(1);
    if (sh_render(m, &l, segs, 2) != 0) exit(1);
}

typedef struct {
    double max_lin, rms_lin, max_enc;
} Err;

/* max/rms of |table[code] - linear| and max |code/max - encoded|. */
static Err measure(const ShadowMap *m, int bits, int srgb, const float *table) {
    float maxcode = (float)(sh_quant_table_size(bits) - 1);
    size_t count = (size_t)m->w * (size_t)m->h;
    Err e = {0.0, 0.0, 0.0};
    double sl = 0.0;
    for (size_t i = 0; i < count; ++i) {
        unsigned int c = 0;
        sh_quantize_code(m->buf[i], bits, srgb, SH_SRGB_GAMMA, &c);
        /* Both errors use the transfer actually configured, so the srgb=0
         * rows compare like with like (display space is linear then). */
        double dl = fabs((double)table[c] - (double)m->buf[i]);
        double de = fabs((double)c / maxcode -
                         (double)sh_encode(m->buf[i], srgb, SH_SRGB_GAMMA));
        if (dl > e.max_lin) e.max_lin = dl;
        if (de > e.max_enc) e.max_enc = de;
        sl += dl * dl;
    }
    e.rms_lin = sqrt(sl / (double)count);
    return e;
}

static double best_quantize_ms(const ShadowMap *m, int bits, int srgb) {
    double t[15];
    for (int i = 0; i < 15; ++i) {
        void *p = NULL;
        size_t sz = 0;
        double a = now_ms();
        if (sh_quantize(m, bits, srgb, SH_SRGB_GAMMA, &p, &sz) != SH_OK) exit(1);
        t[i] = now_ms() - a;
        free(p);
    }
    qsort(t, 15, sizeof(double), cmp_double);
    return t[0];
}

static double best_write_ms(const ShadowMap *m, int bits, int srgb,
                            const char *path) {
    double t[9];
    for (int i = 0; i < 9; ++i) {
        double a = now_ms();
        if (sh_write_pgm(m, path, bits, srgb, SH_SRGB_GAMMA) != SH_OK) exit(1);
        t[i] = now_ms() - a;
    }
    qsort(t, 9, sizeof(double), cmp_double);
    return t[0];
}

int main(void) {
    static float tab8[256];
    static float tab16[65536];

    double t0 = now_ms();
    if (sh_quant_table(8, 1, SH_SRGB_GAMMA, tab8) != SH_OK) return 1;
    double build8 = now_ms() - t0;
    t0 = now_ms();
    if (sh_quant_table(16, 1, SH_SRGB_GAMMA, tab16) != SH_OK) return 1;
    double build16 = now_ms() - t0;
    printf("table: 256 entries (%zu B) built in %.3f ms, "
           "65536 entries (%zu B) built in %.3f ms\n",
           sizeof tab8, build8, sizeof tab16, build16);

    if (hand_check(tab8, tab16) != 0) {
        printf("hand reference: FAILED\n");
        return 1;
    }
    printf("hand reference: all points match\n\n");

    const int res[2] = {64, 256};
    printf("%4s %5s %4s %9s %13s %12s %12s %9s %11s\n",
           "res", "bits", "srgb", "file B", "max err lin",
           "rms err lin", "max err disp", "quant ms", "q+write ms");

    for (int r = 0; r < 2; ++r) {
        int n = res[r];
        ShadowMap m;
        render_scene(&m, n);
        for (int bitsel = 0; bitsel < 2; ++bitsel) {
            int bits = bitsel == 0 ? 8 : 16;
            const float *table = bitsel == 0 ? tab8 : tab16;
            for (int srgb = 0; srgb <= 1; ++srgb) {
                char path[128];
                snprintf(path, sizeof path, "build/bench_%d_%d_%d.pgm", n, bits, srgb);
                if (sh_write_pgm(&m, path, bits, srgb, SH_SRGB_GAMMA) != SH_OK) return 1;
                Err e = measure(&m, bits, srgb, table);
                double qa = best_quantize_ms(&m, bits, srgb);
                double qw = best_write_ms(&m, bits, srgb, path);
                printf("%4d %5d %4d %9ld %13.8f %12.8f %12.8f %9.4f %11.4f\n",
                       n, bits, srgb, file_size(path), e.max_lin, e.rms_lin,
                       e.max_enc, qa, qw);
            }
            /* Determinism belongs to the output path, not to a run loop:
             * quantize twice under identical inputs and compare the files. */
            {
                char a[128], b[132];
                snprintf(a, sizeof a, "build/bench_%d_%d_off.pgm", n, bits);
                snprintf(b, sizeof b, "build/bench_%d_%d_off2.pgm", n, bits);
                if (sh_write_pgm(&m, a, bits, 0, SH_SRGB_GAMMA) != SH_OK) return 1;
                if (sh_write_pgm(&m, b, bits, 0, SH_SRGB_GAMMA) != SH_OK) return 1;
                printf("%4d %5d   -- repeat write identical: %s\n", n, bits,
                       files_equal(a, b) ? "yes" : "NO");
                remove(b);
            }
        }
        sh_free(&m);
    }

    printf("\nbytes are whole PGM files (header included). "
           "err lin = |table[code] - linear|, err disp = |code/max - transfer(linear)|.\n");

    /* srgb off, 8 bit must reproduce the pre-change linear byte. */
    {
        ShadowMap m;
        render_scene(&m, 64);
        void *p = NULL;
        size_t sz = 0;
        int same = sh_quantize(&m, 8, 0, SH_SRGB_GAMMA, &p, &sz) == SH_OK;
        unsigned char *q = (unsigned char*)p;
        for (size_t i = 0; same && i < sz; ++i) {
            float v = m.buf[i];
            if (v < 0.0f) v = 0.0f;
            if (v > 1.0f) v = 1.0f;
            if (q[i] != (unsigned char)(v * 255.0f + 0.5f)) same = 0;
        }
        free(p);
        sh_free(&m);
        printf("64x64, 8 bit, srgb off vs old linear formula: %s\n",
               same ? "byte-identical" : "DIFFERS");
        if (!same) return 1;
    }
    return 0;
}
