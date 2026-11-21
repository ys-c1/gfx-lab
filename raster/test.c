#include "raster.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)
#define CKERR(c, e) do { ++checks; \
    if (!(c)) { ++fails; printf("FAIL %d %s expected -%d got %d\n", __LINE__, #c, (int)(e), rc); } \
    else if (info.code != (e)) { ++fails; printf("FAIL %d %s: info.code %s\n", __LINE__, #c, rt_stencil_error_name(info.code)); } \
} while (0)

/* Independent reference: number of pixel centres inside the circle. */
static int ref_covered(double cx, double cy, double r, int w, int h) {
    int n = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            double dx = x + 0.5 - cx, dy = y + 0.5 - cy;
            if (dx * dx + dy * dy <= r * r) ++n;
        }
    return n;
}

/* Independent reference for one comparison function. */
static int ref_func(RTStencilFunc f, int ref, int v) {
    switch (f) {
    case RT_STENCIL_FUNC_NEVER: return 0;
    case RT_STENCIL_FUNC_LESS: return v < ref;
    case RT_STENCIL_FUNC_EQUAL: return v == ref;
    case RT_STENCIL_FUNC_LEQUAL: return v <= ref;
    case RT_STENCIL_FUNC_GREATER: return v > ref;
    case RT_STENCIL_FUNC_NOTEQUAL: return v != ref;
    case RT_STENCIL_FUNC_GEQUAL: return v >= ref;
    case RT_STENCIL_FUNC_ALWAYS: return 1;
    default: return 0;
    }
}

/* Brute-force pass count over the whole buffer. */
static int ref_passes(const unsigned char *vals, int n, RTStencilFunc f,
                      unsigned int ref, unsigned int mask) {
    int c = 0;
    for (int i = 0; i < n; ++i) c += ref_func(f, ref, vals[i] & mask);
    return c;
}

/* Disabled stencil must leave rgb and z byte-identical. */
static void test_disabled_keeps_output(void) {
    RTStencilErrorInfo info;
    RTarget t;
    CK(rt_init(&t, 32, 32) == 0);
    CK(rt_stencil_enabled(&t) == 0);
    RTri tri = {{ {4,4,0.5}, {28,4,0.5}, {16,28,0.5} }, {255, 0, 0}};
    int n = rt_triangle(&t, &tri);
    CK(n > 200);

    unsigned char *rgb_before = (unsigned char*)malloc(sizeof(unsigned char) * 32 * 32 * 3);
    float *z_before = (float*)malloc(sizeof(float) * 32 * 32);
    CK(rgb_before && z_before);
    memcpy(rgb_before, t.rgb, sizeof(unsigned char) * 32 * 32 * 3);
    memcpy(z_before, t.z, sizeof(float) * 32 * 32);

    RTStencilShape s = {16.0, 16.0, 10.0, 0xFF, 0xFF};
    RTStencilStats st;
    int rc = rt_stencil_write(&t, &s, &st, &info);
    CKERR(rc == 0, RT_STENCIL_ERR_NONE);
    CK(st.covered == 0 && st.written == 0);

    RTStencilTest q = {RT_STENCIL_FUNC_ALWAYS, 1, 0xFF};
    rc = rt_stencil_test(&t, &q, &st, &info);
    CKERR(rc == 0, RT_STENCIL_ERR_NONE);
    CK(st.passes == 0);

    RTStencilShade sh = {0.1, {1, 2, 3}};
    rc = rt_stencil_shade(&t, &sh, &st, &info);
    CKERR(rc == 0, RT_STENCIL_ERR_NONE);
    CK(st.shaded == 0);

    unsigned char v = 123;
    rc = rt_stencil_get(&t, 0, 0, &v, &info);
    CKERR(rc == 0, RT_STENCIL_ERR_NONE);
    CK(v == 0);            /* disabled target reports no stencil value */
    v = 123;
    rc = rt_stencil_pass_at(&t, 0, 0, &v, &info);
    CKERR(rc == 0, RT_STENCIL_ERR_NONE);
    CK(v == 0);

    unsigned char bits[128];
    rc = rt_stencil_export_pass_bits(&t, bits, 128, &info);
    CKERR(rc == 0, RT_STENCIL_ERR_NONE);
    int nonzero = 0;
    for (int i = 0; i < 128; ++i) nonzero += bits[i] != 0;
    CK(nonzero == 0);

    CK(memcmp(rgb_before, t.rgb, sizeof(unsigned char) * 32 * 32 * 3) == 0);
    CK(memcmp(z_before, t.z, sizeof(float) * 32 * 32) == 0);

    /* Enabling after the fact must not retroactively alter the output. */
    CK(rt_stencil_enable(&t, 1) == 0);
    CK(rt_stencil_enabled(&t) == 1);
    CK(memcmp(rgb_before, t.rgb, sizeof(unsigned char) * 32 * 32 * 3) == 0);
    CK(memcmp(z_before, t.z, sizeof(float) * 32 * 32) == 0);

    free(rgb_before); free(z_before);
    rt_free(&t);
}

/* Phase 1: written stencil values must match a brute-force circle test. */
static void test_write_matches_bruteforce(int w, int h) {
    RTStencilErrorInfo info;
    RTarget t;
    CK(rt_init(&t, w, h) == 0);
    CK(rt_stencil_enable(&t, 1) == 0);
    RTStencilShape s = {w * 0.37, h * 0.61, w * 0.29, 0xA5, 0xFF};
    RTStencilStats st;
    CK(rt_stencil_write(&t, &s, &st, &info) == 0);
    CK(st.pixels == w * h);
    CK(st.covered == ref_covered(s.cx, s.cy, s.radius, w, h));

    /* Every pixel matches its own analytic predicate; count differences from
     * the raw stencil buffer, checking both directions. */
    int bad = 0, changed = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            double dx = x + 0.5 - s.cx, dy = y + 0.5 - s.cy;
            int inside = dx * dx + dy * dy <= s.radius * s.radius;
            unsigned char want = inside ? s.value : 0;
            unsigned char got = t.stencil[(size_t)y * w + x];
            if (got != want) ++bad;
            changed += got != 0;
        }
    CK(bad == 0);
    CK(changed == st.covered);
    CK(st.written == st.covered);      /* buffer was zeroed, so equal */
    CK(st.min_value == 0 && st.max_value == (int)s.value);
    CK(st.hist[0] == (unsigned int)(w * h - st.covered));
    CK(st.hist[s.value] == (unsigned int)st.covered);

    /* Write mask: only masked bits change. */
    CK(rt_stencil_clear(&t, 0xC3, &info) == 0);
    RTStencilShape s2 = {w * 0.5, h * 0.5, w * 0.4, 0x0C, 0x0F};
    CK(rt_stencil_write(&t, &s2, &st, &info) == 0);
    int mask_bad = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            double dx = x + 0.5 - s2.cx, dy = y + 0.5 - s2.cy;
            int inside = dx * dx + dy * dy <= s2.radius * s2.radius;
            unsigned char want = inside
                ? (unsigned char)((0xC3 & ~s2.write_mask) | (s2.value & s2.write_mask))
                : 0xC3;
            if (t.stencil[(size_t)y * w + x] != want) ++mask_bad;
        }
    CK(mask_bad == 0);
    CK(st.written == st.covered);
    rt_free(&t);
}

