#define _POSIX_C_SOURCE 199309L
#include "mip.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define W 64
#define H 64
#define N 200000
#define GAMMA 2.2

static unsigned long long rng_state;
static void rng_reset(void) { rng_state = 0x9E3779B97F4A7C15ULL; }
static double rnd(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (rng_state >> 11) * (1.0 / 9007199254740992.0);
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

/* 手算对照:不经过金字塔,直接从原图解码到线性,
   对 2^lo 与 2^hi 的 footprint 分别盒式平均,按 t 插值后编回。 */
static void ref_sample(const unsigned char *src, double level, double u, double v,
                       unsigned char out[3]) {
    int lo = (int)level;
    int hi = lo + 1;
    double t = level - lo;
    int maxl = 0;
    while ((W >> maxl) > 1) ++maxl;
    if (hi > maxl) hi = maxl;
    for (int ch = 0; ch < 3; ++ch) {
        double lin = 0.0;
        for (int k = 0; k < 2; ++k) {
            int l = k ? hi : lo;
            double wgt = k ? t : 1.0 - t;
            if (k && wgt == 0.0) continue;
            int wl = W >> l, hl = H >> l;
            int tx = (int)(u * wl), ty = (int)(v * hl);
            if (tx >= wl) tx = wl - 1;
            if (ty >= hl) ty = hl - 1;
            int size = 1 << l;
            double acc = 0.0;
            int cnt = 0;
            for (int y = ty * size; y < ty * size + size && y < H; ++y)
                for (int x = tx * size; x < tx * size + size && x < W; ++x) {
                    acc += pow(src[(y * W + x) * 3 + ch] / 255.0, GAMMA);
                    ++cnt;
                }
            lin += wgt * acc / cnt;
        }
        int v8 = (int)(pow(lin, 1.0 / GAMMA) * 255.0 + 0.5);
        out[ch] = (unsigned char)(v8 < 0 ? 0 : v8 > 255 ? 255 : v8);
    }
}

int main(void) {
    static unsigned char src[W * H * 3];
    /* 1px 棋盘格 0/255:亮部发暗最极端的情形 */
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            unsigned char v = (unsigned char)(((x ^ y) & 1) ? 255 : 0);
            src[(y * W + x) * 3 + 0] = v;
            src[(y * W + x) * 3 + 1] = v;
            src[(y * W + x) * 3 + 2] = v;
        }

    MipChain lin, leg;
    MipError e;
    if (mip_build_ex(&lin, src, W, H, GAMMA, &e) != MIP_OK) return 1;
    if (mip_build_ex(&leg, src, W, H, 0.0, &e) != MIP_OK) return 1;

    /* 手算锚点:全图线性平均 0.5 -> 编回 0.5^(1/2.2)*255 = 186 */
    unsigned char px[3];
    mip_sample_ex(&lin, 6.0, 0.5, 0.5, px, NULL, NULL);
    int anchor_lin = px[0];
    mip_sample_ex(&leg, 6.0, 0.5, 0.5, px, NULL, NULL);
    printf("锚点(全图平均): 手算 186  线性链 %d  旧链 %d\n\n", anchor_lin, px[0]);

    printf("%-6s %-8s %-14s %-16s %-14s %-10s %s\n",
           "倍率", "层号", "层链", "亮度误差(线性)", "亮度误差(旧)", "采样次数", "耗时");
    double ratios[] = {4.0, 16.0, 1.5};
    for (int r = 0; r < 3; ++r) {
        double level = log2(ratios[r]);
        unsigned char o[3], o2[3], rf[3];
        MipTrace tr;

        rng_reset();
        double t0 = now_ms();
        long samples = 0;
        for (int i = 0; i < N; ++i) {
            mip_sample_ex(&lin, level, rnd(), rnd(), o, &tr, NULL);
            samples += tr.samples;
        }
        double t1 = now_ms();
        for (int i = 0; i < N; ++i)
            mip_sample_ex(&leg, level, rnd(), rnd(), o2, NULL, NULL);
        double t2 = now_ms();

        rng_reset();
        double el = 0.0, eg = 0.0;
        int ml = 0, mg = 0;
        for (int i = 0; i < N; ++i) {
            double u = rnd(), v = rnd();
            mip_sample_ex(&lin, level, u, v, o, NULL, NULL);
            mip_sample_ex(&leg, level, u, v, o2, NULL, NULL);
            ref_sample(src, level, u, v, rf);
            int d = abs(o[0] - rf[0]);
            el += d;
            if (d > ml) ml = d;
            d = abs(o2[0] - rf[0]);
            eg += d;
            if (d > mg) mg = d;
        }

        mip_sample_ex(&lin, level, 0.5, 0.5, o, &tr, NULL);
        char chain[32];
        snprintf(chain, sizeof chain, "[%d->%d] t=%.3f", tr.lo, tr.hi, tr.t);
        printf("%-6.1f %-8.4f %-14s 均%.3f 最大%-6d 均%-8.2f %-10ld %.1f ms (%.0f ns/次, 旧 %.0f)\n",
               ratios[r], level, chain, el / N, ml, eg / N, samples,
               t1 - t0, (t1 - t0) * 1e6 / N, (t2 - t1) * 1e6 / N);
    }

    mip_free(&lin);
    mip_free(&leg);
    return 0;
}
