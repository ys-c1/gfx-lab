#include "raster.h"
#include <stdio.h>
#include <string.h>
static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

static void fill_scene(RTarget *t, const RTri *tris, int n, const RDepthMask *m) {
    for (int i = 0; i < n; ++i) {
        if (m) rt_triangle_ex(t, &tris[i], m, i);
        else rt_triangle(t, &tris[i]);
    }
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

    /* 掩码关闭（enabled=0 或 mask=NULL）必须与旧路径逐字节一致 */
    RTri scene[3] = {
        {{ {2,2,0.9},   {60,6,0.4},  {30,58,0.6} }, {255, 0, 0}},
        {{ {8,50,0.3},  {56,44,0.8}, {40,4,0.2}  }, {0, 255, 0}},
        {{ {16,16,0.5}, {48,20,0.5}, {32,48,0.7} }, {0, 0, 255}},
    };
    RTarget a, b, c;
    CK(rt_init(&a, 64, 64) == 0);
    CK(rt_init(&b, 64, 64) == 0);
    CK(rt_init(&c, 64, 64) == 0);
    RDepthMask off = {0, 0};
    fill_scene(&a, scene, 3, NULL);   /* 旧接口 */
    fill_scene(&b, scene, 3, &off);   /* 新接口，掩码关闭 */
    CK(memcmp(a.z, b.z, sizeof(float) * 64 * 64) == 0);
    CK(memcmp(a.rgb, b.rgb, 64 * 64 * 3) == 0);
    for (int i = 0; i < 3; ++i) rt_triangle_ex(&c, &scene[i], NULL, i); /* mask=NULL */
    CK(memcmp(a.z, c.z, sizeof(float) * 64 * 64) == 0);
    CK(memcmp(a.rgb, c.rgb, 64 * 64 * 3) == 0);
    rt_free(&b); rt_free(&c);

    /* 掩码打开、write_depth=0：深度不写，颜色照写 */
    RTarget d;
    CK(rt_init(&d, 32, 32) == 0);
    RDepthMask nowrite = {1, 0};
    n = rt_triangle_ex(&d, &tri, &nowrite, 5);
    CK(n > 200);
    CK(d.z[16*32+16] > 1e29f);              /* 深度保持初始值 */
    CK(d.rgb[(16*32+16)*3] == 255);         /* 颜色已写 */
    /* 再来一个更远的三角形：深度没写，测试总过，颜色被覆盖 */
    RTri far_tri = {{ {4,4,0.9}, {28,4,0.9}, {16,28,0.9} }, {0, 255, 0}};
    n = rt_triangle_ex(&d, &far_tri, &nowrite, 6);
    CK(n > 200);
    CK(d.rgb[(16*32+16)*3+1] == 255);
    CK(d.z[16*32+16] > 1e29f);
    rt_free(&d);

    /* 掩码打开、write_depth=1：与旧路径一致 */
    RTarget e;
    CK(rt_init(&e, 64, 64) == 0);
    RDepthMask wr = {1, 1};
    fill_scene(&e, scene, 3, &wr);
    CK(memcmp(a.z, e.z, sizeof(float) * 64 * 64) == 0);
    CK(memcmp(a.rgb, e.rgb, 64 * 64 * 3) == 0);
    rt_free(&a); rt_free(&e);

    /* 非法掩码：报错 -4 */
    RTarget f;
    CK(rt_init(&f, 32, 32) == 0);
    RDepthMask bad1 = {2, 1};
    RDepthMask bad2 = {1, 7};
    RDepthMask bad3 = {-1, 0};
    CK(rt_triangle_ex(&f, &tri, &bad1, 0) == -4);
    CK(rt_triangle_ex(&f, &tri, &bad2, 1) == -4);
    CK(rt_triangle_ex(&f, &tri, &bad3, 2) == -4);
    rt_free(&f);

    /* 空缓冲：报错 -3 */
    RTarget g = {0, 0, NULL, NULL};
    CK(rt_triangle_ex(&g, &tri, NULL, 0) == -3);
    RTarget h = {8, 8, NULL, NULL};
    CK(rt_triangle_ex(&h, &tri, &off, 1) == -3);

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
