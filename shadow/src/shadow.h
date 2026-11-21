#ifndef SHADOW_H
#define SHADOW_H
typedef struct { float x, y, radius, intensity; } ShLight;
typedef struct { float x0, y0, x1, y1; } ShSeg;
typedef struct { int w, h; float *buf; } ShadowMap;
int sh_init(ShadowMap *m, int w, int h);
void sh_free(ShadowMap *m);
int sh_clear(ShadowMap *m);
int sh_render(ShadowMap *m, const ShLight *light, const ShSeg *segs, int nseg);
int sh_write_pgm(const ShadowMap *m, const char *path);

/* ---- 环境光染色阴影 ----
 * 分三段：1) sh_tint_shadow_color 按环境色算阴影色
 *         2) sh_tint_blend       按遮挡比例混色
 *         3) sh_tint_write       写回缓冲
 * cfg->enabled 为 0 时输出与原灰度行为完全一致（rgb 三通道 = 原亮度）。 */
typedef struct { float r, g, b; } ShColor;

enum { SH_TINT_EXACT = 0, SH_TINT_FAST = 1 }; /* 低速(穷举) / 高速(色表) */
#define SH_TINT_TABLE_SIZE 257

typedef struct {
    int enabled;          /* 开关：0 时行为不变 */
    int mode;             /* SH_TINT_EXACT 或 SH_TINT_FAST */
    int has_ambient;      /* 非 0：阴影色按 ambient 环境色计算 */
    ShColor ambient;
    int has_shadow_color; /* 非 0：使用固定阴影色；与 has_ambient 冲突 */
    ShColor shadow_color;
} ShTintConfig;

typedef struct {
    long pixels;           /* 像素数 */
    double blend_ratio;    /* 混合比例：全图平均遮挡比例 [0,1] */
    double tint_error;     /* 染色误差：高速档相对穷举结果的平均绝对误差 */
    double time_exact_ms;  /* 低速档耗时 */
    double time_fast_ms;   /* 高速档耗时 */
    long ambient_changes;  /* 环境色变化次数（单独统计） */
    long table_rebuilds;   /* 色表重建次数 */
} ShTintStats;

typedef struct {
    ShColor table[SH_TINT_TABLE_SIZE]; /* 色表：table[i] = 阴影色 * (i/256) */
    ShColor table_color;               /* 建表时用的阴影色 */
    int table_valid;
    int has_ambient;
    ShColor ambient;                   /* 上一次见到的环境色 */
    long ambient_changes;
    long table_rebuilds;
} ShTintState;

int sh_tint_init(ShTintState *st);
/* 取色表；n 返回表项数（SH_TINT_TABLE_SIZE），未建表时返回 NULL */
const ShColor *sh_tint_table(const ShTintState *st, int *n);

/* 三段流水（可单独调用，sh_tint_render 内部也按此顺序执行） */
ShColor sh_tint_shadow_color(const ShTintConfig *cfg);         /* 段一 */
ShColor sh_tint_blend(ShColor lit, ShColor shadow, float occ); /* 段二 */
void sh_tint_write(float *rgb_out, long idx, ShColor c);       /* 段三 */

/* 需先 sh_render 填好 m->buf（作为受光亮度）。rgb_out 为 3*w*h  floats。
 * 返回 0 成功；-1 参数空；-2 光参数非法；
 * -3 配置冲突（ambient 与 shadow_color 同时给出，stderr 指明两方）；
 * -4 mode 非法。stats 可为 NULL。 */
int sh_tint_render(ShTintState *st, const ShadowMap *m, const ShLight *light,
                   const ShSeg *segs, int nseg, const ShTintConfig *cfg,
                   float *rgb_out, ShTintStats *stats);
/* 同一场景跑低速+高速两档，与穷举结果核对，填 tint_error 与两档耗时 */
int sh_tint_compare(ShTintState *st, const ShadowMap *m, const ShLight *light,
                    const ShSeg *segs, int nseg, const ShTintConfig *cfg,
                    ShTintStats *stats);
#endif
