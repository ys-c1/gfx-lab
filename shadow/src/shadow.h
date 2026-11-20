#ifndef SHADOW_H
#define SHADOW_H
typedef struct { float x, y, radius, intensity; } ShLight;
typedef struct { float x0, y0, x1, y1; } ShSeg;
typedef struct { float x0, y0, x1, y1, tau; } ShSegT; /* tau: 透光率 [0,1] */
typedef struct { int w, h; float *buf; } ShadowMap;

/* 透光渲染统计：重叠区（>=2 层遮挡）单独统计 */
typedef struct {
    int n_lit;            /* 无遮挡像素数 */
    int n_single;         /* 恰好 1 层遮挡的像素数 */
    int n_overlap;        /* >=2 层遮挡（重叠区）像素数 */
    double sum_single;    /* 单层遮挡区亮度之和 */
    double sum_overlap;   /* 重叠区亮度之和 */
    double mean_single;   /* 单层区平均亮度 */
    double mean_overlap;  /* 重叠区平均亮度 */
    float max_trans_err;  /* 透光率累乘误差：float 累乘 vs double 累乘的最大偏差 */
} ShStats;

/* 错误码：透光率越界 / 遮挡段退化，均编码出错段序号 i */
#define SH_ERR_TAU(i)   (-1000 - (i))
#define SH_ERR_DEGEN(i) (-2000 - (i))

int sh_init(ShadowMap *m, int w, int h);
void sh_free(ShadowMap *m);
int sh_clear(ShadowMap *m);
int sh_render(ShadowMap *m, const ShLight *light, const ShSeg *segs, int nseg);
/* 两段式透光阴影：段1 沿光线按遮挡体透光率累乘，段2 合成阴影亮度并统计。
   全部遮挡体不透光（tau=0）时与 sh_render 逐像素一致。 */
int sh_render_t(ShadowMap *m, const ShLight *light, const ShSegT *segs, int nseg, ShStats *st);
int sh_write_pgm(const ShadowMap *m, const char *path);
#endif
