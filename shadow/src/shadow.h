#ifndef SHADOW_H
#define SHADOW_H

/* 阴影图：w*h 的浮点缓冲，每像素一个光照值（0 = 全阴影）。 */
typedef struct { float x, y, radius, intensity; } ShLight;
typedef struct { float x0, y0, x1, y1; } ShSeg;
typedef struct { int w, h; float *buf; } ShadowMap;

/* 抖动矩阵类型。 */
enum {
    SH_JITTER_NONE   = 0, /* 不抖动：每像素 1 个中心点采样，行为与旧版一致 */
    SH_JITTER_BAYER  = 1, /* 有序 Bayer 抖动（2x2/4x4/8x8） */
    SH_JITTER_ROTATED = 2, /* 旋转 45 度的 Bayer（Void-and-estimate 风格的简化版） */
    SH_JITTER_CUSTOM = 3  /* 调用方提供矩阵 */
};

/* 采样档位：低速（少样本）与高速（多样本，质量优先）。 */
enum {
    SH_TIER_FAST = 0, /* 低速档：2x2 = 4 采样点 */
    SH_TIER_HIGH = 1  /* 高速档：8x8 = 64 采样点 */
};

typedef struct {
    int enabled;       /* 开关：为假时忽略其余字段，走旧版单点路径 */
    int matrix_type;   /* SH_JITTER_* */
    int tier;          /* SH_TIER_*，决定每像素采样点数 */
    int tile;          /* 矩阵边长（只读输出/自定义输入）：2、4、8 */
    const float *mat;  /* 仅 SH_JITTER_CUSTOM 使用：tile*tile 个 [0,1) 偏移 */
} ShJitterCfg;

int sh_init(ShadowMap *m, int w, int h);
void sh_free(ShadowMap *m);
int sh_clear(ShadowMap *m);

/* 旧接口保留：等价于 enabled=0 的 sh_render_jit，逐像素中心单点采样。 */
int sh_render(ShadowMap *m, const ShLight *light, const ShSeg *segs, int nseg);

/* 分两段的抖动渲染：
 * 1) 按抖动矩阵把每像素的规则采样点扰动到子像素位置；
 * 2) 对扰动后的采样点逐个判定遮挡并平均，得到软阴影值。
 * out_nsamp 可为 NULL，否则回传实际每像素采样点数。 */
int sh_render_jit(ShadowMap *m, const ShLight *light, const ShSeg *segs,
                  int nseg, const ShJitterCfg *cfg, int *out_nsamp);

/* 校验抖动配置；冲突时返回负值并把冲突两方名字写入 err（容量 errcap）。 */
int sh_jitter_validate(const ShJitterCfg *cfg, char *err, int errcap);

/* 生成/导出抖动矩阵。生成的矩阵值落在 [0,1)，按行主序存放。 */
int sh_jitter_build(int matrix_type, int tile, float *out);
int sh_jitter_export(const char *path, int matrix_type, int tile, const float *mat);

int sh_write_pgm(const ShadowMap *m, const char *path);
#endif