/* Phase 2: pass counts, every function, against brute force. */
static void test_test_matches_bruteforce(int w, int h) {
    RTStencilErrorInfo info;
    enum { FUNCS = RT_STENCIL_FUNC_COUNT };
    static const unsigned int refs[] = {0, 1, 0x0F, 0x30, 0xA5, 0xFF};

    RTarget t;
    CK(rt_init(&t, w, h) == 0);
    CK(rt_stencil_enable(&t, 1) == 0);

    /* Seed with a deterministic non-uniform pattern: write two shapes. */
    RTStencilShape a = {w * 0.30, h * 0.30, w * 0.22, 0x0F, 0xFF};
    RTStencilShape b = {w * 0.70, h * 0.62, w * 0.30, 0xF0, 0xFF};
    RTStencilStats st;
    CK(rt_stencil_write(&t, &a, &st, &info) == 0);
    CK(rt_stencil_write(&t, &b, &st, &info) == 0);

    int n = w * h;
    unsigned char *vals = (unsigned char*)malloc((size_t)n);
    CK(vals != NULL);
    CK(rt_stencil_export_values(&t, vals, n, &info) == 0);

    static const unsigned int masks[] = {0x01, 0x0F, 0xF0, 0xFF};
    for (int m = 0; m < 4; ++m) {
        for (int f = 0; f < FUNCS; ++f) {
            for (size_t r = 0; r < sizeof refs / sizeof refs[0]; ++r) {
                unsigned int refv = refs[r] & masks[m];
                RTStencilTest q = {(RTStencilFunc)f, (unsigned char)refv,
                                   (unsigned char)masks[m]};
                int rc = rt_stencil_test(&t, &q, &st, &info);
                CKERR(rc == 0, RT_STENCIL_ERR_NONE);
                int want = ref_passes(vals, n, (RTStencilFunc)f, refv, masks[m]);
                if (st.passes != want) {
                    ++checks; ++fails;
                    printf("FAIL phase2 func %d mask %02X ref %02X: got %d want %d\n",
                           f, masks[m], refv, st.passes, want);
                }
                /* spot-check the recorded bits along a stride too */
                int bit_bad = 0;
                for (int i = 0; i < n; ++i) {
                    unsigned char pass;
                    if (rt_stencil_pass_at(&t, i % w, i / w, &pass, &info) != 0) { bit_bad = 1; break; }
                    if (pass != (unsigned char)ref_func((RTStencilFunc)f, (int)refv, vals[i] & masks[m])) { bit_bad = 1; break; }
                }
                CK(bit_bad == 0);
            }
        }
    }

    /* The mask must actually gate the comparison: with mask 0x0F only the low
     * nibble may matter, so 0x01, 0x11, 0x21 and 0x31 all compare equal to
     * ref 0x01, while the 0x0F / 0xF0 / 0xFF pixels from the shapes above
     * and the untouched zeros do not. */
    unsigned char nib[4] = {0x01, 0x11, 0x21, 0x31};
    for (int i = 0; i < 4; ++i) t.stencil[(size_t)i] = nib[i];
    RTStencilTest q = {RT_STENCIL_FUNC_EQUAL, 0x01, 0x0F};
    CK(rt_stencil_test(&t, &q, &st, &info) == 0);
    CK(st.passes == 4);          /* all four low nibbles are 1 */

    free(vals);
    rt_free(&t);
}

