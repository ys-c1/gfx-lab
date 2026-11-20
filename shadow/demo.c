/* Shadow-map demo and coverage analysis.
 *
 *   ./demo                 render demo.pgm exactly as before (sampling off)
 *   ./demo 4 [out.pgm]     render with 4 subsamples per pixel
 *   ./demo measure         compare off / 4 / 16 spp at 64 and 256 against an
 *                          analytic coverage reference and print error, edge
 *                          pixel counts and timings
 */
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
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

/* Analytic per-pixel lit fraction of the demo scene.  Each segment blocks the
 * wedge beyond it; the union of the wedges clipped to the pixel is computed
 * exactly with polygon clipping.  When a wedge is thinner than double
 * precision can resolve (far-field occlusion) that pixel falls back to a
 * 1024-spp midpoint brute force. */
#define MAXV 256
typedef struct { double x[MAXV], y[MAXV]; int n; } Pg;

static double sarea(const Pg *p) {
    double a = 0.0;
    for (int i = 0; i < p->n; ++i) {
        int j = (i + 1) % p->n;
        a += p->x[i] * p->y[j] - p->x[j] * p->y[i];
    }
    return 0.5 * a;
}
static void ccw(Pg *p) {
    if (sarea(p) >= 0) return;
    for (int i = 0; i < p->n / 2; ++i) {
        double tx = p->x[i], ty = p->y[i];
        p->x[i] = p->x[p->n - 1 - i]; p->y[i] = p->y[p->n - 1 - i];
        p->x[p->n - 1 - i] = tx; p->y[p->n - 1 - i] = ty;
    }
}
static void push(Pg *p, double x, double y) {
    if (p->n >= MAXV) return;
    if (p->n && fabs(p->x[p->n - 1] - x) < 1e-12 && fabs(p->y[p->n - 1] - y) < 1e-12) return;
    p->x[p->n] = x; p->y[p->n] = y; ++p->n;
}
static Pg clip_half(const Pg *in, double ax, double ay, double bx, double by) {
    Pg out; out.n = 0;
    if (in->n == 0) return out;
    for (int i = 0; i < in->n; ++i) {
        int j = (i + 1) % in->n;
        double xi = in->x[i], yi = in->y[i], xj = in->x[j], yj = in->y[j];
        double vi = (bx - ax) * (yi - ay) - (by - ay) * (xi - ax);
        double vj = (bx - ax) * (yj - ay) - (by - ay) * (xj - ax);
        if (vi >= 0) push(&out, xi, yi);
        if ((vi >= 0) != (vj >= 0) && vi != vj) {
            double t = vi / (vi - vj);
            push(&out, xi + t * (xj - xi), yi + t * (yj - yi));
        }
    }
    return out;
}
static Pg clip_rect(Pg in, double x0, double y0, double x1, double y1) {
    in = clip_half(&in, x0, y0, x1, y0);
    in = clip_half(&in, x1, y0, x1, y1);
    in = clip_half(&in, x1, y1, x0, y1);
    in = clip_half(&in, x0, y1, x0, y0);
    return in;
}
static Pg isect(Pg a, const Pg *b) {
    Pg bb = *b; ccw(&bb); ccw(&a);
    if (a.n < 3 || bb.n < 3) { a.n = 0; return a; }
    for (int i = 0; i < bb.n && a.n > 0; ++i) {
        int j = (i + 1) % bb.n;
        a = clip_half(&a, bb.x[i], bb.y[i], bb.x[j], bb.y[j]);
    }
    return a;
}
static double area(Pg p) { double a = sarea(&p); return a < 0 ? -a : a; }

static int ray_hit(const ShSeg *s, double lx, double ly, double px, double py) {
    double rx = px - lx, ry = py - ly;
    double sx = (double)s->x1 - s->x0, sy = (double)s->y1 - s->y0;
    double den = rx * sy - ry * sx;
    if (fabs(den) < 1e-300) return 0;
    double t = (((double)s->x0 - lx) * sy - ((double)s->y0 - ly) * sx) / den;
    double u = (((double)s->x0 - lx) * ry - ((double)s->y0 - ly) * rx) / den;
    return t > 0.0 && t < 1.0 && u >= 0.0 && u <= 1.0;
}

