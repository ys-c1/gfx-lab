/*
 * Two-stage multi-light benchmark.
 *
 * Compares single-light vs two-light rendering on the same occluded scene:
 *   - peak absolute error of the composite against a hand-computed
 *     superposition (same closed-form falloff, same segment tests, summed
 *     by hand in ascending light index order)
 *   - light passes, composite add operations, and best-of-N wall time
 *
 * Build: make -C shadow bench
 */
#define _POSIX_C_SOURCE 199309L
#include "shadow.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define W 256
#define H 256
#define REPS 200

static const ShSeg scene[] = {
    { 60.0f,  40.0f,  60.0f, 200.0f },
    {150.0f,  70.0f, 220.0f, 130.0f },
    { 90.0f, 190.0f, 190.0f, 210.0f },
};
#define NSEG ((int)(sizeof scene / sizeof scene[0]))

/* Independent reference: re-implements the segment test and falloff from
 * the header's definition, then sums in ascending index order. */
static int ref_blocked(const ShSeg *s, float lx, float ly, float px, float py) {
    float rx = px - lx, ry = py - ly;
    float sx = s->x1 - s->x0, sy = s->y1 - s->y0;
    float den = rx * sy - ry * sx;
    if (fabsf(den) < 1e-9f) return 0;
    float t = ((s->x0 - lx) * sy - (s->y0 - ly) * sx) / den;
    float u = ((s->x0 - lx) * ry - (s->y0 - ly) * rx) / den;
    return t > 0.0f && t < 1.0f && u >= 0.0f && u <= 1.0f;
}

static int ref_shadowed(const ShLight *l, float px, float py) {
    for (int i = 0; i < NSEG; ++i)
        if (ref_blocked(&scene[i], l->x, l->y, px, py)) return 1;
    return 0;
}

/* Same closed form, accumulated in double and using the algebraically equal
 * r/(r+d) form, so the comparison measures float rounding rather than
 * replaying the library's exact instruction sequence. */
static double ref_one_d(const ShLight *l, double px, double py) {
    if (ref_shadowed(l, (float)px, (float)py)) return 0.0;
    double dx = px - l->x, dy = py - l->y;
    double d = sqrt(dx * dx + dy * dy);
    return (double)l->intensity * ((double)l->radius / ((double)l->radius + d));
}

/* out[] = sum_i ref_one(i) / sum_i intensity_i, ascending i. */
static void ref_super(const ShLight *ls, int n, float *out) {
    double den = 0.0;
    for (int i = 0; i < n; ++i) den += ls[i].intensity;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            double num = 0.0;
            for (int i = 0; i < n; ++i) num += ref_one_d(&ls[i], x + 0.5, y + 0.5);
            out[(size_t)y * W + x] = den > 0.0 ? (float)(num / den) : 0.0f;
        }
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

typedef struct { double ms; ShStats st; float peak_err; float peak_ref; float peak_got; } Row;

static Row run(const ShadowMap *m, const ShLight *ls, int n, const float *ref,
               float *out) {
    Row r;
    ShStats st;
    double best = 1e30;
    for (int k = 0; k < REPS; ++k) {
        double t0 = now_ms();
        if (sh_render_lights((ShadowMap*)m, ls, n, scene, NSEG, 1, NULL, &st) != SH_OK)
            { fprintf(stderr, "render failed\n"); exit(1); }
        double dt = now_ms() - t0;
        if (dt < best) best = dt;
    }
    for (size_t i = 0; i < (size_t)W * H; ++i) out[i] = m->buf[i];
    r.ms = best; r.st = st;
    r.peak_err = 0.0f; r.peak_ref = 0.0f; r.peak_got = 0.0f;
    if (ref) {
        for (size_t i = 0; i < (size_t)W * H; ++i) {
            float d = fabsf(out[i] - ref[i]);
            if (d > r.peak_err) r.peak_err = d;
            if (ref[i] > r.peak_ref) { r.peak_ref = ref[i]; r.peak_got = out[i]; }
        }
    }
    return r;
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double*)a, y = *(const double*)b;
    return (x > y) - (x < y);
}

