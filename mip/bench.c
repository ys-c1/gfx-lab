#define _POSIX_C_SOURCE 199309L
#include "mip.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

/* 每个配置跑 REPS 次取最小耗时，降低噪声 */
#define REPS 5

typedef struct {
    const char *name;
    double sharpness, delta, ms;
    unsigned long samples;
} Row;

static Row run(const MipChain *c, int mode, int ow, int oh, unsigned char *out) {
    /* mode: 0=双线性 1=锐化weight=1.0 2=手工反卷积x8 */
    Row r; MipUpscaleStats st;
    r.name = mode == 0 ? "bilinear" : mode == 1 ? "sharpen w=1.0" : "deconv iter=8";
    r.ms = 1e30; r.sharpness = 0; r.samples = 0;
    for (int rep = 0; rep < REPS; ++rep) {
        double t0 = now_ms();
        int rc = mode == 2 ? mip_deconv(c, 0, 8, ow, oh, out, &st)
                           : mip_upscale(c, 0, mode == 1 ? 1.0 : 0.0, ow, oh, out, &st);
        double t1 = now_ms();
        if (rc != 0) { fprintf(stderr, "rc=%d field=%s\n", rc, mip_err_field(rc)); exit(1); }
        if (t1 - t0 < r.ms) r.ms = t1 - t0;
        r.sharpness = st.sharpness;
        r.samples = st.samples;
    }
    return r;
}

int main(void) {
    enum { W = 64, H = 64 };
    unsigned char *src = (unsigned char*)malloc(W*H*3);
    /* 测试图：垂直阶跃边 + 4px 棋盘格，边缘丰富 */
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            int edge = x < W / 2 ? 0 : 1;
            int chk = ((x / 4) + (y / 4)) & 1;
            unsigned char v = (unsigned char)(edge ? (chk ? 255 : 192) : (chk ? 64 : 0));
            unsigned char *p = src + (y*W + x) * 3;
            p[0] = v; p[1] = v; p[2] = v;
        }
    MipChain c;
    if (mip_build(&c, src, W, H) != 0) return 1;

    for (int scale = 2; scale <= 8; scale *= 4) {
        int ow = W * scale, oh = H * scale;
        unsigned char *out = (unsigned char*)malloc((size_t)ow*oh*3);
        Row rows[3];
        for (int m = 0; m < 3; ++m) rows[m] = run(&c, m, ow, oh, out);
        printf("== 倍率 %dx (输出 %dx%d) ==\n", scale, ow, oh);
        printf("%-14s %12s %12s %14s %10s\n", "方法", "锐度(均梯度)", "锐度差", "采样次数", "耗时ms");
        for (int m = 0; m < 3; ++m) {
            double d = rows[m].sharpness - rows[0].sharpness;
            printf("%-14s %12.3f %+12.3f %14lu %10.3f\n",
                   rows[m].name, rows[m].sharpness, d, rows[m].samples, rows[m].ms);
        }
        printf("\n");
        free(out);
    }
    mip_free(&c);
    free(src);
    return 0;
}
