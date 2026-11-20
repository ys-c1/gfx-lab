#ifndef MIP_H
#define MIP_H
typedef struct {
    int levels;
    int w0, h0;
    unsigned char **pix;
    int *w, *h;
} MipChain;
int mip_build(MipChain *c, const unsigned char *src, int w, int h);
void mip_free(MipChain *c);
int mip_sample(const MipChain *c, int level, double u, double v, unsigned char out[3]);

/* ---- 放大锐化：三段流水线 ----
 * 双线性放大会把边缘抹平。这里把第 level 层与其下一层（更糊）的差
 * 作为丢失的细节带，按可配权重加回：
 *   段1 mip_fetch_pair    按层号取相邻两层
 *   段2 mip_recalc_sharp  双线性放大两层，按 weight 叠加细节带（weight==0 即纯双线性）
 *   段3 mip_composite     浮点结果夹取合成到 8 位，并给出锐度指标（平均梯度幅值）
 */

/* 错误码：mip_err_field() 把错误码映射为出错的字段名 */
#define MIP_E_ARG    -1  /* 空指针 / 尺寸非法等一般参数错误 */
#define MIP_E_WEIGHT -2  /* weight 字段非法（为负） */
#define MIP_E_LEVEL  -3  /* level 字段非法（越界，需 0 <= level < levels-1） */
const char *mip_err_field(int rc);

typedef struct {
    const unsigned char *lo; /* 第 level 层像素 */
    const unsigned char *hi; /* 第 level+1 层像素（更糊） */
    int lw, lh;              /* lo 尺寸 */
    int hw, hh;              /* hi 尺寸 */
} MipLevelPair;

/* 段1：按层号取相邻两层。level 越界返回 MIP_E_LEVEL。 */
int mip_fetch_pair(const MipChain *c, int level, MipLevelPair *pair);

/* 段2：重算。accum 为 out_w*out_h*3 的浮点缓冲；samples 累计纹素采样次数（可 NULL）。
 * weight < 0 返回 MIP_E_WEIGHT；weight == 0 时退化为纯双线性，输出与原样一致。 */
int mip_recalc_sharp(const MipLevelPair *pair, double weight,
                     int out_w, int out_h, double *accum,
                     unsigned long *samples);

/* 段3：合成。sharpness 为输出图的平均梯度幅值（0..255，可 NULL）。 */
int mip_composite(const double *accum, int w, int h,
                  unsigned char *out, double *sharpness);

typedef struct {
    double sharpness;      /* 输出图锐度：平均梯度幅值 */
    unsigned long samples; /* 纹素采样次数 */
} MipUpscaleStats;

/* 三段合一的便捷入口 */
int mip_upscale(const MipChain *c, int level, double weight,
                int out_w, int out_h, unsigned char *out,
                MipUpscaleStats *stats);

/* 手工反卷积对照：双线性放大后做 Van Cittert 迭代，
 * 模糊模型取 mip 降采样的 2x2 盒式核。level 越界返回 MIP_E_LEVEL。 */
int mip_deconv(const MipChain *c, int level, int iterations,
               int out_w, int out_h, unsigned char *out,
               MipUpscaleStats *stats);
#endif
