#ifndef MIP_H
#define MIP_H

/* 寻址模式：先把 (u,v) 按模式换算到 [0,1)，再用换算结果采样。 */
enum {
    MIP_ADDR_CLAMP  = 1, /* 夹到边：t<0 -> 0, t>=1 -> 下一整数前的最大值 */
    MIP_ADDR_WRAP   = 2, /* 重复平铺：t - floor(t) */
    MIP_ADDR_MIRROR = 3  /* 镜像平铺：偶数段正向、奇数段反向 */
};

typedef struct {
    int mode;       /* MIP_ADDR_* 之一 */
} MipAddr;

typedef struct {
    int enabled;        /* 0 = 关闭寻址，走与旧版逐像素相同的夹边路径 */
    const MipAddr *addr;/* 寻址表，两项：[0]=u 轴 [1]=v 轴；enabled 时必须非空且模式合法 */
} MipSampler;

typedef struct {
    int levels;
    int w0, h0;
    unsigned char **pix;
    int *w, *h;
} MipChain;

int mip_build(MipChain *c, const unsigned char *src, int w, int h);
void mip_free(MipChain *c);

/* 旧接口：固定夹边，行为与历史版本逐像素一致。 */
int mip_sample(const MipChain *c, int level, double u, double v, unsigned char out[3]);

/* 第一步：按寻址表把一个轴的纹理坐标换算到 [0,1)。
 * tri_id 仅用于报错时指明是哪个三角形。返回 0 成功，<0 为错误码。 */
int mip_address(const MipSampler *s, int axis, double t, int tri_id, double *out_t);

/* 第二步：先按寻址表换算 (u,v)，再按换算结果采样。
 * s 为 NULL 或 enabled==0 时关闭寻址，结果与 mip_sample 完全相同。
 * tri_id 仅用于报错时指明是哪个三角形。返回 0 成功，<0 为错误码。 */
int mip_sample_ex(const MipChain *c, int level, const MipSampler *s,
                  double u, double v, int tri_id, unsigned char out[3]);

/* 错误码 */
#define MIP_ERR_LEVEL   (-1) /* level 非法 */
#define MIP_ERR_MODE    (-2) /* 寻址模式非法 */
#define MIP_ERR_NONFIN  (-3) /* 纹理坐标为非有限值（NaN/Inf） */
#define MIP_ERR_NOTABLE (-4) /* 寻址已开启但寻址表缺失 */
#define MIP_ERR_ARG     (-5) /* 空指针等参数错误 */

#endif
