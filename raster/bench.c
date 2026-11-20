/* Blend benchmark and correctness harness for the two-pass rasteriser.
 *
 * Renders one normalised scene at 32x32 and 256x256 and reports, for each:
 *   - blended pixels reported by the rasteriser (count and share of frame)
 *   - colour error of every pixel against an independent per-pixel reference
 *     that applies the same alpha formula by hand
 *   - render time (best of several runs)
 * Then it compares the two resolutions and prints a per-pixel hand
 * calculation for sample pixels in both images.
 */
#include "raster.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define REPEATS 15

/* ---- scene, in normalised [0,1] coordinates ----------------------------- */
/* opaque quad, z = 0.8 */
static const double QUAD[4][2] = {{0.125,0.125},{0.875,0.125},{0.875,0.875},{0.125,0.875}};
static const unsigned char QUAD_RGB[3] = {120, 60, 200};
/* translucent triangle 1, z = 0.4, alpha 0.35 */
static const double TRI1[3][2] = {{0.05,0.10},{0.95,0.25},{0.40,0.95}};
static const unsigned char TRI1_RGB[3] = {240, 200, 40};
static const float TRI1_A = 0.35f;
/* translucent triangle 2, z = 0.35, alpha 0.6 */
static const double TRI2[3][2] = {{0.10,0.50},{0.90,0.45},{0.50,0.90}};
static const unsigned char TRI2_RGB[3] = {30, 210, 120};
static const float TRI2_A = 0.6f;

static RTri mk(const double v[3][2], int N, const unsigned char rgb[3], double z, float a) {
    RTri t;
    for (int i = 0; i < 3; ++i) {
        t.v[i].x = v[i][0] * (double)N;
        t.v[i].y = v[i][1] * (double)N;
        t.v[i].z = z;
    }
    t.rgb[0] = rgb[0]; t.rgb[1] = rgb[1]; t.rgb[2] = rgb[2];
    t.alpha = a;
    return t;
}

/* ---- independent per-pixel reference ------------------------------------ */
/* Pixel-centre test using the same edge orientation as the rasteriser:
 * inside when all three edge functions are >= 0.  The vertices are converted
 * to pixel space here, independently of mk(). */
static int inside(const double v[3][2], double px, double py, int N) {
    double ax = v[0][0] * N, ay = v[0][1] * N;
    double bx = v[1][0] * N, by = v[1][1] * N;
    double cx = v[2][0] * N, cy = v[2][1] * N;
    double e0 = (cx - bx) * (py - by) - (cy - by) * (px - bx);
    double e1 = (ax - cx) * (py - cy) - (ay - cy) * (px - cx);
    double e2 = (bx - ax) * (py - ay) - (by - ay) * (px - ax);
    return e0 >= 0.0 && e1 >= 0.0 && e2 >= 0.0;
}

static int in_quad(double px, double py, int N) {
    const double t0[3][2] = {{QUAD[0][0],QUAD[0][1]},{QUAD[1][0],QUAD[1][1]},{QUAD[2][0],QUAD[2][1]}};
    const double t1[3][2] = {{QUAD[0][0],QUAD[0][1]},{QUAD[2][0],QUAD[2][1]},{QUAD[3][0],QUAD[3][1]}};
    return inside(t0, px, py, N) || inside(t1, px, py, N);
}

static unsigned char mix1(unsigned char src, unsigned char dst, double a) {
    double v = (double)src * a + (double)dst * (1.0 - a);
    if (v < 0.0) v = 0.0;
    if (v > 255.0) v = 255.0;
    return (unsigned char)(v + 0.5);
}

/* Reference colour for the pixel whose centre is (px,py) in pixel space. */
static void expected_px(double px, double py, int N, unsigned char out[3]) {
    unsigned char dst[3] = {0, 0, 0};
    if (in_quad(px, py, N)) { dst[0] = QUAD_RGB[0]; dst[1] = QUAD_RGB[1]; dst[2] = QUAD_RGB[2]; }
    if (inside(TRI1, px, py, N)) {
        for (int c = 0; c < 3; ++c) dst[c] = mix1(TRI1_RGB[c], dst[c], TRI1_A);
    }
    if (inside(TRI2, px, py, N)) {
        for (int c = 0; c < 3; ++c) dst[c] = mix1(TRI2_RGB[c], dst[c], TRI2_A);
    }
    out[0] = dst[0]; out[1] = dst[1]; out[2] = dst[2];
}