/* Phase 3: only passing pixels are touched; the rest are untouched. */
static void test_shade_matches_pass_set(int w, int h) {
    RTStencilErrorInfo info;
    RTarget t;
    CK(rt_init(&t, w, h) == 0);
    CK(rt_stencil_enable(&t, 1) == 0);

    /* Background: a triangle, so we can verify shaded pixels overwrite it and
     * others do not. */
    double sx = (double)w, sy = (double)h;
    RTri bg = {{ {0,0,0.9}, {sx,0,0.9}, {0,sy,0.9} }, {10, 20, 30}};
    CK(rt_triangle(&t, &bg) >= 0);
    unsigned char *rgb_before = (unsigned char*)malloc((size_t)w * h * 3);
    float *z_before = (float*)malloc(sizeof(float) * (size_t)w * h);
    CK(rgb_before && z_before);
    memcpy(rgb_before, t.rgb, (size_t)w * h * 3);
    memcpy(z_before, t.z, sizeof(float) * (size_t)w * h);

    RTStencilStats st;
    RTStencilShape s = {w * 0.45, h * 0.55, w * 0.31, 0x05, 0xFF};
    CK(rt_stencil_write(&t, &s, &st, &info) == 0);

    RTStencilTest q = {RT_STENCIL_FUNC_EQUAL, 0x05, 0xFF};
    CK(rt_stencil_test(&t, &q, &st, &info) == 0);
    int passes = st.passes;

    RTStencilShade sh = {0.25, {200, 100, 50}};
    CK(rt_stencil_shade(&t, &sh, &st, &info) == 0);
    CK(st.shaded == passes);

    int mism = 0;
    size_t ndiff = 0;
    for (size_t i = 0; i < (size_t)w * h; ++i) {
        int passed = t.pass[i] != 0;
        int w_ = t.rgb[i*3+0] == 200 && t.rgb[i*3+1] == 100 && t.rgb[i*3+2] == 50;
        int z_ = t.z[i] == 0.25f;
        if (passed != (w_ && z_)) ++mism;
        if (!passed && (memcmp(rgb_before + i*3, t.rgb + i*3, 3) != 0 ||
                        z_before[i] != t.z[i])) ++ndiff;
    }
    CK(mism == 0);
    CK(ndiff == 0);

    /* Pass bits round-trip through the packed bitmask. */
    int need = (w * h + 7) / 8;
    unsigned char *bits = (unsigned char*)malloc((size_t)need);
    CK(bits != NULL);
    CK(rt_stencil_export_pass_bits(&t, bits, need, &info) == 0);
    int bitbad = 0;
    for (int i = 0; i < w * h; ++i) {
        int got = (bits[i >> 3] >> (i & 7)) & 1;
        if (got != t.pass[i]) ++bitbad;
    }
    CK(bitbad == 0);

    free(bits); free(rgb_before); free(z_before);
    rt_free(&t);
}

