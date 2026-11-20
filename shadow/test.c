#include "shadow.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

static uint64_t fnv(const unsigned char *p, size_t n) {
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}

static void quantize(const float *buf, unsigned char *out, int n) {
    for (int i = 0; i < n; ++i) {
        float v = buf[i];
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        out[i] = (unsigned char)(v * 255.0f + 0.5f);
    }
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

    /* Sampling off must reproduce the pre-subsampling image bit for bit.
       Golden hashes were taken from the module before this feature existed. */
    const ShSeg dsegs[2] = {{20.0f, 60.0f, 60.0f, 60.0f}, {70.0f, 70.0f, 110.0f, 70.0f}};
    ShLight dl = {64.0f, 10.0f, 40.0f, 1.0f};
    ShadowMap demo;
    CK(sh_init(&demo, 128, 128) == 0);
    CK(sh_render(&demo, &dl, dsegs, 2) == 0);
    unsigned char q[128 * 128];
    quantize(demo.buf, q, 128 * 128);
    CK(fnv(q, sizeof q) == 0xdd650ebdca20f766ULL);
    sh_free(&demo);

    ShadowMap t64;
    CK(sh_init(&t64, 64, 64) == 0);
    ShLight tl = {32.0f, 8.0f, 20.0f, 1.0f};
    ShSeg ts = {10.0f, 20.0f, 54.0f, 20.0f};
    CK(sh_render(&t64, &tl, &ts, 1) == 0);
    unsigned char q2[64 * 64];
    quantize(t64.buf, q2, sizeof q2);
    CK(fnv(q2, sizeof q2) == 0x83212d02662141b9ULL);
    sh_free(&t64);

    /* Rejections name the offending field. */
    const char *field = NULL;
    ShadowMap z;
    CK(sh_init_ex(&z, 0, 32, &field) == SH_ERR_SIZE);
    CK(field && strcmp(field, "w") == 0);
    CK(sh_init_ex(&z, 32, 0, &field) == SH_ERR_SIZE);
    CK(field && strcmp(field, "h") == 0);
    CK(sh_init_ex(&z, 0, 0, &field) == SH_ERR_SIZE);
    CK(field && strcmp(field, "w") == 0);
    CK(sh_init_ex(NULL, 32, 32, &field) == SH_ERR_ARG);
    CK(field && strcmp(field, "m") == 0);

    ShadowMap r;
    CK(sh_init(&r, 16, 16) == 0);
    ShLight rl = {8.0f, 4.0f, 10.0f, 1.0f};
    ShSeg rs = {2.0f, 8.0f, 14.0f, 8.0f};
    int bad_samples[] = {1, 2, 3, 5, 6, 7, 10, -4};
    for (size_t i = 0; i < sizeof bad_samples / sizeof bad_samples[0]; ++i) {
        field = NULL;
        CK(sh_render_ex(&r, &rl, &rs, 1, bad_samples[i], NULL, NULL, &field) == SH_ERR_SAMPLES);
        CK(field && strcmp(field, "samples") == 0);
    }
    CK(sh_render_ex(&r, &rl, &rs, 1, 4, NULL, NULL, &field) == SH_OK);
    CK(sh_render_ex(&r, &rl, &rs, 1, 8, NULL, NULL, &field) == SH_OK);

    /* 4-spp on a short stub near the light: the four quadrant centers of the
       pixel beside the stub straddle its shadow wedge, so exactly two of the
       four subsamples are blocked and coverage is 0.5. */
    ShLight stl = {8.0f, 4.0f, 100.0f, 1.0f};
    ShSeg stub = {7.5f, 8.0f, 8.5f, 8.0f};
    ShStats st;
    float cov[256];
    CK(sh_render_ex(&r, &stl, &stub, 1, 4, &st, cov, NULL) == SH_OK);
    CK(cov[8 * 16 + 8] == 0.5f);
    CK(cov[9 * 16 + 8] == 0.5f);
    CK(cov[10 * 16 + 8] == 0.0f);
    CK(st.edge_pixels > 0);
    CK(fabs(st.coverage - (double)st.lit / (double)st.total) < 1e-12);
    CK(st.total == 16LL * 16 * 4);
    CK(st.samples == 4);

    /* A slanted edge must produce mixed pixels and reduced stair-stepping. */
    ShadowMap c0, c4, c16;
    CK(sh_init(&c0, 64, 64) == 0);
    CK(sh_init(&c4, 64, 64) == 0);
    CK(sh_init(&c16, 64, 64) == 0);
    ShLight sl = {32.0f, 4.0f, 20.0f, 1.0f};
    ShSeg slant = {6.0f, 6.0f, 58.0f, 50.0f};
    ShStats s0, s4, s16;
    CK(sh_render_ex(&c0, &sl, &slant, 1, SH_SAMPLES_OFF, &s0, NULL, NULL) == SH_OK);
    CK(sh_render_ex(&c4, &sl, &slant, 1, 4, &s4, NULL, NULL) == SH_OK);
    CK(sh_render_ex(&c16, &sl, &slant, 1, 16, &s16, NULL, NULL) == SH_OK);
    CK(s0.edge_pixels == 0);
    CK(s4.edge_pixels > 20);
    CK(s16.edge_pixels >= s4.edge_pixels);
    CK(s0.samples == 1 && s0.total == 64LL * 64);
    CK(s16.samples == 16 && s16.total == 64LL * 64 * 16);

    /* Coverage accumulates over subsamples: a pixel's coverage is exactly n/4
       for n in {0..4} because each subsample owns a quarter of the pixel. */
    float cov4[64 * 64];
    CK(sh_render_ex(&c4, &sl, &slant, 1, 4, NULL, cov4, NULL) == SH_OK);
    for (int i = 0; i < 64 * 64; ++i) {
        float f = cov4[i] * 4.0f;
        CK(fabsf(f - roundf(f)) < 1e-5f);
    }
    /* 16 spp lies on a finer grid, so its coverage is in steps of 1/16. */
    float cov16[64 * 64];
    CK(sh_render_ex(&c16, &sl, &slant, 1, 16, NULL, cov16, NULL) == SH_OK);
    for (int i = 0; i < 64 * 64; ++i) {
        float f = cov16[i] * 16.0f;
        CK(fabsf(f - roundf(f)) < 1e-4f);
    }
    /* Color is the mean of the subsamples: every fully shadowed pixel is
       black, every fully lit pixel is brighter than any shadowed one. */
    for (int i = 0; i < 64 * 64; ++i) {
        if (cov16[i] == 0.0f) CK(c16.buf[i] == 0.0f);
        if (cov16[i] == 1.0f) CK(c16.buf[i] > 0.0f);
    }
    sh_free(&c0); sh_free(&c4); sh_free(&c16);

    /* Rejection leaves the map and stats untouched. */
    ShadowMap keep;
    CK(sh_init(&keep, 8, 8) == 0);
    for (int i = 0; i < 64; ++i) keep.buf[i] = 0.25f;
    ShStats ks;
    memset(&ks, 0x5a, sizeof ks);
    ShStats before = ks;
    field = NULL;
    CK(sh_render_ex(&keep, &rl, &rs, 1, 6, &ks, NULL, &field) == SH_ERR_SAMPLES);
    CK(memcmp(&ks, &before, sizeof ks) == 0);
    for (int i = 0; i < 64; ++i) CK(keep.buf[i] == 0.25f);
    CK(sh_render_ex(&keep, &rl, &rs, 1, 4, &ks, NULL, &field) == SH_OK);
    CK(ks.samples == 4);
    sh_free(&keep);
    sh_free(&r);
    sh_free(&m);

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
