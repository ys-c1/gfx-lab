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

/* 单次三线性采样的诊断信息：层号、混合权重、两层各自的双线性颜色、触到的纹素数 */
typedef struct {
    int level_lo;          /* 较清晰的一层 */
    int level_hi;          /* 较模糊的一层（blend 关闭时等于 level_lo） */
    double weight;         /* 实际使用的 hi 层权重，[0,1] */
    unsigned char lo[3];   /* level_lo 上的双线性结果 */
    unsigned char hi[3];   /* level_hi 上的双线性结果 */
    int texel_reads;       /* 本次采样读取的纹素数 */
} MipTrace;

/* 由屏幕空间 UV 导数求 LOD。导数非有限时返回 -1.0 并把 err_field 置为
   "dudx"/"dudy"/"dvdx"/"dvdy"；成功返回 [0, levels-1] 内的 LOD。 */
double mip_lod_from_derivs(const MipChain *c,
                           double dudx, double dudy, double dvdx, double dvdy,
                           const char **err_field);

/* 把连续 LOD 拆成 (level_lo, weight)。lod 非有限返回 -1，err_field="lod"。 */
int mip_lod_split(const MipChain *c, double lod, int *level_lo, double *weight,
                  const char **err_field);

/* 三线性采样：在 level_lo 与 level_lo+1 上各做一次双线性，再按 weight 混合。
   blend==0 时走与 mip_sample 完全相同的最近层最近点路径（逐像素一致）。
   校验失败返回 -1，err_field 指明字段：
   "level_lo" 层号越界；"weight" 权重越界或非有限；
   "levels" 层数不足（weight>0 但不存在 level_lo+1）；"u"/"v" 坐标非有限。 */
int mip_sample_trilinear(const MipChain *c, int level_lo, double weight,
                         double u, double v, int blend,
                         unsigned char out[3], MipTrace *trace,
                         const char **err_field);
#endif
