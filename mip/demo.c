#include "mip.h"
#include "sharpen.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double now_s(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void gen_img(unsigned char *img, int w, int h) {
    unsigned s = 12345u;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            unsigned char *px = img + ((size_t)y * w + x) * 3;
            int edge = ((x / 8) + (y / 8)) % 2;
            s = s * 1664525u + 1013904223u;
            int nz = (int)((s >> 24) & 7);
            px[0] = (unsigned char)(edge ? 200 + nz : 30 + nz);
            px[1] = (unsigned char)(edge ? 180 - nz : 60 + nz);
            px[2] = (unsigned char)((x * 7 + y * 3) & 0xff);
        }
}

static int bench(int w, int h, int reps) {
    size_t n = (size_t)w * (size_t)h;
    unsigned char *img = (unsigned char *)malloc(n * 3);
    unsigned char *dst = (unsigned char *)malloc(n * 3);
    unsigned char *ref = (unsigned char *)malloc(n * 3);
    if (!img || !dst || !ref) { free(img); free(dst); free(ref); return 1; }
    gen_img(img, w, h);
    MipSharpenParams p = { 1.2, 6.0, 96.0 };
    MipSharpenStats st, rst;
    if (mip_sharpen(img, w, h, &p, dst, &st) != 0) return 1;
    if (mip_sharpen_ref(img, w, h, &p, ref, &rst) != 0) return 1;

    /* 误差: 与参考实现逐字节对比 */
    int err_max = 0;
    double err_sum = 0.0;
    long ndiff = 0;
    for (size_t i = 0; i < n * 3; ++i) {
        int d = abs((int)dst[i] - (int)ref[i]);
        if (d > err_max) err_max = d;
        err_sum += d;
        if (d) ++ndiff;
    }
    double t0 = now_s();
    for (int r = 0; r < reps; ++r) mip_sharpen(img, w, h, &p, dst, NULL);
    double dt = now_s() - t0;

    printf("n=%zu (%dx%d) err_max=%d err_mean=%.6g diff_bytes=%ld time=%.3fms (%.2fus/call)\n",
           n, w, h, err_max, err_sum / (double)(n * 3), ndiff,
           dt * 1e3, dt * 1e6 / reps);
    printf("  overshoot=%.6f strength_mean=%.6f strength_max=%.6f edge_px=%ld ringing_px=%ld\n",
           st.overshoot, st.strength_mean, st.strength_max, st.edge_pixels, st.ringing_pixels);
    printf("  ref: overshoot=%.6f strength_mean=%.6f edge_px=%ld ringing_px=%ld\n",
           rst.overshoot, rst.strength_mean, rst.edge_pixels, rst.ringing_pixels);
    free(img); free(dst); free(ref);
    return 0;
}

int main(void) {
    enum { W = 64, H = 64 };
    unsigned char *src = (unsigned char*)malloc(W*H*3);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            unsigned char *p = src + (y*W+x)*3;
            p[0] = (unsigned char)(x * 4); p[1] = (unsigned char)(y * 4); p[2] = 128;
        }
    MipChain c;
    if (mip_build(&c, src, W, H) != 0) return 1;
    printf("levels %d sizes %dx%d %dx%d\n", c.levels, c.w[0], c.h[0], c.w[c.levels-1], c.h[c.levels-1]);
    mip_free(&c);
    free(src);

    if (bench(40, 25, 20000) != 0) return 1;    /* 1 千 texel */
    if (bench(100, 100, 2000) != 0) return 1;   /* 1 万 texel */
    return 0;
}
