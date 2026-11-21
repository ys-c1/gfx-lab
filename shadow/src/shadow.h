#ifndef SHADOW_H
#define SHADOW_H
typedef struct { float x, y, radius, intensity; } ShLight;
typedef struct { float x0, y0, x1, y1; } ShSeg;
typedef struct { int w, h; float *buf; } ShadowMap;

typedef enum { SH_T_INT = 1, SH_T_FLOAT = 2 } ShType;
typedef struct { ShType t; int i; float f; } ShValue;

typedef struct {
    int segmented;   /* 0: 逐像素旧路径（默认，逐字节一致）; 1: 三段式 */
    int block;       /* 归约块边长（像素，>=1）；1 为无损 */
    float threshold; /* 合成阈值，作用于归约后的占用值，[0,1] */
} ShConfig;

typedef struct {
    int segments;        /* 段数 */
    long reductions;     /* 归约次数（块级归约） */
    long local_bytes;    /* 阶段一局部缓冲峰值字节数 */
    long reduced_bytes;  /* 阶段二归约网格总字节数 */
    double pre_post_diff;/* 归约前后差异：sum |local - 归约展开值| */
    long pre_post_cells; /* 归约前后有差异的像素数 */
} ShStats;

int sh_init(ShadowMap *m, int w, int h);
void sh_free(ShadowMap *m);
int sh_clear(ShadowMap *m);
int sh_render(ShadowMap *m, const ShLight *light, const ShSeg *segs, int nseg);

void sh_config_default(ShConfig *c);
/* 0 成功；-1 未知字段；-2 类型不符（*bad_field 指向字段名）；-3 取值越界 */
int sh_config_set(ShConfig *c, const char *field, ShValue v, const char **bad_field);
/* cfg 为 NULL 或 cfg->segmented==0 时与 sh_render 逐字节相同 */
int sh_render_ex(ShadowMap *m, const ShLight *light, const ShSeg *segs, int nseg,
                 const ShConfig *cfg, ShStats *stats);

int sh_write_pgm(const ShadowMap *m, const char *path);
#endif