/* The documented three-phase order and illegal state transitions. */
static void test_phase_order(void) {
    RTStencilErrorInfo info;
    RTarget t;
    CK(rt_init(&t, 16, 16) == 0);
    CK(rt_stencil_enable(&t, 1) == 0);

    RTStencilTest q = {RT_STENCIL_FUNC_ALWAYS, 1, 0xFF};
    RTStencilStats st;
    int rc = rt_stencil_test(&t, &q, &st, &info);
    CK(rc == -(int)RT_STENCIL_ERR_STATE && info.code == RT_STENCIL_ERR_STATE);
    CK(strcmp(info.param, "phase") == 0);

    RTStencilShade sh = {0.5, {1, 1, 1}};
    rc = rt_stencil_shade(&t, &sh, &st, &info);
    CK(rc == -(int)RT_STENCIL_ERR_STATE && info.code == RT_STENCIL_ERR_STATE);

    RTStencilShape s = {8, 8, 4, 1, 0xFF};
    CK(rt_stencil_write(&t, &s, &st, &info) == 0);
    /* Re-test after WRITE is legal. */
    CK(rt_stencil_test(&t, &q, &st, &info) == 0);
    CK(st.passes == 16 * 16);
    /* Re-test after TEST is legal too. */
    CK(rt_stencil_test(&t, &q, &st, &info) == 0);
    CK(rt_stencil_shade(&t, &sh, &st, &info) == 0);
    /* Shading twice without a new test is not. */
    rc = rt_stencil_shade(&t, &sh, &st, &info);
    CK(rc == -(int)RT_STENCIL_ERR_STATE);
    /* Writing a new phase resets things. */
    CK(rt_stencil_write(&t, &s, &st, &info) == 0);
    CK(rt_stencil_test(&t, &q, &st, &info) == 0);
    CK(rt_stencil_shade(&t, &sh, &st, &info) == 0);

    /* pass_at before any test is illegal. */
    unsigned char b;
    CK(rt_stencil_clear(&t, 0, &info) == 0);
    rc = rt_stencil_pass_at(&t, 0, 0, &b, &info);
    CK(rc == -(int)RT_STENCIL_ERR_STATE);
    rt_free(&t);
}

