#ifndef MIP_H
#define MIP_H
typedef struct {
    int levels;
    int w0, h0;
    unsigned char **pix;
    int *w, *h;
} MipChain;

/* 采样诊断：enable_center=0 时 mip_sample_ex 走老路径，输出与 mip_sample 字节一致 */
typedef struct {
    int enable_center;          /* 1=半纹素中心修正新路径；0=老路径 */
    int samples;                /* 累计采样调用次数 */
    int corrections;            /* 修正轴次数：raw 合法且与老路径坐标不同 */
    int oob;                    /* 越界样本次数（仅新路径检出） */
    int last_oob;               /* 最近一次调用是否检出越界 */
    int last_raw_x, last_raw_y; /* 最近一次的修正后纹素坐标（未 clamp） */
    int last_old_x, last_old_y; /* 最近一次老路径坐标（clamp 后，用于对照） */
    double last_ou, last_ov;    /* 最近一次使用的纹素中心偏移量 */
} MipDiag;

int mip_build(MipChain *c, const unsigned char *src, int w, int h);
void mip_free(MipChain *c);
int mip_sample(const MipChain *c, int level, double u, double v, unsigned char out[3]);
void mip_diag_init(MipDiag *d, int enable_center);
/* 返回 0=正常；1=新路径检出越界（out 仍以 clamp 后的安全纹素填充，不发生越界访问）；-1=参数错误 */
int mip_sample_ex(const MipChain *c, int level, double u, double v, unsigned char out[3], MipDiag *d);
#endif
