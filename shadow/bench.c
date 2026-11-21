#define _POSIX_C_SOURCE 199309L
#include "shadow.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * Benchmark / evidence harness for two-stage multi-light shadows.
 *
 * For each resolution (256 and 1024) it reports:
 *   - the stage-1 order table (rank, input index, intensity), also exported
 *     as CSV with `make bench`;
 *   - the bright-to-dim accumulation order and the exact accumulation count
 *     (pixels x passes), plus the pass count;
 *   - memory: map bytes, order-table bytes, peak extra bytes, and the
 *     per-light temporary maps that the streaming implementation avoids;
 *   - error against an independent per-pixel reference that recomputes every
 *     pixel from the light list in sorted order, sharing no code with the
 *     library;
 *   - the error between the sorted order and the deliberately-wrong input
 *     order, to show the ordering actually matters;
 *   - a permutation sweep: every rotation of the input lights must produce a
 *     bit-identical buffer;
 *   - a byte-identity check of the legacy path against sh_render().
 *
 * Geometry is expressed as fractions of the map so both resolutions use the
 * same scene.
 */

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

#define NLIGHT 5
#define NSEG   4

/* Intensities 0.30, 1.00, 0.70, 0.70 (tie), 0.05.  Input order is shuffled. */
static const float LIGHT_F[5][4] = {
    /* x,    y,    radius, intensity */
    { 0.78f, 0.70f, 0.25f, 0.30f },
    { 0.20f, 0.18f, 0.22f, 1.00f },
    { 0.70f, 0.22f, 0.30f, 0.70f },
    { 0.34f, 0.80f, 0.30f, 0.70f },
    { 0.50f, 0.50f, 0.15f, 0.05f },
};

static const float SEG_F[4][4] = {
    { 0.26f, 0.55f, 0.55f, 0.42f },
    { 0.58f, 0.20f, 0.62f, 0.74f },
    { 0.10f, 0.86f, 0.88f, 0.80f },
    { 0.42f, 0.12f, 0.46f, 0.36f },
};

static void make_scene(int size, ShLight L[NLIGHT], ShSeg S[NSEG]) {
    for (int i = 0; i < NLIGHT; ++i) {
        L[i].x = LIGHT_F[i][0] * (float)size;
        L[i].y = LIGHT_F[i][1] * (float)size;
        L[i].radius = LIGHT_F[i][2] * (float)size;
        L[i].intensity = LIGHT_F[i][3];
    }
    for (int i = 0; i < NSEG; ++i) {
        S[i].x0 = SEG_F[i][0] * (float)size; S[i].y0 = SEG_F[i][1] * (float)size;
        S[i].x1 = SEG_F[i][2] * (float)size; S[i].y1 = SEG_F[i][3] * (float)size;
    }
}

/* Independent per-pixel reference, in the order given by `order`. */
static float ref_pixel(int x, int y, const ShLight *L,
                       const ShSeg *S, int nseg, const int *order, int n) {
    float px = (float)x + 0.5f, py = (float)y + 0.5f, v = 0.0f;
    for (int k = 0; k < n; ++k) {
        const ShLight *l = &L[order[k]];
        int sh = 0;
        for (int i = 0; i < nseg && !sh; ++i) {
            const ShSeg *s = &S[i];
            float rx = px - l->x, ry = py - l->y;
            float sx = s->x1 - s->x0, sy = s->y1 - s->y0;
            float den = rx * sy - ry * sx;
            if (fabsf(den) < 1e-9f) continue;
            float t = ((s->x0 - l->x) * sy - (s->y0 - l->y) * sx) / den;
            float u = ((s->x0 - l->x) * ry - (s->y0 - l->y) * rx) / den;
            if (t > 0.0f && t < 1.0f && u >= 0.0f && u <= 1.0f) sh = 1;
        }
        if (!sh) {
            float dx = px - l->x, dy = py - l->y;
            v += l->intensity / (1.0f + sqrtf(dx * dx + dy * dy) / l->radius);
        }
    }
    return v;
}

