#ifndef SHADOW_H
#define SHADOW_H
#include <stddef.h>
typedef struct { float x, y, radius, intensity; } ShLight;
typedef struct { float x0, y0, x1, y1; } ShSeg;
typedef struct { int w, h; float *buf; } ShadowMap;
int sh_init(ShadowMap *m, int w, int h);
void sh_free(ShadowMap *m);
int sh_clear(ShadowMap *m);
int sh_render(ShadowMap *m, const ShLight *light, const ShSeg *segs, int nseg);
/* 带偏移渲染：等价于把光源平移 (ox, oy)。ox=oy=0 时与 sh_render 逐位一致。 */
int sh_render_off(ShadowMap *m, const ShLight *light, const ShSeg *segs, int nseg,
                  float ox, float oy);
int sh_write_pgm(const ShadowMap *m, const char *path);

/* ---- 时序稳定：三段式偏移抑制（段1 平滑 → 段2 斜率修正 → 段3 跳变统计） ---- */
typedef struct {
    float k;       /* 平滑系数，[0,1]：0=完全沿用前帧，1=不平滑 */
    float slope_k; /* 斜率修正系数，>= 0 */
    int   suppress;/* 0=不抑制（输出=原始偏移，结果与未启用相同），1=启用三段处理 */
} ShStabCfg;

typedef struct {
    float raw_x, raw_y; /* 本帧原始偏移 */
    float out_x, out_y; /* 三段处理后的输出偏移 */
    float jump;         /* 本帧跳变量 |raw - 前帧输出| */
} ShStabSample;

typedef struct {
    int has_prev;         /* 前帧偏移是否有效 */
    float prev_x, prev_y; /* 前帧输出偏移 */
    unsigned long n_jump; /* 已统计的跳变样本数 */
    double jump_sum, jump_max, jump_last;
    ShStabSample *curve;  /* 跳变曲线（逐帧样本，可导出） */
    size_t curve_len, curve_cap;
} ShStab;

typedef struct {
    unsigned long frames; /* 跳变样本数 */
    double mean, max, last;
} ShStabStats;

#define SH_STAB_OK       0
#define SH_STAB_E_ARG   -1 /* 参数为空 */
#define SH_STAB_E_K     -2 /* 字段 k：平滑系数越界 [0,1] */
#define SH_STAB_E_SLOPE -3 /* 字段 slope_k：斜率修正系数越界（要求 >= 0） */
#define SH_STAB_E_PREV  -4 /* 字段 prev：前帧偏移缺失（先 sh_stab_seed） */
#define SH_STAB_E_IO    -5 /* 导出失败 */
#define SH_STAB_E_MEM   -6 /* 跳变曲线内存不足 */

int sh_stab_init(ShStab *s);
void sh_stab_free(ShStab *s);
/* 设定首帧偏移；启用抑制而未 seed 时 sh_stab_frame 报 SH_STAB_E_PREV */
int sh_stab_seed(ShStab *s, float x, float y);
int sh_stab_frame(ShStab *s, const ShStabCfg *cfg,
                  float raw_x, float raw_y,
                  float slope_x, float slope_y,
                  ShStabSample *out);
int sh_stab_stats(const ShStab *s, ShStabStats *out);
/* 导出跳变曲线 CSV：frame,raw_x,raw_y,out_x,out_y,jump */
int sh_stab_export_csv(const ShStab *s, const char *path);
/* 错误串，指出出错字段 */
const char *sh_stab_err(int rc);
#endif
