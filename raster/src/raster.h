#ifndef RASTER_H
#define RASTER_H
typedef struct { double x, y, z; } RVert;
typedef struct { RVert v[3]; unsigned char rgb[3]; } RTri;
typedef struct { int w, h; float *z; unsigned char *rgb; } RTarget;

/* 深度写掩码。enabled=0：掩码关闭，行为与 rt_triangle 完全一致（测试+写入一体）；
 * enabled=1：先判深度测试，再由 write_depth 决定写不写深度（颜色照常写）。
 * 字段只能取 0/1，其余值视为非法掩码。 */
typedef struct { int enabled; int write_depth; } RDepthMask;

int rt_init(RTarget *t, int w, int h);
void rt_free(RTarget *t);
int rt_triangle(RTarget *t, const RTri *tri);
/* 两段式光栅化。tri_id 用于报错信息，指明是哪个三角形。
 * 返回 >=0：写入颜色的像素数；负值为错误码：
 *  -1 参数为空  -2 退化三角形  -3 目标缓冲为空  -4 非法掩码 */
int rt_triangle_ex(RTarget *t, const RTri *tri, const RDepthMask *mask, int tri_id);
int rt_write_ppm(const RTarget *t, const char *path);
#endif