static unsigned long long hash_map(const ShadowMap *m) {
    const unsigned char *p = (const unsigned char*)m->buf;
    unsigned long long h = 1469598103934665603ULL;
    size_t n = (size_t)m->w * (size_t)m->h * sizeof(float);
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}

/* Same formula in double precision, from the caller's (float) scene values.
 * Comparing the float map against this isolates the library's float rounding
 * rather than the reference's own. */
static double ref_pixel_d(int x, int y, const ShLight *L,
                          const ShSeg *S, int nseg, const int *order, int n) {
    double px = (double)x + 0.5, py = (double)y + 0.5, v = 0.0;
    for (int k = 0; k < n; ++k) {
        const ShLight *l = &L[order[k]];
        int sh = 0;
        for (int i = 0; i < nseg && !sh; ++i) {
            const ShSeg *s = &S[i];
            double rx = px - (double)l->x, ry = py - (double)l->y;
            double sx = (double)s->x1 - (double)s->x0, sy = (double)s->y1 - (double)s->y0;
            double den = rx * sy - ry * sx;
            if (fabs(den) < 1e-9) continue;
            double t = (((double)s->x0 - (double)l->x) * sy - ((double)s->y0 - (double)l->y) * sx) / den;
            double u = (((double)s->x0 - (double)l->x) * ry - ((double)s->y0 - (double)l->y) * rx) / den;
            if (t > 0.0 && t < 1.0 && u >= 0.0 && u <= 1.0) sh = 1;
        }
        if (!sh) {
            double dx = px - (double)l->x, dy = py - (double)l->y;
            v += (double)l->intensity / (1.0 + sqrt(dx * dx + dy * dy) / (double)l->radius);
        }
    }
    return v;
}

struct err { double max, mean, rmse; };

static struct err compare(const ShadowMap *got, const ShLight *L,
                          const ShSeg *S, const int *order, int n) {
    struct err e = { 0.0, 0.0, 0.0 };
    double se = 0.0;
    for (int y = 0; y < got->h; ++y)
        for (int x = 0; x < got->w; ++x) {
            double want = ref_pixel_d(x, y, L, S, NSEG, order, n);
            double d = fabs((double)got->buf[(size_t)y * got->w + x] - want);
            if (d > e.max) e.max = d;
            e.mean += d;
            se += d * d;
        }
    double np = (double)got->w * (double)got->h;
    e.mean /= np;
    e.rmse = sqrt(se / np);
    return e;
}

/* The old, order-sensitive path: sum in the caller's input order. */
static void render_input_order(ShadowMap *m, const ShLight *L,
                               const ShSeg *S, int n) {
    int io[NLIGHT];
    for (int i = 0; i < n; ++i) io[i] = i;
    for (int y = 0; y < m->h; ++y)
        for (int x = 0; x < m->w; ++x)
            m->buf[(size_t)y * m->w + x] = ref_pixel(x, y, L, S, NSEG, io, n);
}

/* What callers did before this API existed: loop sh_render() over the lights.
 * sh_render() overwrites the map, so only the LAST light survives. */
static void render_legacy_sequence(ShadowMap *m, const ShLight *L, int n) {
    for (int i = 0; i < n; ++i) sh_render(m, &L[i], NULL, 0);
}

static int export_order(const ShLight *L, const ShSeg *S, int size,
                        const char *scene) {
    ShadowMap m;
    if (sh_init(&m, size, size) != 0) return -1;
    ShMultiStats st;
    int rc = sh_render_lights(&m, L, NLIGHT, S, NSEG, 1, &st, NULL);
    sh_free(&m);
    if (rc != SH_OK) return -1;
    char path[64];
    snprintf(path, sizeof path, "bench_order_%d.csv", size);
    if (sh_write_order_table(path, st.order, st.lights, scene, size, size) != SH_OK)
        return -1;
    printf("\nexported %s\n", path);
    return 0;
}

