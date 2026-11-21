#include "raster.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

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

    /* 不启用色调：输出保持原样 */
    RTarget plain, off;
    CK(rt_init(&plain, 32, 32) == 0);
    CK(rt_init(&off, 32, 32) == 0);
    rt_triangle(&plain, &tri);
    CK(rt_triangle_tint(&off, &tri, NULL) == n);
    CK(memcmp(plain.rgb, off.rgb, 32*32*3) == 0);
    CK(memcmp(plain.z, off.z, 32*32*sizeof(float)) == 0);

    /* 第一段：按顶点色调解出系数；色调表、顶点号、插值系数均可取到 */
    double table[] = {1.0, 0.5, 0.25};
    RTint tint = { table, 3, {0, 1, 2} };
    RTintPlan plan;
    char err[128];
    CK(rt_tint_solve(&tri, &tint, &plan, err, sizeof err) == 0);
    CK(plan.vi[0] == 0 && plan.vi[1] == 1 && plan.vi[2] == 2);
    CK(plan.tint[0] == 1.0 && plan.tint[1] == 0.5 && plan.tint[2] == 0.25);
    CK(fabs(plan.a[0]*4  + plan.b[0]*4 + plan.c[0] - 1.0) < 1e-9); /* λ0 在 v0 处为 1 */
    CK(fabs(plan.a[0]*28 + plan.b[0]*4 + plan.c[0]) < 1e-9);       /* λ0 在 v1 处为 0 */

    /* 第二段：按重心坐标插值；插值权重可取到 */
    double w[3];
    rt_tint_weights(&plan, 4, 4, w);
    CK(fabs(w[0] - 1.0) < 1e-9 && fabs(w[1]) < 1e-9 && fabs(w[2]) < 1e-9);
    rt_tint_weights(&plan, 16, 16, w);
    CK(fabs(w[0] + w[1] + w[2] - 1.0) < 1e-9);
    CK(fabs(rt_tint_interp(&plan, w) - (w[0]*1.0 + w[1]*0.5 + w[2]*0.25)) < 1e-12);

    /* 第三段：乘到最终颜色（均匀色调 0.5 → 颜色减半） */
    double half[] = {0.5, 0.5, 0.5};
    RTint th = { half, 3, {0, 1, 2} };
    RTarget ht;
    CK(rt_init(&ht, 32, 32) == 0);
    CK(rt_triangle_tint(&ht, &tri, &th) == n);
    CK(ht.rgb[(16*32+16)*3] == 128);

    /* 非法值必须拒绝，并说明位置 */
    RTint bad_idx = { table, 3, {0, 7, 2} };
    CK(rt_tint_solve(&tri, &bad_idx, &plan, err, sizeof err) == -3);
    CK(strstr(err, "vertex 1") != NULL);
    double nanv[] = {1.0, NAN, 0.5};
    RTint bad_nan = { nanv, 3, {0, 1, 2} };
    CK(rt_tint_solve(&tri, &bad_nan, &plan, err, sizeof err) == -4);
    CK(strstr(err, "vertex 1") != NULL);
    double over[] = {1.0, 1.5, 0.5};
    RTint bad_over = { over, 3, {0, 1, 2} };
    CK(rt_tint_solve(&tri, &bad_over, &plan, err, sizeof err) == -4);
    CK(strstr(err, "vertex 1") != NULL);
    CK(rt_tint_solve(&flat, &tint, &plan, err, sizeof err) == -2);
    CK(rt_triangle_tint(&ht, &tri, &bad_over) == -4);

    /* 系数路径与暴力遍历对照 */
    RTarget fa, fb;
    CK(rt_init(&fa, 64, 64) == 0);
    CK(rt_init(&fb, 64, 64) == 0);
    RTri big = {{ {4,4,0.5}, {60,6,0.5}, {30,60,0.5} }, {200, 100, 50}};
    CK(rt_triangle_tint(&fa, &big, &tint) == rt_triangle_tint_bf(&fb, &big, &tint));
    int maxd = 0;
    for (int i = 0; i < 64*64*3; ++i) {
        int d = abs(fa.rgb[i] - fb.rgb[i]);
        if (d > maxd) maxd = d;
    }
    CK(maxd <= 1);

    rt_free(&t); rt_free(&plain); rt_free(&off); rt_free(&ht); rt_free(&fa); rt_free(&fb);
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
