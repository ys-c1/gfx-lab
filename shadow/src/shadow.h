#ifndef SHADOW_H
#define SHADOW_H

typedef struct { float x, y, radius, intensity; } ShLight;
typedef struct { float x0, y0, x1, y1; } ShSeg;
typedef struct { int w, h; float *buf; } ShadowMap;

/* 错误码：每个非法字段有独立码，sh_err_field 给出字段名 */
enum {
    SH_OK = 0,
    SH_ERR_ARG = -1,     /* 空指针 / 尺寸非法 */
    SH_ERR_RADIUS = -2,  /* radius 字段 <= 0 */
    SH_ERR_SIZE = -3,    /* size 字段 <= 0（面光源） */
    SH_ERR_SAMPLES = -4  /* samples 字段 <= 0（面光源） */
};
const char *sh_err_field(int rc);

int sh_init(ShadowMap *m, int w, int h);
void sh_free(ShadowMap *m);
int sh_clear(ShadowMap *m);
int sh_render(ShadowMap *m, const ShLight *light, const ShSeg *segs, int nseg);
int sh_write_pgm(const ShadowMap *m, const char *path);

/* 面光源：x,y 为圆盘中心；radius 为衰减半径（与 ShLight 同义）；
 * size 为发光圆盘半径，samples 为圆盘上的采样点数。 */
typedef struct {
    float x, y;
    float radius;
    float intensity;
    float size;
    int samples;
} ShAreaLight;

/* 第一段：按光源面积采多个点（向日葵螺旋，确定性分布）。
 * xy 输出 2*min(cap, samples) 个 float；n 返回总采样数（可大于 cap）。 */
int sh_area_points(const ShAreaLight *l, float *xy, int cap, int *n);
/* 第二段：逐像素统计可见采样点，按覆盖比例合成亮度。 */
int sh_render_area(ShadowMap *m, const ShAreaLight *l, const ShSeg *segs, int nseg);
/* 第三段：量半影带宽（部分遮挡像素的最大连续段，单位像素）。 */
int sh_penumbra_width(const ShadowMap *m, const ShAreaLight *l, float *width_px);

#endif