/* Median over a fresh set of runs, for the timing table. */
static double median_ms(const ShadowMap *m, const ShLight *ls, int n) {
    double t[REPS];
    for (int k = 0; k < REPS; ++k) {
        double t0 = now_ms();
        sh_render_lights((ShadowMap*)m, ls, n, scene, NSEG, 1, NULL, NULL);
        t[k] = now_ms() - t0;
    }
    qsort(t, REPS, sizeof t[0], cmp_double);
    return t[REPS / 2];
}

int main(void) {
    ShadowMap m;
    if (sh_init(&m, W, H) != 0) return 1;

    ShLight one[1] = {{ 40.0f, 40.0f, 90.0f, 1.0f }};
    ShLight two[2] = {{ 40.0f, 40.0f, 90.0f, 1.0f },
                      {220.0f, 60.0f, 70.0f, 0.5f }};

    float *ref1 = malloc(sizeof(float) * W * H);
    float *ref2 = malloc(sizeof(float) * W * H);
    float *out1 = malloc(sizeof(float) * W * H);
    float *out2 = malloc(sizeof(float) * W * H);

    ref_super(one, 1, ref1);
    ref_super(two, 2, ref2);

    Row r1 = run(&m, one, 1, ref1, out1);
    Row r2 = run(&m, two, 2, ref2, out2);
    double med1 = median_ms(&m, one, 1);
    double med2 = median_ms(&m, two, 2);

    /* How much the second light actually changes the image. */
    float max_delta = 0.0f;
    double mean_delta = 0.0;
    for (size_t i = 0; i < (size_t)W * H; ++i) {
        float d = fabsf(out2[i] - out1[i]);
        if (d > max_delta) max_delta = d;
        mean_delta += d;
    }
    mean_delta /= (double)W * H;

    printf("scene      : %dx%d, %d segments, %d repetitions\n", W, H, NSEG, REPS);
    printf("lights L0  : (%.1f, %.1f) r=%.1f I=%.2f\n",
           one[0].x, one[0].y, one[0].radius, one[0].intensity);
    printf("lights L1  : (%.1f, %.1f) r=%.1f I=%.2f  (two-light row only)\n",
           two[1].x, two[1].y, two[1].radius, two[1].intensity);
    printf("order      : pass i=0..n-1, composite in same order, "
           "out = sum(radiance_i)/sum(intensity_i)\n\n");

    printf("%-12s %6s %14s %10s %10s %10s %10s\n",
           "case", "passes", "temp pixels", "comp ops", "peak err",
           "peak ref", "peak out");
    printf("%-12s %6d %14zu %10zu %10.3e %10.4f %10.4f\n",
           "single", r1.st.passes, r1.st.temp_pixels, r1.st.blend_ops,
           (double)r1.peak_err, (double)r1.peak_ref, (double)r1.peak_got);
    printf("%-12s %6d %14zu %10zu %10.3e %10.4f %10.4f\n\n",
           "dual", r2.st.passes, r2.st.temp_pixels, r2.st.blend_ops,
           (double)r2.peak_err, (double)r2.peak_ref, (double)r2.peak_got);

    printf("%-12s %14s %14s %10s\n", "case", "median ms", "ratio", "max delta");
    printf("%-12s %14.3f %14s %10s\n", "single", med1, "-", "-");
    printf("%-12s %14.3f %13.2fx %10.4f\n\n", "dual", med2, med2 / med1, (double)max_delta);
    printf("mean |dual - single| = %.4f  (proves L1 is no longer ignored)\n",
           mean_delta);

    free(ref1); free(ref2); free(out1); free(out2);
    sh_free(&m);
    return 0;
}
