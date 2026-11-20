#ifndef MIP_H
#define MIP_H

typedef struct {
    int levels;
    int w0, h0;
    unsigned char **pix;
    int *w, *h;
} MipChain;

/* 错误码：拒绝时按参数区分，可用 mip_strerror 取参数名 */
#define MIP_OK        0
#define MIP_ERR_ARG   -1  /* 空指针或链未构建 */
#define MIP_ERR_LEVEL -2  /* level 越界 */
#define MIP_ERR_UV    -3  /* u/v 非有限 */
#define MIP_ERR_DUDX  -4  /* dudx 非有限 */
#define MIP_ERR_DVDX  -5  /* dvdx 非有限 */
#define MIP_ERR_DUDY  -6  /* dudy 非有限 */
#define MIP_ERR_DVDY  -7  /* dvdy 非有限 */

const char *mip_strerror(int err);

/* 各向异性子样本数硬上限：请求倍率再大也封顶，避免采样数爆炸 */
#define MIP_ANISO_CAP 16

typedef struct {
    int level;     /* 实际选用的层 */
    int nsamples;  /* 实际子样本数 */
} MipAnisoInfo;

int mip_build(MipChain *c, const unsigned char *src, int w, int h);
void mip_free(MipChain *c);
int mip_sample(const MipChain *c, int level, double u, double v, unsigned char out[3]);

/* 各向异性采样。
 * dudx/dvdx/dudy/dvdy：屏幕 x/y 方向上 uv 的一阶导数（像素足迹）。
 * 按导数算足迹长短轴：短轴长度选层，长宽比决定沿长轴的子样本数
 * （不超过 min(max_aniso, MIP_ANISO_CAP)），采样后平均合成。
 * max_aniso <= 1 时关闭：按各向同性（长轴）选层并单次采样，
 * 输出与 mip_sample(c, 该层, u, v) 完全相同。
 * info 可为 NULL。 */
int mip_sample_aniso(const MipChain *c, double u, double v,
                     double dudx, double dvdx, double dudy, double dvdy,
                     int max_aniso, unsigned char out[3], MipAnisoInfo *info);
#endif