static Pg shadow_poly(const ShSeg *s, double lx, double ly, double *angle) {
    double a0 = atan2((double)s->y0 - ly, (double)s->x0 - lx);
    double a1 = atan2((double)s->y1 - ly, (double)s->x1 - lx);
    double d = a1 - a0;
    const double PI = 3.14159265358979323846;
    while (d > PI) d -= 2 * PI;
    while (d < -PI) d += 2 * PI;
    *angle = fabs(d);
    Pg p; p.n = 0;
    const double R = 1e6;
    push(&p, lx, ly);
    for (int i = 0; i <= 64; ++i) {
        double a = a0 + d * (double)i / 64.0;
        push(&p, lx + cos(a) * R, ly + sin(a) * R);
    }
    double nx = -(s->y1 - s->y0), ny = (s->x1 - s->x0);
    if (nx * (lx - s->x0) + ny * (ly - s->y0) > 0) { nx = -nx; ny = -ny; }
    double ex = ny, ey = -nx;
    ccw(&p);
    return clip_half(&p, s->x0 - ex * R, s->y0 - ey * R, s->x0 + ex * R, s->y0 + ey * R);
}

static double brute_cov(const ShSeg *segs, int n, double lx, double ly, int px, int py) {
    const int G = 1024;
    long long lit = 0;
    for (int j = 0; j < G; ++j)
        for (int i = 0; i < G; ++i) {
            double x = px + ((double)i + 0.5) / G, y = py + ((double)j + 0.5) / G;
            int sh = 0;
            for (int s = 0; s < n; ++s) if (ray_hit(&segs[s], lx, ly, x, y)) { sh = 1; break; }
            if (!sh) ++lit;
        }
    return (double)lit / ((double)G * G);
}

static double analytic_cov(const ShSeg *segs, int n, double lx, double ly, int px, int py) {
    Pg bp[8];
    double widest = 0.0;
    if (n > 8) n = 8;
    for (int i = 0; i < n; ++i) {
        double ang;
        bp[i] = shadow_poly(&segs[i], lx, ly, &ang);
        if (ang > widest) widest = ang;
        bp[i] = clip_rect(bp[i], px, py, px + 1, py + 1);
        ccw(&bp[i]);
    }
    if (widest > 0.0 && widest < 1e-6) return brute_cov(segs, n, lx, ly, px, py);
    double u = 0.0;
    for (int i = 0; i < n; ++i) u += area(bp[i]);
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j) u -= area(isect(bp[i], &bp[j]));
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j)
            for (int k = j + 1; k < n; ++k)
                u += area(isect(isect(bp[i], &bp[j]), &bp[k]));
    if (u < 0) u = 0;
    if (u > 1) u = 1;
    return 1.0 - u;
}

static void scene(int res, ShLight *l, ShSeg segs[3]) {
    float k = (float)res / 128.0f;
    *l = (ShLight){64.0f * k, 10.0f * k, 40.0f * k, 1.0f};
    segs[0] = (ShSeg){20 * k, 60 * k, 60 * k, 60 * k};
    segs[1] = (ShSeg){70 * k, 70 * k, 110 * k, 70 * k};
    segs[2] = (ShSeg){30 * k, 30 * k, 90 * k, 95 * k};
}

