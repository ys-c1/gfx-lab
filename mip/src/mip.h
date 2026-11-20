#ifndef MIP_H
#define MIP_H

typedef struct {
    int levels;
    int w0, h0;
    unsigned char **pix; /* 编码空间金字塔(gamma == 0) */
    float **lin;         /* 线性空间金字塔(gamma > 0) */
    double gamma;        /* 0 = 不转空间,保持旧行为 */
    int *w, *h;
} MipChain;

enum {
    MIP_OK = 0,
    MIP_ERR_ARG = -1,    /* 空指针或尺寸非法 */
    MIP_ERR_LEVELS = -2, /* 层数为零 */
    MIP_ERR_GAMMA = -3,  /* gamma 非法 */
    MIP_ERR_LEVEL = -4   /* 需求层号越界 */
};

typedef struct {
    int code;            /* MIP_OK 或上面的错误码 */
    const char *field;   /* 出错字段名:"gamma" / "levels" / "level" 等,无错为 NULL */
} MipError;

/* 层链:一次采样实际走过的相邻两层与权重 */
typedef struct {
    int lo;      /* 低层号 */
    int hi;      /* 高层号 */
    double t;    /* lo 与 hi 之间的插值权重 */
    int samples; /* 本次纹素读取次数 */
} MipTrace;

int mip_build(MipChain *c, const unsigned char *src, int w, int h);
/* gamma == 0:与 mip_build 完全一致;gamma > 0:先按 gamma 解码到线性再建金字塔 */
int mip_build_ex(MipChain *c, const unsigned char *src, int w, int h,
                 double gamma, MipError *err);
void mip_free(MipChain *c);

int mip_sample(const MipChain *c, int level, double u, double v, unsigned char out[3]);
/* 三段式:线性层链取相邻两层 -> 按需求层号插值 -> 编回。
   level 可为小数(如倍率 1.5 对应 log2(1.5))。
   gamma == 0 的链输出与 mip_sample 一致。trace / err 可为 NULL。 */
int mip_sample_ex(const MipChain *c, double level, double u, double v,
                  unsigned char out[3], MipTrace *trace, MipError *err);

#endif