static int run(int size) {
    const int W = size, H = size;
    ShLight L[NLIGHT];
    ShSeg S[NSEG];
    make_scene(size, L, S);

    ShadowMap m;
    if (sh_init(&m, W, H) != 0) return 1;

    ShMultiStats st;
    ShError err;
    double t0 = now_ms();
    if (sh_render_lights(&m, L, NLIGHT, S, NSEG, 1, &st, &err) != SH_OK) {
        printf("render failed: %s\n", err.field);
        return 1;
    }
    double ms = now_ms() - t0;
    unsigned long long h = hash_map(&m);

    /* ---- order table -------------------------------------------------- */
    printf("\n=== %dx%d ===\n", W, H);
    printf("order (stage 1), brightest first:\n");
    printf("  rank  index  intensity      x        y   radius\n");
    for (int i = 0; i < st.lights; ++i) {
        const ShLightOrder *o = &st.order[i];
        printf("  %4d  %5d  %9.4f  %7.1f  %7.1f  %6.1f\n",
               i, o->index, o->intensity, o->x, o->y, o->radius);
    }
    printf("input order intensities:");
    for (int i = 0; i < NLIGHT; ++i) printf(" %.2f", L[i].intensity);
    printf("\n");

    /* ---- passes / accumulation --------------------------------------- */
    printf("passes=%d  pixels=%llu  accumulate_ops=%llu (pixels*passes=%llu)\n",
           st.passes, st.pixels, st.accumulate_ops, st.pixels * (unsigned long long)st.passes);
    printf("sorted=%d  temp_maps=%d\n", st.sorted, st.temp_maps);

    /* ---- memory ------------------------------------------------------- */
    printf("memory: map=%.1f KiB  order=%.0f B  total=%.1f KiB  "
           "peak_extra=%.0f B  avoided_temps=%.1f MiB\n",
           (double)st.map_bytes / 1024.0, (double)st.order_bytes,
           (double)st.total_bytes / 1024.0, (double)st.peak_extra_bytes,
           (double)st.avoided_temp_bytes / (1024.0 * 1024.0));
    printf("time: %.3f ms  (%.1f ns/pixel/pass)\n", ms,
           ms * 1e6 / (double)st.accumulate_ops);

    /* ---- error vs independent per-pixel reference --------------------- */
    {
        int order[NLIGHT];
        for (int i = 0; i < st.lights; ++i) order[i] = st.order[i].index;
        struct err e = compare(&m, L, S, order, NLIGHT);
        printf("error vs per-pixel reference (sorted order, float vs double): "
               "max=%.3e  mean=%.3e  rmse=%.3e\n", e.max, e.mean, e.rmse);
    }

    /* ---- why the order matters: input order diff ----------------------
     * The formula adds the same terms in a different order, so the gap is
     * pure float rounding; it is what made the old output flicker. */
    {
        ShadowMap mi;
        if (sh_init(&mi, W, H) != 0) { sh_free(&m); return 1; }
        render_input_order(&mi, L, S, NLIGHT);
        double max = 0.0, sum = 0.0, sq = 0.0; long diff = 0;
        for (int i = 0; i < W * H; ++i) {
            double d = fabs((double)m.buf[i] - (double)mi.buf[i]);
            if (d > max) max = d;
            sum += d; sq += d * d;
            if (m.buf[i] != mi.buf[i]) ++diff;
        }
        printf("input order vs sorted order: max=%.3e  mean=%.3e  rmse=%.3e  "
               "bits-differing pixels=%ld/%d\n",
               max, sum / (double)(W * H), sqrt(sq / (double)(W * H)), diff, W * H);
        sh_free(&mi);
    }

    /* ---- the symptom: legacy per-light sequence depends on order ------ */
    {
        ShadowMap last;
        if (sh_init(&last, W, H) != 0) { sh_free(&m); return 1; }
        render_legacy_sequence(&last, L, NLIGHT);
        ShadowMap only_last;
        if (sh_init(&only_last, W, H) != 0) { sh_free(&last); sh_free(&m); return 1; }
        sh_render(&only_last, &L[NLIGHT - 1], NULL, 0);
        int equals_last = memcmp(last.buf, only_last.buf,
                                 (size_t)W * H * sizeof(float)) == 0;
        /* reverse the input: a different light ends up on top */
        ShLight rev[NLIGHT];
        for (int i = 0; i < NLIGHT; ++i) rev[i] = L[NLIGHT - 1 - i];
        ShadowMap revoked;
        if (sh_init(&revoked, W, H) != 0) { sh_free(&last); sh_free(&only_last); sh_free(&m); return 1; }
        render_legacy_sequence(&revoked, rev, NLIGHT);
        double gmax = 0.0;
        for (int i = 0; i < W * H; ++i) {
            double d = fabs((double)last.buf[i] - (double)revoked.buf[i]);
            if (d > gmax) gmax = d;
        }
        printf("legacy per-light sequence: equals last light alone=%s, "
               "max delta when input is reversed=%.3f\n",
               equals_last ? "yes" : "NO", gmax);
        sh_free(&last); sh_free(&only_last); sh_free(&revoked);
        if (!equals_last || gmax == 0.0) { sh_free(&m); return 1; }
    }

    /* ---- permutation sweep: rotations must be bit-identical ----------- */
    {
        int bad = 0;
        unsigned long long first = h;
        for (int r = 1; r < NLIGHT; ++r) {
            ShLight rot[NLIGHT];
            for (int i = 0; i < NLIGHT; ++i) rot[i] = L[(i + r) % NLIGHT];
            ShadowMap mr;
            if (sh_init(&mr, W, H) != 0) { bad++; continue; }
            ShMultiStats sr;
            if (sh_render_lights(&mr, rot, NLIGHT, S, NSEG, 1, &sr, &err) != SH_OK ||
                hash_map(&mr) != first) bad++;
            sh_free(&mr);
        }
        printf("rotations bit-identical: %s (%d/%d differ)\n",
               bad ? "NO" : "yes", bad, NLIGHT - 1);
        if (bad) { sh_free(&m); return 1; }
    }

    /* ---- legacy path identity ---------------------------------------- */
    {
        ShadowMap a, b;
        if (sh_init(&a, W, H) != 0 || sh_init(&b, W, H) != 0) { sh_free(&m); return 1; }
        sh_render(&a, &L[0], S, NSEG);
        ShMultiStats s0;
        sh_render_lights(&b, L, NLIGHT, S, NSEG, 0, &s0, &err);
        int same = memcmp(a.buf, b.buf, (size_t)W * H * sizeof(float)) == 0;
        printf("enable_multi=0 == sh_render(lights[0]): %s  "
               "(buffer fnv %016llx)\n", same ? "byte-identical" : "DIFFERS",
               hash_map(&b));
        sh_free(&a); sh_free(&b);
        if (!same) { sh_free(&m); return 1; }
    }

    printf("buffer fnv %016llx\n", h);
    sh_free(&m);
    return 0;
}

int main(void) {
    int rc = 0;
    int fails = 0;
    rc |= run(256);
    rc |= run(1024);

    /* Export the stage-1 order tables used by the report. */
    for (int size = 256; size <= 1024; size *= 4) {
        ShLight L[NLIGHT];
        ShSeg S[NSEG];
        make_scene(size, L, S);
        if (export_order(L, S, size, "bench5") != 0) {
            printf("warning: could not export the %d order table\n", size);
            ++fails;
        }
    }
    if (rc) { printf("\nbench FAILED\n"); return 1; }
    printf("\nbench OK%s\n", fails ? " (with export warnings)" : "");
    return 0;
}
