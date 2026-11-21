#define _POSIX_C_SOURCE 199309L
#include "mip.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { TEX = 32 };

typedef struct { double sx, sy, u, v; } TVert;
typedef struct { TVert v[3]; int id; } TTri;

/* ---- 独立手算参考实现：不调用 mip 模块任何函数，从源纹理数组直接算 ---- */

static void ref_address(int mode, double t, double *out) {
    if (mode == MIP_ADDR_CLAMP) {
        if (t < 0.0) t = 0.0;
        else if (t >= 1.0) t = 1.0 - 1e-15;
        *out = t;
    } else if (mode == MIP_ADDR_WRAP) {
        *out = t - floor(t);
    } else { /* mirror：用整数奇偶判断，与库内 fmod 路径相互独立 */
        double z = fabs(t);
        long long p = (long long)floor(z);
        double f = z - floor(z);
        *out = (p & 1LL) ? 1.0 - f : f;
    }
}

static void ref_sample(const unsigned char *tex, int tw, int th,
                       int mu, int mv, double u, double v, unsigned char out[3]) {
    double tu, tv;
    ref_address(mu, u, &tu);
    ref_address(mv, v, &tv);
    int x = (int)(tu * tw), y = (int)(tv * th);
    if (x < 0) x = 0; else if (x >= tw) x = tw - 1;
    if (y < 0) y = 0; else if (y >= th) y = th - 1;
    for (int ch = 0; ch < 3; ++ch) out[ch] = tex[(y * tw + x) * 3 + ch];
}

/* ---- 屏幕空间三角形 + 重心插值 ---- */

static double edge_fn(const TVert *a, const TVert *b, double px, double py) {
    return (px - a->sx) * (b->sy - a->sy) - (py - a->sy) * (b->sx - a->sx);
}

/* 命中返回三角形 id（>=0），重心坐标写入 w，否则返回 -1。兼容顺/逆时针绕序。 */
static int tri_bary(const TTri *t, double px, double py, double w[3]) {
    double area = edge_fn(&t->v[0], &t->v[1], t->v[2].sx, t->v[2].sy);
    double w0 = edge_fn(&t->v[1], &t->v[2], px, py);
    double w1 = edge_fn(&t->v[2], &t->v[0], px, py);
    double w2 = edge_fn(&t->v[0], &t->v[1], px, py);
    if (area > 0) {
        if (w0 < 0 || w1 < 0 || w2 < 0) return -1;
    } else {
        if (w0 > 0 || w1 > 0 || w2 > 0) return -1;
    }
    w[0] = w0 / area; w[1] = w1 / area; w[2] = w2 / area;
    return t->id;
}

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static double maxd(double a, double b) { return a > b ? a : b; }