/* ---- rendering ----------------------------------------------------------- */
static void draw(int N, unsigned char **rgb_out, int *blended_out, double *ms_out) {
    const double qa[3][2] = {{QUAD[0][0],QUAD[0][1]},{QUAD[1][0],QUAD[1][1]},{QUAD[2][0],QUAD[2][1]}};
    const double qb[3][2] = {{QUAD[0][0],QUAD[0][1]},{QUAD[2][0],QUAD[2][1]},{QUAD[3][0],QUAD[3][1]}};
    RTri q[2] = { mk(qa, N, QUAD_RGB, 0.8, 1.0f), mk(qb, N, QUAD_RGB, 0.8, 1.0f) };
    RTri t1 = mk(TRI1, N, TRI1_RGB, 0.4, TRI1_A);
    RTri t2 = mk(TRI2, N, TRI2_RGB, 0.35, TRI2_A);

    double best = 0.0;
    int blended = 0;
    unsigned char *copy = NULL;
    for (int rep = 0; rep < REPEATS; ++rep) {
        RTarget t;
        if (rt_init(&t, N, N) != 0) { *rgb_out = NULL; *blended_out = 0; *ms_out = 0.0; return; }
        struct timespec a, b;
        timespec_get(&a, TIME_UTC);
        rt_triangle(&t, &q[0]);
        rt_triangle(&t, &q[1]);
        rt_set_blend(&t, 1);
        RtStats s1, s2;
        rt_triangle_at(&t, &t1, 2, &s1);
        rt_triangle_at(&t, &t2, 3, &s2);
        timespec_get(&b, TIME_UTC);
        double ms = (double)(b.tv_sec - a.tv_sec) * 1e3
                  + (double)(b.tv_nsec - a.tv_nsec) * 1e-6;
        if (rep == 0 || ms < best) {
            best = ms;
            blended = s1.blended + s2.blended;
            free(copy);
            copy = (unsigned char*)malloc((size_t)N * N * 3);
            memcpy(copy, t.rgb, (size_t)N * N * 3);
        }
        rt_free(&t);
    }
    *rgb_out = copy;
    *blended_out = blended;
    *ms_out = best;
}

/* ---- error against the reference ---------------------------------------- */
typedef struct { long differ; double mean, max; } Err;

static Err measure(int N, const unsigned char *rgb) {
    Err e = {0, 0.0, 0.0};
    double total = 0.0;
    for (int y = 0; y < N; ++y) {
        for (int x = 0; x < N; ++x) {
            unsigned char want[3];
            expected_px(x + 0.5, y + 0.5, N, want);
            int px_diff = 0;
            for (int c = 0; c < 3; ++c) {
                double d = fabs((double)rgb[((size_t)y * N + x) * 3 + c] - (double)want[c]);
                total += d;
                if (d > e.max) e.max = d;
                if (d > 0.0) px_diff = 1;
            }
            if (px_diff) ++e.differ;
        }
    }
    e.mean = total / (3.0 * (double)N * (double)N);
    return e;
}

static void hand_calc(int N, const unsigned char *rgb, double u, double v, const char *label) {
    int x = (int)(u * N), y = (int)(v * N);
    double px = x + 0.5, py = y + 0.5;
    unsigned char dst[3] = {0, 0, 0}, want[3], prev[3];
    if (in_quad(px, py, N)) { dst[0] = QUAD_RGB[0]; dst[1] = QUAD_RGB[1]; dst[2] = QUAD_RGB[2]; }
    prev[0] = dst[0]; prev[1] = dst[1]; prev[2] = dst[2];
    expected_px(px, py, N, want);
    const unsigned char *got = &rgb[((size_t)y * N + x) * 3];
    printf("  %-10s (%3d,%3d) centre u=%.4f v=%.4f | dst=(%3u,%3u,%3u) %s%s%s\n",
           label, x, y, u, v, dst[0], dst[1], dst[2],
           in_quad(px, py, N) ? "[quad]" : "",
           inside(TRI1, px, py, N) ? "[tri1]" : "",
           inside(TRI2, px, py, N) ? "[tri2]" : "");
    if (inside(TRI1, px, py, N)) {
        printf("     tri1: round(%.2f*(%3u,%3u,%3u) + %.2f*(%3u,%3u,%3u)) = (%3u,%3u,%3u)\n",
               (double)TRI1_A, TRI1_RGB[0], TRI1_RGB[1], TRI1_RGB[2],
               (double)(1.0f - TRI1_A), prev[0], prev[1], prev[2],
               mix1(TRI1_RGB[0], prev[0], TRI1_A), mix1(TRI1_RGB[1], prev[1], TRI1_A),
               mix1(TRI1_RGB[2], prev[2], TRI1_A));
        for (int c = 0; c < 3; ++c) prev[c] = mix1(TRI1_RGB[c], prev[c], TRI1_A);
    }
    if (inside(TRI2, px, py, N)) {
        printf("     tri2: round(%.2f*(%3u,%3u,%3u) + %.2f*(%3u,%3u,%3u)) = (%3u,%3u,%3u)\n",
               (double)TRI2_A, TRI2_RGB[0], TRI2_RGB[1], TRI2_RGB[2],
               (double)(1.0f - TRI2_A), prev[0], prev[1], prev[2],
               mix1(TRI2_RGB[0], prev[0], TRI2_A), mix1(TRI2_RGB[1], prev[1], TRI2_A),
               mix1(TRI2_RGB[2], prev[2], TRI2_A));
    }
    printf("     hand = (%3u,%3u,%3u)   actual = (%3u,%3u,%3u)   %s\n",
           want[0], want[1], want[2], got[0], got[1], got[2],
           (want[0]==got[0] && want[1]==got[1] && want[2]==got[2]) ? "match" : "MISMATCH");
}

