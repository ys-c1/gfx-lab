#ifndef SHARPEN_H
#define SHARPEN_H

/* 两段式贴图锐化, 解决放大后边缘发虚:
 *   第一段 mip_sharpen_amount —— 按亮度梯度计算逐像素锐化量;
 *   第二段 mip_sharpen_apply  —— 按锐化量做无振铃增强(3x3 局部 min/max 钳制)。
 * 默认参数(strength = 0)下输出与输入逐字节一致, 即与未锐化的当前版本一致。 */

typedef struct {
    double strength;  /* 锐化强度, 0 = 关闭(默认) */
    double grad_lo;   /* 梯度下限, 低于此值不锐化 */
    double grad_hi;   /* 梯度上限, 达到此值锐化量饱和, 必须 > grad_lo */
} MipSharpenParams;

#define MIP_SHARPEN_DEFAULT_INIT { 0.0, 8.0, 64.0 }

typedef struct {
    double overshoot;      /* 过冲量: 未钳制增强超出局部 [min,max] 的总量 */
    double strength_mean;  /* 实际应用的平均锐化量 */
    double strength_max;   /* 实际应用的最大锐化量 */
    long edge_pixels;      /* 边缘像素数: 锐化量 > 0 的像素 */
    long ringing_pixels;   /* 振铃像素数: 未钳制时越出局部范围的像素(单独统计) */
} MipSharpenStats;

MipSharpenParams mip_sharpen_default(void);

/* 返回值: 0 成功; -1 空指针或尺寸不合法; -3 参数不合法 */
int mip_sharpen_amount(const unsigned char *src, int w, int h,
                       const MipSharpenParams *p, float *amount_out);
int mip_sharpen_apply(const unsigned char *src, const float *amount, int w, int h,
                      unsigned char *dst, MipSharpenStats *stats);
int mip_sharpen(const unsigned char *src, int w, int h,
                const MipSharpenParams *p, unsigned char *dst, MipSharpenStats *stats);

/* 批处理: 第 i 项输入不合法时返回 -2 并由 bad_index 标出索引 i */
int mip_sharpen_batch(int n, const unsigned char *const *srcs,
                      const int *ws, const int *hs,
                      const MipSharpenParams *p, unsigned char *const *dsts,
                      int *bad_index);

/* 参考实现: 直接按定义逐像素朴素计算, 用于逐项核对与误差测量 */
int mip_sharpen_ref(const unsigned char *src, int w, int h,
                    const MipSharpenParams *p, unsigned char *dst, MipSharpenStats *stats);

#endif
