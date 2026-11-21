#ifndef RASTER_H
#define RASTER_H
typedef struct { double x, y, z; } RVert;
typedef struct { RVert v[3]; unsigned char rgb[3]; } RTri;
typedef struct { int w, h; float *z; unsigned char *rgb; } RTarget;
int rt_init(RTarget *t, int w, int h);
void rt_free(RTarget *t);
int rt_triangle(RTarget *t, const RTri *tri);
int rt_write_ppm(const RTarget *t, const char *path);

/* 画线参数。坐标为连续像素坐标，像素 (i,j) 中心在 (i+0.5, j+0.5)。 */
#define RLINE_W_MIN 0.5
#define RLINE_W_MAX 64.0
#define RLINE_LEVELS_MIN 2
#define RLINE_LEVELS_MAX 256
typedef struct {
    double x0, y0, x1, y1;   /* 端点，须为有限值 */
    double width;            /* 线宽（像素），[RLINE_W_MIN, RLINE_W_MAX] */
    int levels;              /* 覆盖率分档数，[RLINE_LEVELS_MIN, RLINE_LEVELS_MAX]，常用 32/256 */
    int aa;                  /* 0=单像素基线（与旧画面逐像素一致），1=两段式抗锯齿 */
    unsigned char rgb[3];
    char err[32];            /* 出参：参数越界时写入字段名，如 "width" */
} RLine;
/* 返回覆盖像素数（>=0）；-1 参数越界（ln->err 指明字段），-2 空指针。
 * aa=0 时忽略 width/levels 的视觉效果（仍校验），逐像素等同基线 Bresenham。
 * aa=1 时两段式：先按斜率求垂直覆盖宽度 wcov = width*len/max(|dx|,|dy|)，
 * 再按像素覆盖率分成 levels 档档亮度，取 max 叠加，不写 z 缓冲。 */
int rt_line(RTarget *t, RLine *ln);
#endif