/* Illegal values are rejected with a code and the offending argument named. */
static void test_invalid_values(void) {
    RTStencilErrorInfo info;
    RTarget t;
    RTStencilStats st;
    CK(rt_init(&t, 32, 32) == 0);
    CK(rt_stencil_enable(&t, 1) == 0);

    RTStencilShape s = {16, 16, 10, 0xFF, 0xFF};
    CK(rt_stencil_write(&t, &s, &st, &info) == 0);

    RTStencilShape bad = s;
    int rc;

    bad.radius = 0;
    rc = rt_stencil_write(&t, &bad, &st, &info);
    CKERR(rc == -(int)RT_STENCIL_ERR_GEOMETRY, RT_STENCIL_ERR_GEOMETRY);
    CK(strcmp(info.param, "radius") == 0);

    bad = s; bad.radius = -3;
    rc = rt_stencil_write(&t, &bad, &st, &info);
    CK(rc == -(int)RT_STENCIL_ERR_GEOMETRY);
    CK(strcmp(info.param, "radius") == 0);

    bad = s; bad.cx = NAN;
    rc = rt_stencil_write(&t, &bad, &st, &info);
    CK(rc == -(int)RT_STENCIL_ERR_GEOMETRY);
    CK(strcmp(info.param, "shape") == 0);

    bad = s; bad.cy = INFINITY;
    rc = rt_stencil_write(&t, &bad, &st, &info);
    CK(rc == -(int)RT_STENCIL_ERR_GEOMETRY);

    bad = s; bad.write_mask = 0;
    rc = rt_stencil_write(&t, &bad, &st, &info);
    CKERR(rc == -(int)RT_STENCIL_ERR_BAD_MASK, RT_STENCIL_ERR_BAD_MASK);
    CK(strcmp(info.param, "write_mask") == 0);

    bad = s; bad.value = 0x80; bad.write_mask = 0x0F;
    rc = rt_stencil_write(&t, &bad, &st, &info);
    CKERR(rc == -(int)RT_STENCIL_ERR_BAD_VALUE, RT_STENCIL_ERR_BAD_VALUE);
    CK(strcmp(info.param, "value") == 0);
    CK(info.index == 7);          /* the highest offending bit */

    rc = rt_stencil_write(&t, NULL, &st, &info);
    CK(rc == -(int)RT_STENCIL_ERR_NULL);
    CK(strcmp(info.param, "shape") == 0);

    RTStencilTest q = {(RTStencilFunc)-1, 2, 0xFF};
    rc = rt_stencil_test(&t, &q, &st, &info);
    CK(rc == -(int)RT_STENCIL_ERR_BAD_FUNC);
    CK(strcmp(info.param, "func") == 0);

    q.func = (RTStencilFunc)99;
    rc = rt_stencil_test(&t, &q, &st, &info);
    CKERR(rc == -(int)RT_STENCIL_ERR_BAD_FUNC, RT_STENCIL_ERR_BAD_FUNC);
    CK(strcmp(info.param, "func") == 0);

    q = (RTStencilTest){RT_STENCIL_FUNC_EQUAL, 2, 0};
    rc = rt_stencil_test(&t, &q, &st, &info);
    CKERR(rc == -(int)RT_STENCIL_ERR_BAD_MASK, RT_STENCIL_ERR_BAD_MASK);
    CK(strcmp(info.param, "read_mask") == 0);

    q = (RTStencilTest){RT_STENCIL_FUNC_EQUAL, 0x12, 0x0F};
    rc = rt_stencil_test(&t, &q, &st, &info);
    CKERR(rc == -(int)RT_STENCIL_ERR_BAD_VALUE, RT_STENCIL_ERR_BAD_VALUE);
    CK(strcmp(info.param, "ref") == 0);
    CK(info.index == 4);

    rc = rt_stencil_test(&t, NULL, &st, &info);
    CK(rc == -(int)RT_STENCIL_ERR_NULL);
    CK(strcmp(info.param, "test") == 0);

    q = (RTStencilTest){RT_STENCIL_FUNC_ALWAYS, 0, 0xFF};
    CK(rt_stencil_test(&t, &q, &st, &info) == 0);
    RTStencilShade sh = {0.5, {1, 1, 1}};
    sh.z = NAN;
    rc = rt_stencil_shade(&t, &sh, &st, &info);
    CKERR(rc == -(int)RT_STENCIL_ERR_RANGE, RT_STENCIL_ERR_RANGE);
    CK(strcmp(info.param, "z") == 0);
    sh.z = 0.5;
    rc = rt_stencil_shade(&t, NULL, &st, &info);
    CK(rc == -(int)RT_STENCIL_ERR_NULL);
    CK(strcmp(info.param, "shade") == 0);

    /* Coordinate errors name the pixel. */
    unsigned char v;
    rc = rt_stencil_get(&t, -1, 4, &v, &info);
    CKERR(rc == -(int)RT_STENCIL_ERR_INDEX, RT_STENCIL_ERR_INDEX);
    CK(info.x == -1 && info.y == 4);
    rc = rt_stencil_get(&t, 32, 4, &v, &info);
    CK(rc == -(int)RT_STENCIL_ERR_INDEX);
    CK(info.x == 32 && info.y == 4);
    rc = rt_stencil_pass_at(&t, 0, 32, &v, &info);
    CK(rc == -(int)RT_STENCIL_ERR_INDEX);
    CK(info.y == 32);

    /* Bitmask buffer size mismatches and bit range. */
    int need = (32 * 32 + 7) / 8;
    unsigned char buf[128];
    rc = rt_stencil_export_pass_bits(&t, buf, need - 1, &info);
    CKERR(rc == -(int)RT_STENCIL_ERR_RANGE, RT_STENCIL_ERR_RANGE);
    CK(strcmp(info.param, "out_bytes") == 0);
    rc = rt_stencil_export_pass_bits(&t, buf, need, &info);
    CK(rc == 0);
    rc = rt_stencil_export_value_bits(&t, 8, buf, need, &info);
    CKERR(rc == -(int)RT_STENCIL_ERR_RANGE, RT_STENCIL_ERR_RANGE);
    CK(strcmp(info.param, "bit") == 0);
    rc = rt_stencil_export_value_bits(&t, -1, buf, need, &info);
    CK(rc == -(int)RT_STENCIL_ERR_RANGE);
    rc = rt_stencil_export_values(&t, buf, 32 * 32 - 1, &info);
    CKERR(rc == -(int)RT_STENCIL_ERR_RANGE, RT_STENCIL_ERR_RANGE);
    CK(strcmp(info.param, "n") == 0);

    /* A rejected call must not move the phase: the last successful call was
     * the test, so the target is still in TEST. */
    CK(t.phase == RT_STENCIL_PHASE_TEST);

    /* Errors also name the enum in words. */
    CK(strcmp(rt_stencil_error_name(RT_STENCIL_ERR_BAD_FUNC), "bad-function") == 0);
    rt_free(&t);
}

