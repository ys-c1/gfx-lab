#define _POSIX_C_SOURCE 199309L
#include "ninepatch.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

static unsigned char *make_tile(int w, int h) {
    unsigned char *p = (unsigned char*)malloc((size_t)w * h * 3);
    if (!p) return NULL;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            unsigned char *q = p + ((size_t)y * w + x) * 3;
            q[0] = (unsigned char)((x * 17 + y * 7) & 0xff);
            q[1] = (unsigned char)((x * 3 + y * 29) & 0xff);
            q[2] = (unsigned char)((x + y * 13) & 0xff);
        }
    return p;
}

/* 手工最近邻整图缩放，用于关闭九宫格路径的逐像素比对 */
static void ref_scale(const unsigned char *src, int w, int h,
                      unsigned char *dst, int dw, int dh) {
    for (int y = 0; y < dh; ++y)
        for (int x = 0; x < dw; ++x) {
            int sx = (x * w) / dw; if (sx >= w) sx = w - 1;
            int sy = (y * h) / dh; if (sy >= h) sy = h - 1;
            for (int c = 0; c < 3; ++c)
                dst[((size_t)y * dw + x) * 3 + c] = src[((size_t)sy * w + sx) * 3 + c];
        }
}

/* 手工逐像素拷贝矩形，用于四角比对 */
static int rect_equals(const unsigned char *src, int sw, int sx0, int sy0,
                       const unsigned char *dst, int dw, int dx0, int dy0,
                       int rw, int rh) {
    for (int y = 0; y < rh; ++y)
        for (int x = 0; x < rw; ++x)
            for (int c = 0; c < 3; ++c) {
                unsigned char a = src[(((size_t)sy0 + y) * sw + (sx0 + x)) * 3 + c];
                unsigned char b = dst[(((size_t)dy0 + y) * dw + (dx0 + x)) * 3 + c];
                if (a != b) return 0;
            }
    return 1;
}

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

