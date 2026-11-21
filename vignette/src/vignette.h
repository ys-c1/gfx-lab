#ifndef VIGNETTE_H
#define VIGNETTE_H

/* 两段式暗角：第一段按到中心距离算权重，第二段按权重压暗。 */

typedef struct {
    float strength; /* 暗角强度 [0,1]，角落最大压暗比例 */
    float inner;    /* 内半径(归一化) [0,1)，内圈不压暗 */
    float outer;    /* 外半径(归一化) (inner,1]，此外达到最大压暗 */
    float gamma;    /* 权重曲线指数，>0 且有限 */
    int lut_size;   /* 权重表长度 [2,4096]，供快速路径使用 */
    int enabled;    /* 0=不启用(输出原样) 1=启用 */
} VignetteCfg;

typedef struct {
    float strength;   /* 实际生效的暗角强度(未启用为 0) */
    int edge_pixels;  /* 边缘像素数：权重 < 1-1e-6 的像素个数 */
    float min_weight; /* 最小权重(画面最暗处保留的亮度比例) */
} VignetteStats;

/* 校验配置。返回 0 合法；-1 表示 cfg 为空；
 * 否则返回非法字段的位置序号(1=strength 2=inner 3=outer 4=gamma
 * 5=lut_size 6=enabled)，字段名经 where 返回(可为 NULL)。 */
int vg_validate(const VignetteCfg *cfg, const char **where);

/* 第一段：按到中心距离算权重，d=0 为中心，d=1 为角落。 */
/* 暴力参考：逐像素精确求曲线(smoothstep + pow)。 */
int vg_weights_ref(const VignetteCfg *cfg, int w, int h, float *out);
/* 快速路径：先建 LUT，再逐像素查表线性插值。 */
int vg_lut_build(const VignetteCfg *cfg, float *lut, int n);
int vg_weights_fast(const VignetteCfg *cfg, int w, int h,
                    const float *lut, int n, float *out);

/* 第二段：按权重压暗 out = in * (1 - strength*(1-weight))。
 * 未启用时忽略 weights(可传 NULL)，out 与 in 逐字节一致(in/out 可重叠)。 */
int vg_apply(const VignetteCfg *cfg, int w, int h,
             const float *weights, const float *in, float *out);

/* 统计：强度、边缘像素数、最小权重。 */
int vg_stats(const VignetteCfg *cfg, int w, int h,
             const float *weights, VignetteStats *st);

/* 导出权重曲线表 CSV：t,exact,lut 共 rows 行采样。 */
int vg_export_curve(const VignetteCfg *cfg, const char *path, int rows);

#endif