/* A cleared buffer writes every pixel on the next shape. */
static void test_clear_semantics(void) {
    RTStencilErrorInfo info;
    RTarget t;
    RTStencilStats st;
    CK(rt_init(&t, 24, 24) == 0);
    CK(rt_stencil_enable(&t, 1) == 0);

    RTStencilShape s = {12, 12, 4, 0x33, 0xFF};
    CK(rt_stencil_write(&t, &s, &st, &info) == 0);
    CK(st.written == st.covered);

    CK(rt_stencil_clear(&t, 0x33, &info) == 0);
    /* Same shape, same value: nothing changes now. */
    CK(rt_stencil_write(&t, &s, &st, &info) == 0);
    CK(st.written == 0);

    CK(rt_stencil_clear(&t, 0x00, &info) == 0);
    CK(rt_stencil_write(&t, &s, &st, &info) == 0);
    CK(st.written == st.covered);

    /* Disabling resets the phase; enable again starts clean. */
    CK(rt_stencil_enable(&t, 0) == 0);
    CK(t.phase == RT_STENCIL_PHASE_NONE);
    CK(rt_stencil_enable(&t, 1) == 0);
    rt_free(&t);
}

int main(void) {
    /* The disabled path is covered by the pre-existing test body below. */
    RTarget t;
    CK(rt_init(&t, 32, 32) == 0);
    RTri tri = {{ {4,4,0.5}, {28,4,0.5}, {16,28,0.5} }, {255, 0, 0}};
    int n = rt_triangle(&t, &tri);
    CK(n > 200);
    CK(t.z[16*32+16] < 1.0f);
    CK(t.rgb[(16*32+16)*3] == 255);
    RTri flat = {{ {0,0,0}, {8,8,0}, {16,16,0} }, {0,255,0}};
    CK(rt_triangle(&t, &flat) == -2);
    rt_free(&t);

    test_disabled_keeps_output();
    test_write_matches_bruteforce(64, 64);
    test_test_matches_bruteforce(64, 64);
    test_shade_matches_pass_set(64, 64);
    test_phase_order();
    test_invalid_values();
    test_clear_semantics();

    /* The same brute-force comparisons at the larger resolution. */
    test_write_matches_bruteforce(512, 512);
    test_shade_matches_pass_set(512, 512);

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