static int measure(void) {
    int resolutions[2] = {64, 256};
    int counts[3] = {SH_SAMPLES_OFF, 4, 16};
    const char *names[3] = {"off", "4", "16"};
    for (int ri = 0; ri < 2; ++ri) {
        int res = resolutions[ri];
        ShadowMap m;
        if (sh_init(&m, res, res) != 0) return 1;
        ShLight l;
        ShSeg segs[3];
        scene(res, &l, segs);
        size_t np = (size_t)res * res;
        double *ref = (double*)malloc(np * sizeof(double));
        float *cov = (float*)malloc(np * sizeof(float));
        if (!ref || !cov) return 1;
        double refcov = 0.0;
        for (int y = 0; y < res; ++y)
            for (int x = 0; x < res; ++x) {
                ref[(size_t)y * res + x] = analytic_cov(segs, 3, l.x, l.y, x, y);
                refcov += ref[(size_t)y * res + x];
            }
        printf("== %dx%d (analytic reference coverage %.6f) ==\n", res, res, refcov / (double)np);
        printf("%-4s %9s %10s %11s %11s %11s %13s %13s %9s\n",
               "spp", "time ms", "coverage", "mean |dcov|", "rms dcov",
               "max |dcov|", "edge pixels", "boundary px", "mismatch");
        for (int si = 0; si < 3; ++si) {
            ShStats st;
            double best = 1e9;
            for (int rep = 0; rep < 5; ++rep) {
                double t0 = now_ms();
                if (sh_render_ex(&m, &l, segs, 3, counts[si], &st, cov, NULL) != SH_OK) return 1;
                double dt = now_ms() - t0;
                if (dt < best) best = dt;
            }
            double mean = 0.0, rms = 0.0, mx = 0.0;
            long long mism = 0, over8 = 0;
            int wx = 0, wy = 0;
            for (int y = 0; y < res; ++y)
                for (int x = 0; x < res; ++x) {
                    size_t i = (size_t)y * res + x;
                    double d = fabs((double)cov[i] - ref[i]);
                    mean += d; rms += d * d;
                    if (d > 0.125) ++over8;
                    if (d > mx) { mx = d; wx = x; wy = y; }
                    if ((cov[i] >= 0.5f) != (ref[i] >= 0.5)) ++mism;
                }
            printf("%-4s %9.3f %10.6f %11.3e %11.3e %11.3e %13lld %13lld %9lld\n",
                   names[si], best, st.coverage, mean / (double)np,
                   sqrt(rms / (double)np), mx, st.edge_pixels,
                   st.boundary_pixels, mism);
            printf("     worst pixel (%d,%d): |dcov| %.4f;  pixels with |dcov| > 1/8: %lld\n",
                   wx, wy, mx, over8);
        }
        free(ref);
        free(cov);
        sh_free(&m);
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "measure") == 0) return measure();
    int samples = SH_SAMPLES_OFF;
    if (argc >= 2) {
        samples = atoi(argv[1]);
        if (samples != SH_SAMPLES_OFF && (samples <= 0 || samples % 4 != 0)) {
            fprintf(stderr, "samples must be 0 (off) or a positive multiple of 4\n");
            return 2;
        }
    }
    const char *path = argc >= 3 ? argv[2] : "demo.pgm";
    ShadowMap m;
    const char *field = NULL;
    if (sh_init_ex(&m, 128, 128, &field) != 0) return 1;
    ShLight l = {64.0f, 10.0f, 40.0f, 1.0f};
    ShSeg segs[2] = {{20.0f, 60.0f, 60.0f, 60.0f}, {70.0f, 70.0f, 110.0f, 70.0f}};
    ShStats st;
    if (sh_render_ex(&m, &l, segs, 2, samples, &st, NULL, &field) != 0) {
        fprintf(stderr, "render rejected: %s\n", field ? field : "?");
        sh_free(&m);
        return 1;
    }
    float sum = 0.0f;
    for (int i = 0; i < 128 * 128; ++i) sum += m.buf[i];
    printf("mean %.4f samples %d coverage %.6f edge_pixels %lld\n",
           sum / (128.0f * 128.0f), st.samples, st.coverage, st.edge_pixels);
    int rc = sh_write_pgm(&m, path);
    sh_free(&m);
    return rc == 0 ? 0 : 1;
}
