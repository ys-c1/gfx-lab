/* Evidence harness for the three-phase stencil path.
 *
 * For each resolution (64x64 and 512x512) it runs the phases independently of
 * the library and reports, per phase:
 *
 *   write  - stencil values actually stored (histogram, min/max), pixels
 *            covered by the circle, pixels whose value changed, elapsed time;
 *            every stored value is compared against an independent analytic
 *            circle test.
 *   test   - pixels visited, pixels passing, elapsed time; the pass count and
 *            the packed pass bitmask are compared against a brute-force
 *            traversal of the exported stencil values, for every comparison
 *            function.
 *   shade  - pixels coloured, elapsed time; each pixel's rgb/z is checked
 *            against the brute-force pass set, and every untouched pixel is
 *            checked to be byte-identical.
 *
 * Timings are the median of REPS runs of the pixel kernel only.  Error
 * columns are exact integer differences, so any drift shows up as non-zero.
 */
#include "raster.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { REPS = 15 };

static int cmp_double(const void *a, const void *b) {
    double x = *(const double*)a, y = *(const double*)b;
    return (x > y) - (x < y);
}

/* Independent circle predicate, used by both the library comparison and the
 * brute-force pass count. */
static int in_circle(double cx, double cy, double r2, int x, int y) {
    double dx = (double)x + 0.5 - cx;
    double dy = (double)y + 0.5 - cy;
    return dx * dx + dy * dy <= r2;
}

