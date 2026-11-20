#define _POSIX_C_SOURCE 199309L
#include "ninepatch.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

enum { W = 24, H = 20, L = 4, R = 4, T = 3, B = 3 };

/* 构造一个按钮图块：圆角感边框 + 高亮上沿，四角用特殊颜色便于目视检查 */
static unsigned char *make_button(void) {
    unsigned char *p = (unsigned char*)calloc((size_t)W * H * 3, 1);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            unsigned char *q = p + ((size_t)y * W + x) * 3;
            int border = (x < L || x >= W - R || y < T || y >= H - B);
            if (border) {
                q[0] = (unsigned char)(40 + y * 6);   /* 深蓝边框，纵向渐变 */
                q[1] = (unsigned char)(80 + y * 4);
                q[2] = 200;
            } else {
                q[0] = 180; q[1] = 200; q[2] = 230;   /* 浅蓝中心面 */
            }
        }
    /* 四角标记为醒目的红色，拉伸后若角部变形可一眼看出 */
    const int corners[4][2] = {{0, 0}, {W - L, 0}, {0, H - B}, {W - L, H - B}};
    for (int c = 0; c < 4; ++c)
        for (int y = 0; y < B; ++y)
            for (int x = 0; x < L; ++x) {
                unsigned char *q = p + ((size_t)(corners[c][1] + y) * W + (corners[c][0] + x)) * 3;
                q[0] = 220; q[1] = 40; q[2] = 40;
            }
    return p;
}

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

int main(void) {
    unsigned char *src = make_button();
    enum { DW = 96, DH = 48 }; /* 目标按钮：宽放大 4x、高放大 2.4x，普通拉伸会令边框变形 */
    unsigned char *dst = (unsigned char*)malloc((size_t)DW * DH * 3);
    if (!dst) return 1;

    NpConfig off = {0, 0, 0, 0, 0, NP_STRETCH, NP_STRETCH};
    NpConfig stretch = {1, L, R, T, B, NP_STRETCH, NP_STRETCH};
    NpConfig tile = {1, L, R, T, B, NP_TILE, NP_TILE};

    const char *names[3] = {"demo_off.ppm", "demo_stretch.ppm", "demo_tile.ppm"};
    NpConfig *cfgs[3] = {&off, &stretch, &tile};

    enum { ITERS = 5000 };
    for (int m = 0; m < 3; ++m) {
        double t0 = now_sec();
        for (int i = 0; i < ITERS; ++i)
            if (np_render(src, W, H, cfgs[m], dst, DW, DH) != 0) return 1;
        double t1 = now_sec();
        if (np_write_ppm(dst, DW, DH, names[m]) != 0) return 1;
        printf("%-16s %dx%d -> %dx%d  %.3f us/call\n", names[m], W, H, DW, DH,
               (t1 - t0) / ITERS * 1e6);
    }

    /* 拒绝路径演示：边宽超图块 (-2) 与策略非法 (-3) */
    NpConfig bad_edge = {1, W, R, T, B, NP_STRETCH, NP_STRETCH};
    NpConfig bad_strat = {1, L, R, T, B, 9, NP_TILE};
    printf("reject edge overflow: %d (expect -2)\n", np_render(src, W, H, &bad_edge, dst, DW, DH));
    printf("reject bad strategy:  %d (expect -3)\n", np_render(src, W, H, &bad_strat, dst, DW, DH));

    free(dst);
    free(src);
    return 0;
}
