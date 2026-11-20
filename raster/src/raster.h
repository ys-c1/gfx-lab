#ifndef RASTER_H
#define RASTER_H
#include <stddef.h>

typedef struct { double x, y, z; } RVert;
typedef struct { RVert v[3]; unsigned char rgb[3]; } RTri;
typedef struct { int w, h; float *z; unsigned char *rgb; } RTarget;

/* 返回码：>=0 写入/通过像素数；-1 空指针；-2 退化三角形；
   -3 深度非有限值（rt_render 时在 st 里另记出错三角形号）；
   -4 目标缓冲为空（z/rgb 为 NULL）*/
int rt_init(RTarget *t, int w, int h);
void rt_free(RTarget *t);
int rt_triangle(RTarget *t, const RTri *tri);
int rt_write_ppm(const RTarget *t, const char *path);

/* 第一阶段：只读深度探测，不修改缓冲。
   返回 >=0 = 能通过深度测试的像素数（0 表示整个三角形被遮挡），
   <0 = 错误码（见上）。*/
int rt_tri_visible(const RTarget *t, const RTri *tri);

typedef struct {
    int tris_total;       /* 处理过的三角形数（含退化与早退）*/
    int tris_early_out;   /* 深度探测为 0、跳过着色的三角形数 */
    int tris_degenerate;  /* 面积为 0 被跳过的三角形数 */
    long pixels_inside;   /* 通过边测试的像素总数（探测+着色两阶段口径之和）*/
    long pixels_probed;   /* 探测阶段遍历的边测试通过像素数（仅早退模式）*/
    long pixels_shaded;   /* 着色阶段遍历的边测试通过像素数 */
    long pixels_filled;   /* 实际着色写入的像素总数 */
    double elapsed_sec;   /* 仅 rt_render 计时范围，其他入口为 0 */
    int error_index;      /* 出错三角形号，-1 表示无错 */
} RStats;

void rt_stats_init(RStats *st);

/* 两阶段驱动：先对每个三角形做深度探测，再只给通过者着色。
   enable_early=0 时退化为逐三角形原路径，输出与 rt_triangle
   逐像素完全相同（z 缓冲与 rgb 缓冲均可逐字节比对）。*/
int rt_render(RTarget *t, const RTri *tris, int n, int enable_early, RStats *st);
#endif