int main(void) {
    unsigned char *img32 = NULL, *img256 = NULL;
    int blended32 = 0, blended256 = 0;
    double ms32 = 0.0, ms256 = 0.0;

    printf("scene: opaque quad z=0.8 rgb(%u,%u,%u); tri1 z=0.4 a=%.2f rgb(%u,%u,%u); "
           "tri2 z=0.35 a=%.2f rgb(%u,%u,%u)\n\n",
           QUAD_RGB[0], QUAD_RGB[1], QUAD_RGB[2],
           (double)TRI1_A, TRI1_RGB[0], TRI1_RGB[1], TRI1_RGB[2],
           (double)TRI2_A, TRI2_RGB[0], TRI2_RGB[1], TRI2_RGB[2]);

    draw(32, &img32, &blended32, &ms32);
    draw(256, &img256, &blended256, &ms256);
    if (!img32 || !img256) return 1;

    Err e32 = measure(32, img32);
    Err e256 = measure(256, img256);

    printf("%-5s %-9s %-14s %-10s %-8s %-8s %-14s %-9s\n",
           "res", "blended", "blended/total", "meanErr", "maxErr", "mismatchPx", "mismatchShare", "best ms");
    printf("%-5d %-9d %-14.4f %-10.4f %-8.3f %-8ld %-14.6f %-9.3f\n",
           32, blended32, blended32 / (32.0 * 32.0), e32.mean, e32.max, e32.differ,
           (double)e32.differ / (32.0 * 32.0), ms32);
    printf("%-5d %-9d %-14.4f %-10.4f %-8.3f %-8ld %-14.6f %-9.3f\n",
           256, blended256, blended256 / (256.0 * 256.0), e256.mean, e256.max, e256.differ,
           (double)e256.differ / (256.0 * 256.0), ms256);
    printf("\nblended-pixel ratio 256/32 : %.3f  (frame area ratio 64)\n",
           (double)blended256 / (double)blended32);
    printf("time ratio 256/32           : %.3f  (best of %d runs)\n", ms256 / ms32, REPEATS);
    printf("ns per pixel               : %.1f / %.1f\n",
           ms32 * 1e6 / (32.0 * 32.0), ms256 * 1e6 / (256.0 * 256.0));

    /* Cross resolution: sample the 256 image at the centre of each 32 pixel.
     * The 256 sample point is 0.5 of a 256-pixel from the 32-pixel centre, so
     * where an edge passes between the two points the colours legitimately
     * differ.  A discrepancy is only unexplained if the two points classify
     * identically in the reference yet the rendered colours disagree. */
    int scale = 256 / 32, cross = 0, cross_explained = 0, cross_unexplained = 0;
    int cross_max = 0;
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            const unsigned char *a = &img32[((size_t)y * 32 + x) * 3];
            const unsigned char *b = &img256[((size_t)(y * scale + scale / 2) * 256
                                             + (x * scale + scale / 2)) * 3];
            int d = 0;
            for (int c = 0; c < 3; ++c) {
                int ad = abs((int)a[c] - (int)b[c]);
                if (ad > d) d = ad;
            }
            if (!d) continue;
            ++cross;
            unsigned char r32[3], r256[3];
            expected_px(x + 0.5, y + 0.5, 32, r32);
            expected_px(x * scale + scale / 2 + 0.5, y * scale + scale / 2 + 0.5, 256, r256);
            if (memcmp(r32, r256, 3) != 0) ++cross_explained;
            else ++cross_unexplained;
            if (d > cross_max) cross_max = d;
        }
    }
    printf("cross-res sample (256 at 32-pixel centres): %d/1024 differ (max %d); "
           "%d explained by an edge between the two sample points, %d unexplained\n",
           cross, cross_max, cross_explained, cross_unexplained);

    printf("\nper-pixel hand calculation:\n");
    printf(" [32x32]\n");
    hand_calc(32, img32, 0.20, 0.20, "quad+tri1");
    hand_calc(32, img32, 0.34, 0.66, "tri1+tri2");
    hand_calc(32, img32, 0.296875, 0.140625, "quad only");
    hand_calc(32, img32, 0.890625, 0.453125, "tri2 only");
    printf(" [256x256]\n");
    hand_calc(256, img256, 0.20, 0.20, "quad+tri1");
    hand_calc(256, img256, 0.34, 0.66, "tri1+tri2");
    hand_calc(256, img256, 100.5 / 256.0, 36.5 / 256.0, "quad only");
    hand_calc(256, img256, 0.890625, 0.453125, "tri2 only");

    free(img32);
    free(img256);
    int bad = (e32.differ != 0 || e256.differ != 0 || cross_unexplained != 0);
    printf("\n%s\n", bad ? "RESULT: FAIL" : "RESULT: PASS (all pixels match the hand reference)");
    return bad ? 1 : 0;
}