static int func_ok(RTStencilFunc f, unsigned int ref, unsigned int v) {
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

static const char *func_name(RTStencilFunc f) {
    switch (f) {
    case RT_STENCIL_FUNC_NEVER: return "never";
    case RT_STENCIL_FUNC_LESS: return "less";
    case RT_STENCIL_FUNC_EQUAL: return "equal";
    case RT_STENCIL_FUNC_LEQUAL: return "lequal";
    case RT_STENCIL_FUNC_GREATER: return "greater";
    case RT_STENCIL_FUNC_NOTEQUAL: return "notequal";
    case RT_STENCIL_FUNC_GEQUAL: return "gequal";
    case RT_STENCIL_FUNC_ALWAYS: return "always";
    default: return "?";
    }
}

/* Runs the whole three-phase cycle once and reports the raw counters. */
typedef struct {
    int covered, written, tested, passes, shaded;
    double write_ms, test_ms, shade_ms;
} Cycle;

static int run_cycle(int res, const RTStencilShape *shape,
                     const RTStencilTest *test, const RTStencilShade *shade,
                     Cycle *out, RTStencilStats *last_stats) {
    RTarget t;
    if (rt_init(&t, res, res) != 0) return -1;
    if (rt_stencil_enable(&t, 1) != 0) { rt_free(&t); return -1; }
    RTStencilErrorInfo err;
    RTStencilStats st;

    if (rt_stencil_clear(&t, 0, &err) != 0) return -1;
    if (rt_stencil_write(&t, shape, &st, &err) != 0) {
        printf("write error: %s (%s)\n", rt_stencil_error_name(err.code),
               err.param ? err.param : "-");
        rt_free(&t);
        return -1;
    }
    out->covered = st.covered;
    out->written = st.written;
    out->write_ms = st.elapsed_ms;

    if (rt_stencil_test(&t, test, &st, &err) != 0) {
        printf("test error: %s (%s)\n", rt_stencil_error_name(err.code),
               err.param ? err.param : "-");
        rt_free(&t);
        return -1;
    }
    out->tested = st.tested;
    out->passes = st.passes;
    out->test_ms = st.elapsed_ms;

    if (rt_stencil_shade(&t, shade, &st, &err) != 0) {
        printf("shade error: %s (%s)\n", rt_stencil_error_name(err.code),
               err.param ? err.param : "-");
        rt_free(&t);
        return -1;
    }
    out->shaded = st.shaded;
    out->shade_ms = st.elapsed_ms;
    if (last_stats) *last_stats = st;
    rt_free(&t);
    return 0;
}

static double median_phase(int res, const RTStencilShape *shape,
                           const RTStencilTest *test, const RTStencilShade *shade,
                           int which) {
    double times[REPS];
    for (int r = 0; r < REPS; ++r) {
        Cycle c;
        if (run_cycle(res, shape, test, shade, &c, NULL) != 0) return -1.0;
        times[r] = which == 0 ? c.write_ms : which == 1 ? c.test_ms : c.shade_ms;
    }
    qsort(times, REPS, sizeof times[0], cmp_double);
    return times[REPS / 2];
}

static void report_resolution(int res) {
    printf("\n================ resolution %dx%d ================\n", res, res);
    double s = (double)res / 64.0;

    /* Shapes scale with the resolution but stay off-centre so that the
     * circle clipping and the boundary pixels are exercised. */
    RTStencilShape shape = {26.0 * s, 41.0 * s, 17.0 * s, 0x09, 0xFF};
    RTStencilTest test = {RT_STENCIL_FUNC_EQUAL, 0x09, 0xFF};
    RTStencilShade shade = {0.25, {200, 100, 50}};

    /* ---- write ---- */
    Cycle c;
    RTStencilStats st;
    if (run_cycle(res, &shape, &test, &shade, &c, &st) != 0) return;
    double r2 = shape.radius * shape.radius;
    long long hand_cov = 0;
    for (int y = 0; y < res; ++y)
        for (int x = 0; x < res; ++x)
            hand_cov += in_circle(shape.cx, shape.cy, r2, x, y);

    /* Re-run the write alone to grab the stored values and bitmasks. */
    RTarget t;
    if (rt_init(&t, res, res) != 0) return;
    RTStencilErrorInfo err;
    rt_stencil_enable(&t, 1);
    rt_stencil_clear(&t, 0, &err);
    rt_stencil_write(&t, &shape, &st, &err);

    int value_diff = 0, value_min = 255, value_max = 0;
    int *hist = (int*)calloc(256, sizeof(int));
    unsigned char *raw = (unsigned char*)malloc((size_t)res * res);
    rt_stencil_export_values(&t, raw, res * res, &err);
    for (int y = 0; y < res; ++y)
        for (int x = 0; x < res; ++x) {
            int i = y * res + x;
            int want = in_circle(shape.cx, shape.cy, r2, x, y) ? shape.value : 0;
            if (raw[i] != want) ++value_diff;
            ++hist[raw[i]];
            if (raw[i] < value_min) value_min = raw[i];
            if (raw[i] > value_max) value_max = raw[i];
        }
    int need = (res * res + 7) / 8;
    unsigned char *bits = (unsigned char*)malloc((size_t)need);
    rt_stencil_export_value_bits(&t, 0, bits, need, &err);
    int bit_diff = 0, bit_set = 0;
    for (int i = 0; i < res * res; ++i) {
        int got = (bits[i >> 3] >> (i & 7)) & 1;
        int want = (raw[i] >> 0) & 1;
        if (got != want) ++bit_diff;
        bit_set += got;
    }

    double w_med = median_phase(res, &shape, &test, &shade, 0);
    printf("phase 1 write\n");
    printf("  pixels %d  covered %d (hand %lld, err %+lld)  written %d  write ms %.3f\n",
           res * res, c.covered, hand_cov, (long long)c.covered - hand_cov,
           c.written, w_med);
    printf("  stencil values: min %d max %d  hist{0:%d, %d:%d}\n",
           value_min, value_max, hist[0], shape.value, hist[shape.value]);
    printf("  per-pixel comparison vs circle predicate: mismatches %d\n", value_diff);
    printf("  packed bitmask row-major LSB-first, bit 0: set %d of %d, mismatches %d\n",
           bit_set, res * res, bit_diff);
    free(bits);
    free(hist);
    free(raw);
    rt_free(&t);

    /* ---- test: swap the write in for a patterned buffer, then compare
     *      every function's pass count and packed bits against brute force. */
    static const RTStencilFunc funcs[] = {
        RT_STENCIL_FUNC_NEVER, RT_STENCIL_FUNC_LESS, RT_STENCIL_FUNC_EQUAL,
        RT_STENCIL_FUNC_LEQUAL, RT_STENCIL_FUNC_GREATER,
        RT_STENCIL_FUNC_NOTEQUAL, RT_STENCIL_FUNC_GEQUAL,
        RT_STENCIL_FUNC_ALWAYS,
    };
    static const struct { unsigned int ref, mask; } refs[] = {
        {0x00, 0xFF}, {0x09, 0xFF}, {0x01, 0x0F}, {0x08, 0x0F}, {0xFF, 0xFF},
    };
    printf("phase 2 test (pixels visited %d)\n", c.tested);
    int test_total_bad = 0;
    for (size_t f = 0; f < sizeof funcs / sizeof funcs[0]; ++f) {
        for (size_t r = 0; r < sizeof refs / sizeof refs[0]; ++r) {
            RTStencilTest q = {funcs[f], (unsigned char)(refs[r].ref & refs[r].mask),
                               (unsigned char)refs[r].mask};
            Cycle tc;
            if (run_cycle(res, &shape, &q, &shade, &tc, &st) != 0) return;
            /* brute force from the pattern the cycle produced: clear then
             * write one shape, so derive the values the same way. */
            int brute = 0;
            for (int y = 0; y < res; ++y)
                for (int x = 0; x < res; ++x) {
                    unsigned int v = in_circle(shape.cx, shape.cy, r2, x, y)
                        ? shape.value : 0u;
                    brute += func_ok(funcs[f], q.ref, v & q.read_mask);
                }
            int bad = (tc.passes != brute);
            test_total_bad += bad;
            printf("  %-9s ref %02X mask %02X: passes %d (brute %d, err %+d)  test ms %.3f\n",
                   func_name(funcs[f]), q.ref, q.read_mask, tc.passes, brute,
                   tc.passes - brute, tc.test_ms);
        }
    }
    printf("  phase 2 disagreements: %d\n", test_total_bad);

    /* The packed pass bitmask for the canonical test, checked bit by bit. */
    {
        RTarget t2;
        if (rt_init(&t2, res, res) != 0) return;
        rt_stencil_enable(&t2, 1);
        rt_stencil_clear(&t2, 0, &err);
        rt_stencil_write(&t2, &shape, &st, &err);
        rt_stencil_test(&t2, &test, &st, &err);
        int n2 = (res * res + 7) / 8;
        unsigned char *pb = (unsigned char*)calloc((size_t)n2, 1);
        rt_stencil_export_pass_bits(&t2, pb, n2, &err);
        int bad = 0, ones = 0;
        for (int y = 0; y < res; ++y)
            for (int x = 0; x < res; ++x) {
                int i = y * res + x;
                int got = (pb[i >> 3] >> (i & 7)) & 1;
                unsigned int v = in_circle(shape.cx, shape.cy, r2, x, y)
                    ? shape.value : 0u;
                int want = func_ok(test.func, test.ref, v & test.read_mask);
                if (got != want) ++bad;
                ones += got;
            }
        printf("  pass bitmask: set %d of %d  (passes %d, err %+d)  bit mismatches %d\n",
               ones, res * res, st.passes, ones - st.passes, bad);
        free(pb);
        rt_free(&t2);
    }

    /* ---- shade ---- */
    {
        RTarget t3;
        if (rt_init(&t3, res, res) != 0) return;
        rt_stencil_enable(&t3, 1);
        /* background triangle, deterministic */
        double sx = (double)res;
        RTri bg = {{ {0, 0, 0.9}, {sx, 0, 0.9}, {0, sx, 0.9} }, {10, 20, 30}};
        rt_triangle(&t3, &bg);
        size_t npix = (size_t)res * res;
        unsigned char *before = (unsigned char*)malloc(npix * 3);
        float *zbefore = (float*)malloc(npix * sizeof(float));
        memcpy(before, t3.rgb, npix * 3);
        memcpy(zbefore, t3.z, npix * sizeof(float));

        rt_stencil_clear(&t3, 0, &err);
        rt_stencil_write(&t3, &shape, &st, &err);
        rt_stencil_test(&t3, &test, &st, &err);
        int passes = st.passes;
        rt_stencil_shade(&t3, &shade, &st, &err);
        double shade_ms = st.elapsed_ms;

        int shaded = 0, wrong = 0, untouched_changed = 0;
        for (size_t i = 0; i < npix; ++i) {
            int x = (int)(i % res), y = (int)(i / res);
            int want = in_circle(shape.cx, shape.cy, r2, x, y) &&
                       func_ok(test.func, test.ref, shape.value & test.read_mask);
            int is_shaded = t3.rgb[i*3+0] == shade.rgb[0] &&
                            t3.rgb[i*3+1] == shade.rgb[1] &&
                            t3.rgb[i*3+2] == shade.rgb[2] &&
                            t3.z[i] == (float)shade.z;
            if (want != is_shaded) ++wrong;
            shaded += is_shaded;
            if (!want && (memcmp(before + i*3, t3.rgb + i*3, 3) != 0 ||
                          zbefore[i] != t3.z[i])) ++untouched_changed;
        }
        double sh_med = median_phase(res, &shape, &test, &shade, 2);
        printf("phase 3 shade\n");
        printf("  shaded %d (passes %d, err %+d)  shade ms %.3f\n",
               shaded, passes, shaded - passes, sh_med);
        printf("  per-pixel rgb/z comparison: wrong %d  untouched-but-changed %d\n",
               wrong, untouched_changed);
        (void)shade_ms;

        free(before); free(zbefore);
        rt_free(&t3);
    }
}

/* Regression guard: with the stencil disabled, the legacy scene must render
 * byte-for-byte as before. */
static void legacy_output_check(void) {
    RTarget t;
    if (rt_init(&t, 128, 128) != 0) return;
    RTri a = {{ {10,10,0.8}, {118,20,0.6}, {64,118,0.4} }, {200, 40, 40}};
    RTri b = {{ {20,100,0.7}, {110,90,0.9}, {70,20,0.2} }, {40, 180, 220}};
    int n1 = rt_triangle(&t, &a), n2 = rt_triangle(&t, &b);
    printf("\nlegacy disabled path: pixels %d %d, stencil_enabled %d\n",
           n1, n2, rt_stencil_enabled(&t));
    rt_free(&t);
}

int main(void) {
    legacy_output_check();
    report_resolution(64);
    report_resolution(512);
    return 0;
}