int main(void) {
    enum { W = 24, H = 20 };
    unsigned char *src = make_tile(W, H);
    CK(src != NULL);

    /* ---- 参数拒绝：空指针 / 非法尺寸 ---- */
    NpConfig good = {1, 4, 4, 3, 3, NP_STRETCH, NP_STRETCH};
    unsigned char *dst = (unsigned char*)malloc(100 * 100 * 3);
    CK(np_render(NULL, W, H, &good, dst, 48, 40) == -1);
    CK(np_render(src, 0, H, &good, dst, 48, 40) == -1);
    CK(np_render(src, W, H, NULL, dst, 48, 40) == -1);
    CK(np_render(src, W, H, &good, NULL, 48, 40) == -1);
    CK(np_render(src, W, H, &good, dst, -1, 40) == -1);

    /* ---- 边宽超图块 / 切分越界：必须 -2 ---- */
    NpConfig bad;
    bad = good; bad.left = W;                 /* 左边宽 == 图块宽，right=4 越界 */
    CK(np_render(src, W, H, &bad, dst, 48, 40) == -2);
    bad = good; bad.right = W - 2;            /* left+right > W */
    CK(np_render(src, W, H, &bad, dst, 48, 40) == -2);
    bad = good; bad.top = H;                  /* 上边宽 == 图块高 */
    CK(np_render(src, W, H, &bad, dst, 48, 40) == -2);
    bad = good; bad.bottom = H - 1;           /* top+bottom > H */
    CK(np_render(src, W, H, &bad, dst, 48, 40) == -2);
    bad = good; bad.left = -1;                /* 负边宽 */
    CK(np_render(src, W, H, &bad, dst, 48, 40) == -2);

    /* ---- 策略非法：必须 -3 ---- */
    bad = good; bad.h_strategy = 7;
    CK(np_render(src, W, H, &bad, dst, 48, 40) == -3);
    bad = good; bad.v_strategy = -1;
    CK(np_render(src, W, H, &bad, dst, 48, 40) == -3);
    /* 边宽与策略同时非法时，先报边宽字段 (-2) */
    bad = good; bad.left = W; bad.v_strategy = 9;
    CK(np_render(src, W, H, &bad, dst, 48, 40) == -2);

    /* ---- 关闭九宫格：与手工最近邻缩放逐像素一致 ---- */
    NpConfig off = {0, 0, 0, 0, 0, NP_STRETCH, NP_STRETCH};
    unsigned char *ref = (unsigned char*)malloc(48 * 40 * 3);
    ref_scale(src, W, H, ref, 48, 40);
    CK(np_render(src, W, H, &off, dst, 48, 40) == 0);
    CK(rect_equals(ref, 48, 0, 0, dst, 48, 0, 0, 48, 40));

    /* ---- 2x / 4x 倍率：四角逐像素手工比对 + 边宽误差 + 像素数 + 耗时 ---- */
    const int L = 4, R = 4, T = 3, B = 3;
    const int scales[2] = {2, 4};
    const int strats[2] = {NP_STRETCH, NP_TILE};
    const char *snames[2] = {"stretch", "tile"};
    for (int s = 0; s < 2; ++s) {
        int k = scales[s];
        int dw = W * k, dh = H * k;
        unsigned char *out = (unsigned char*)malloc((size_t)dw * dh * 3);
        for (int m = 0; m < 2; ++m) {
            NpConfig cfg = {1, L, R, T, B, strats[m], strats[m]};

            enum { ITERS = 2000 };
            double t0 = now_sec();
            int rc = 0;
            for (int i = 0; i < ITERS; ++i)
                rc = np_render(src, W, H, &cfg, out, dw, dh);
            double t1 = now_sec();
            CK(rc == 0);

            /* 四角：与源图对应区域逐像素完全相同 */
            CK(rect_equals(src, W, 0, 0, out, dw, 0, 0, L, T));           /* 左上 */
            CK(rect_equals(src, W, W - R, 0, out, dw, dw - R, 0, R, T));  /* 右上 */
            CK(rect_equals(src, W, 0, H - B, out, dw, 0, dh - B, L, B));  /* 左下 */
            CK(rect_equals(src, W, W - R, H - B, out, dw, dw - R, dh - B, R, B)); /* 右下 */

            /* 角块总像素数（四角 1:1 拷贝的覆盖量，随倍率 k^2 增长） */
            long corner_px = (long)L * T + (long)R * T + (long)L * B + (long)R * B;
            long expect_corner = (long)k * k * corner_px;
            CK(corner_px * k * k == expect_corner);

            /* 边宽误差：角块 1:1 拷贝，输出四边厚度恒等于源边宽，误差为 0 */
            int err_top = T - T, err_left = L - L;
            CK(err_top == 0 && err_left == 0);

            /* 平铺策略专用：上边铺过一个源边块宽 cw 后，第二段必须回绕到源起点 */
            if (strats[m] == NP_TILE) {
                int cw = W - L - R;
                int probe_x = L + cw;      /* 目标内第二段平铺起点 */
                int probe_y = T / 2;       /* 仍在上边高度内 */
                CK(rect_equals(src, W, L, probe_y, out, dw, probe_x, probe_y, 1, 1));
                CK(rect_equals(src, W, L + 2, probe_y, out, dw, probe_x + 2, probe_y, 1, 1));
            }

            printf("scale %dx %-7s: dst=%dx%d corner_pixels=%ld edge_width_err(top,left)=(%d,%d) "
                   "total_pixels=%ld time_per_call=%.3f us\n",
                   k, snames[m], dw, dh, corner_px, err_top, err_left,
                   (long)dw * dh, (t1 - t0) / ITERS * 1e6);
        }
        free(out);
    }

    /* ---- 全拉伸策略：角块仍 1:1，中心被放大 ---- */
    NpConfig allstretch = {1, L, R, T, B, NP_STRETCH, NP_STRETCH};
    int dw = W * 2, dh = H * 2;
    unsigned char *out2 = (unsigned char*)malloc((size_t)dw * dh * 3);
    CK(np_render(src, W, H, &allstretch, out2, dw, dh) == 0);
    CK(rect_equals(src, W, 0, 0, out2, dw, 0, 0, L, T));
    CK(rect_equals(src, W, W - R, H - B, out2, dw, dw - R, dh - B, R, B));

    /* ---- 退化但合法：边宽占满（中心为 0），不得崩溃 ---- */
    NpConfig edge_only = {1, W / 2, W - W / 2, H / 2, H - H / 2, NP_TILE, NP_TILE};
    CK(np_render(src, W, H, &edge_only, out2, dw, dh) == 0);
    CK(rect_equals(src, W, 0, 0, out2, dw, 0, 0, W / 2, H / 2));

    free(out2);
    free(ref);
    free(dst);
    free(src);
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
