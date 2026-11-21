#include "raster.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

/* 独立参考实现：1 像素 Bresenham，代表“现在”的画面 */
static void ref_line(unsigned char *img, int w, int x0, int y0, int x1, int y1) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        img[((size_t)y0 * w + x0) * 3 + 0] = 255;
        img[((size_t)y0 * w + x0) * 3 + 1] = 255;
        img[((size_t)y0 * w + x0) * 3 + 2] = 255;
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static RLine mk(double x0, double y0, double x1, double y1, double wdt, int lv, int aa) {
    RLine l = { x0, y0, x1, y1, wdt, lv, aa, {255, 255, 255}, {0} };
    return l;
}

int main(void) {
    RTarget t;
    CK(rt_init(&t, 32, 32) == 0);
    RTri tri = {{ {4,4,0.5}, {28,4,0.5}, {16,28,0.5} }, {255, 0, 0}};
    int n = rt_triangle(&t, &tri);
    CK(n > 200);
    CK(t.z[16*32+16] < 1.0f);
    CK(t.rgb[(16*32+16)*3] == 255);
    RTri flat = {{ {0,0,0}, {8,8,0}, {16,16,0} }, {0,255,0}};
    CK(rt_triangle(&t, &flat) == -2);
    rt_free(&t);

    /* --- 画线：参数越界报错并指明字段 --- */
    CK(rt_init(&t, 8, 8) == 0);
    RLine l;
    l = mk(0, 0, 7, 7, 0.1, 256, 1);  CK(rt_line(&t, &l) == -1 && strcmp(l.err, "width") == 0);
    l = mk(0, 0, 7, 7, 65.0, 256, 1); CK(rt_line(&t, &l) == -1 && strcmp(l.err, "width") == 0);
    l = mk(0, 0, 7, 7, 1.0, 1, 1);    CK(rt_line(&t, &l) == -1 && strcmp(l.err, "levels") == 0);
    l = mk(0, 0, 7, 7, 1.0, 300, 1);  CK(rt_line(&t, &l) == -1 && strcmp(l.err, "levels") == 0);
    l = mk(NAN, 0, 7, 7, 1.0, 256, 1); CK(rt_line(&t, &l) == -1 && strcmp(l.err, "x0") == 0);
    l = mk(0, 0, 7, INFINITY, 1.0, 256, 1); CK(rt_line(&t, &l) == -1 && strcmp(l.err, "y1") == 0);
    l = mk(0, 0, 7, 7, 1.0, 256, 2);  CK(rt_line(&t, &l) == -1 && strcmp(l.err, "aa") == 0);
    CK(rt_line(&t, NULL) == -2);
    CK(rt_line(NULL, &l) == -2);

    /* --- 开关关掉：与参考 Bresenham 逐像素一致，且不受 width/levels 影响 --- */
    {
        RTarget a, b;
        CK(rt_init(&a, 32, 32) == 0 && rt_init(&b, 32, 32) == 0);
        unsigned char ref[32 * 32 * 3] = {0};
        ref_line(ref, 32, 2, 3, 29, 20);
        l = mk(2, 3, 29, 20, 1.0, 256, 0); CK(rt_line(&a, &l) > 0);
        l = mk(2, 3, 29, 20, 4.0, 32, 0);  CK(rt_line(&b, &l) > 0); /* 不同 width/levels */
        CK(memcmp(a.rgb, ref, sizeof ref) == 0);
        CK(memcmp(a.rgb, b.rgb, 32 * 32 * 3) == 0);
        rt_free(&a); rt_free(&b);
    }

    /* --- 手算对照（8x8，白线） ---
     * 横线 (0.5,2.5)-(7.5,2.5) 宽1：wcov=1，span[2,3]，行2 全覆盖
     *   => 8 个像素全 255，无边缘档。
     * 横线 宽2：span[1.5,3.5]，行1/3 各 cov=0.5 -> 256档: round(127.5)=128
     *   => 24 像素，行1=行3=128，行2=255。
     * 斜线 (0.5 to 7.5 对角) 宽1：wcov=sqrt(2)，span 半宽 ~0.7071
     *   列 i：行 i cov=1 -> 255；行 i±1 cov=0.20711
     *   -> 256档: round(0.20711*255)=53；32档: round(0.20711*31)=6 -> 6*255/31=49
     *   列0/列7 各少一个越界像素 => 覆盖 6*3+2*2=22。 */
    l = mk(0.5, 2.5, 7.5, 2.5, 1.0, 256, 1);
    CK(rt_line(&t, &l) == 8);
    CK(t.rgb[(2*8+0)*3] == 255 && t.rgb[(2*8+7)*3] == 255);
    CK(t.rgb[(1*8+3)*3] == 0 && t.rgb[(3*8+3)*3] == 0);

    CK(rt_init(&t, 8, 8) == 0);
    l = mk(0.5, 2.5, 7.5, 2.5, 2.0, 256, 1);
    CK(rt_line(&t, &l) == 24);
    CK(t.rgb[(1*8+4)*3] == 128 && t.rgb[(3*8+4)*3] == 128 && t.rgb[(2*8+4)*3] == 255);

    CK(rt_init(&t, 8, 8) == 0);
    l = mk(0.5, 0.5, 7.5, 7.5, 1.0, 256, 1);
    CK(rt_line(&t, &l) == 22);
    CK(t.rgb[(0*8+0)*3] == 255);
    CK(t.rgb[(0*8+1)*3] == 53 && t.rgb[(1*8+0)*3] == 53);
    CK(t.rgb[(7*8+7)*3] == 255 && t.rgb[(6*8+7)*3] == 53);

    CK(rt_init(&t, 8, 8) == 0);
    l = mk(0.5, 0.5, 7.5, 7.5, 1.0, 32, 1);
    CK(rt_line(&t, &l) == 22);
    CK(t.rgb[(0*8+1)*3] == 49);
    rt_free(&t);

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