int main(void) {
    unsigned char *tex = (unsigned char*)malloc(TEX * TEX * 3);
    for (int y = 0; y < TEX; ++y)
        for (int x = 0; x < TEX; ++x) {
            unsigned char *p = tex + (y * TEX + x) * 3;
            p[0] = (unsigned char)(x * 8);
            p[1] = (unsigned char)(y * 8);
            p[2] = (unsigned char)((x + y) * 4);
        }
    MipChain c;
    if (mip_build(&c, tex, TEX, TEX) != 0) return 1;
    printf("mipchain: %d levels, base %dx%d\n", c.levels, c.w0, c.h0);

    /* 两个铺满画面的三角形，UV 落在 [-2,3)，强制大量重复平铺与越界采样 */
    TTri tris[2];
    tris[0].id = 0;
    tris[0].v[0] = (TVert){0, 0, -2, -2};
    tris[0].v[1] = (TVert){1, 0,  3, -2};
    tris[0].v[2] = (TVert){1, 1,  3,  3};
    tris[1].id = 1;
    tris[1].v[0] = (TVert){0, 0, -2, -2};
    tris[1].v[1] = (TVert){1, 1,  3,  3};
    tris[1].v[2] = (TVert){0, 1, -2,  3};

    MipAddr table[2] = { { MIP_ADDR_WRAP }, { MIP_ADDR_WRAP } };
    MipSampler wrap_s = { 1, table };
    MipSampler off_s  = { 0, NULL };

    const int ITERS = 200;
    int resolutions[2] = { 32, 256 };
    size_t cap = (size_t)256 * 256 * 3;
    unsigned char *frame   = (unsigned char*)malloc(cap);
    unsigned char *lib_buf = (unsigned char*)malloc(cap);
    unsigned char *ref_buf = (unsigned char*)malloc(cap);

    for (int ri = 0; ri < 2; ++ri) {
        int R = resolutions[ri];
        long long samples = 0, oob = 0, mismatch = 0;
        long long abs_sum = 0, max_diff = 0, identity_mismatch = 0;
        double t_lib = 0, t_ref = 0;

        for (int it = 0; it < ITERS; ++it) {
            double t0 = now_sec();
            for (int py = 0; py < R; ++py)
                for (int px = 0; px < R; ++px) {
                    double nx = (px + 0.5) / R, ny = (py + 0.5) / R, w[3];
                    int id = -1;
                    for (int k = 0; k < 2 && id < 0; ++k) id = tri_bary(&tris[k], nx, ny, w);
                    if (id < 0) continue;
                    const TTri *t = &tris[id];
                    double u = w[0]*t->v[0].u + w[1]*t->v[1].u + w[2]*t->v[2].u;
                    double v = w[0]*t->v[0].v + w[1]*t->v[1].v + w[2]*t->v[2].v;
                    mip_sample_ex(&c, 0, &wrap_s, u, v, id, lib_buf + (py * R + px) * 3);
                }
            double t1 = now_sec();
            for (int py = 0; py < R; ++py)
                for (int px = 0; px < R; ++px) {
                    double nx = (px + 0.5) / R, ny = (py + 0.5) / R, w[3];
                    int id = -1;
                    for (int k = 0; k < 2 && id < 0; ++k) id = tri_bary(&tris[k], nx, ny, w);
                    if (id < 0) continue;
                    const TTri *t = &tris[id];
                    double u = w[0]*t->v[0].u + w[1]*t->v[1].u + w[2]*t->v[2].u;
                    double v = w[0]*t->v[0].v + w[1]*t->v[1].v + w[2]*t->v[2].v;
                    ref_sample(tex, TEX, TEX, MIP_ADDR_WRAP, MIP_ADDR_WRAP, u, v,
                               ref_buf + (py * R + px) * 3);
                }
            double t2 = now_sec();
            t_lib += t1 - t0;
            t_ref += t2 - t1;

            if (it == 0) {
                for (int py = 0; py < R; ++py)
                    for (int px = 0; px < R; ++px) {
                        size_t idx = (size_t)(py * R + px) * 3;
                        double nx = (px + 0.5) / R, ny = (py + 0.5) / R, w[3];
                        int id = -1;
                        for (int k = 0; k < 2 && id < 0; ++k) id = tri_bary(&tris[k], nx, ny, w);
                        if (id < 0) continue;
                        const TTri *t = &tris[id];
                        double u = w[0]*t->v[0].u + w[1]*t->v[1].u + w[2]*t->v[2].u;
                        double v = w[0]*t->v[0].v + w[1]*t->v[1].v + w[2]*t->v[2].v;
                        unsigned char legacy[3], disabled[3];
                        mip_sample(&c, 0, u, v, legacy);
                        mip_sample_ex(&c, 0, &off_s, u, v, id, disabled);
                        ++samples;
                        if (u < 0.0 || u >= 1.0 || v < 0.0 || v >= 1.0) ++oob;
                        for (int ch = 0; ch < 3; ++ch) {
                            long long d = (long long)lib_buf[idx+ch] - ref_buf[idx+ch];
                            if (d < 0) d = -d;
                            abs_sum += d;
                            max_diff = maxd(max_diff, d);
                            if (d) ++mismatch;
                            if (legacy[ch] != disabled[ch]) ++identity_mismatch;
                        }
                        if (R == 256) memcpy(frame + idx, lib_buf + idx, 3);
                    }
            }
        }

        printf("\n== frame %dx%d, %d iters ==\n", R, R, ITERS);
        printf("samples            : %lld\n", samples);
        printf("out-of-bounds raw  : %lld (%.2f%%)\n", oob, 100.0 * oob / samples);
        printf("color vs hand calc : max|d|=%lld  sum|d|=%lld  mismatched channels=%lld\n",
               max_diff, abs_sum, mismatch);
        printf("disabled vs legacy : mismatched channels=%lld (must be 0)\n", identity_mismatch);
        printf("time per frame     : lib=%.3f us  hand-ref=%.3f us\n",
               t_lib / ITERS * 1e6, t_ref / ITERS * 1e6);
    }

    /* level 1 上抽验：三种模式各自与独立手算实现逐像素对比。
     * 参考实现直接读 MipChain 的 level 像素，但寻址/取样逻辑完全独立。 */
    int modes[3] = { MIP_ADDR_CLAMP, MIP_ADDR_WRAP, MIP_ADDR_MIRROR };
    const char *mname[3] = { "clamp", "wrap", "mirror" };
    for (int mi = 0; mi < 3; ++mi) {
        MipAddr t2[2] = { { modes[mi] }, { modes[mi] } };
        MipSampler s2 = { 1, t2 };
        long long l1_mismatch = 0, l1_samples = 0;
        for (int i = 0; i < 1000; ++i) {
            double u = -2.0 + (i * 7.13) / 1000.0, v = -1.5 + (i * 3.77) / 1000.0;
            unsigned char lib[3], hand[3];
            mip_sample_ex(&c, 1, &s2, u, v, 9, lib);
            ref_sample(c.pix[1], c.w[1], c.h[1], modes[mi], modes[mi], u, v, hand);
            for (int ch = 0; ch < 3; ++ch) if (lib[ch] != hand[ch]) ++l1_mismatch;
            ++l1_samples;
        }
        printf("level1 %-6s spot check: %lld samples, mismatched channels=%lld\n",
               mname[mi], l1_samples * 3, l1_mismatch);
    }

    /* 错误注入：三类错误分别返回不同错误码，stderr 报告会带三角形 id */
    unsigned char px[3];
    MipSampler bad = { 1, (MipAddr[2]){ { 7 }, { MIP_ADDR_WRAP } } };
    MipSampler no_table = { 1, NULL };
    printf("\nerror injection (triangle ids shown on stderr):\n");
    printf("  bad mode    -> %d (expect %d)\n", mip_sample_ex(&c, 0, &bad, 0.5, 0.5, 101, px), MIP_ERR_MODE);
    printf("  non-finite  -> %d (expect %d)\n", mip_sample_ex(&c, 0, &wrap_s, INFINITY, 0.5, 102, px), MIP_ERR_NONFIN);
    printf("  table missing-> %d (expect %d)\n", mip_sample_ex(&c, 0, &no_table, 0.5, 0.5, 103, px), MIP_ERR_NOTABLE);

    FILE *f = fopen("demo.ppm", "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", 256, 256);
        fwrite(frame, 1, 256 * 256 * 3, f);
        fclose(f);
        printf("\nwrote demo.ppm (256x256, wrap/wrap)\n");
    }

    free(frame);
    free(lib_buf);
    free(ref_buf);
    mip_free(&c);
    free(tex);
    return 0;
}
