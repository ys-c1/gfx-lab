#ifndef RASTER_H
#define RASTER_H
#include <stddef.h>
typedef struct { double x, y, z; } RVert;
typedef struct { RVert v[3]; unsigned char rgb[3]; } RTri;
typedef struct { int w, h; float *z; unsigned char *rgb; } RTarget;

/* 顶点色调输入：色调表 + 三个顶点号 */
typedef struct {
    const double *table; /* 色调表，取值须在 [0,1] */
    size_t n;            /* 表长 */
    int vi[3];           /* 顶点号：三个顶点在色调表中的下标 */
} RTint;

/* 第一段产物：按顶点色调解出的插值系数 */
typedef struct {
    double a[3], b[3], c[3]; /* 重心基函数 λ_i(x,y) = a_i*x + b_i*y + c_i */
    double tint[3];          /* 三个顶点的色调（按顶点号取自色调表） */
    int vi[3];               /* 顶点号（回读用） */
} RTintPlan;

int rt_init(RTarget *t, int w, int h);
void rt_free(RTarget *t);
int rt_triangle(RTarget *t, const RTri *tri);
/* 带顶点色调的光栅化；tint 为 NULL 时不启用，输出与 rt_triangle 完全一致 */
int rt_triangle_tint(RTarget *t, const RTri *tri, const RTint *tint);
/* 暴力遍历参考实现：每像素重算重心坐标，用于对照 */
int rt_triangle_tint_bf(RTarget *t, const RTri *tri, const RTint *tint);

/* 第一段：校验并按顶点色调解出插值系数。
   返回 0 成功；负值失败，err（若非 NULL）写明非法值位置。
   -1 空参数/空表  -2 退化三角形  -3 顶点号越界  -4 色调值非法 */
int rt_tint_solve(const RTri *tri, const RTint *tint, RTintPlan *plan, char *err, size_t errcap);
/* 第二段：按重心坐标取插值权重，再加权出色调 */
void rt_tint_weights(const RTintPlan *plan, double px, double py, double w[3]);
double rt_tint_interp(const RTintPlan *plan, const double w[3]);

int rt_write_ppm(const RTarget *t, const char *path);
#endif
