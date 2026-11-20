#ifndef DCOMP_H
#define DCOMP_H
#include <stdio.h>

/* 三档：门限以下 / 压缩区 / 限幅区（边界：|x|==threshold 归低档，==ceiling 归压缩档） */
#define DC_TIER_LOW      0
#define DC_TIER_COMPRESS 1
#define DC_TIER_LIMIT    2

typedef struct {
    float threshold;    /* 门限，线性幅度，合法范围 (0,1] */
    float ratio;        /* 压缩比，>=1（4.0 即 4:1） */
    float ceiling;      /* 限幅天花板，线性幅度，(0,1] 且 >= threshold */
    float attack_ms;    /* 启动时间，毫秒，>=0（0 表示瞬时） */
    float release_ms;   /* 恢复时间，毫秒，>=0（0 表示瞬时） */
    int   sample_rate;  /* 采样率 Hz，>0 */
    int   bypass;       /* 1=不加处理，输出与输入逐样本一致 */
} DcParams;

typedef struct {
    float in_peak;                  /* 输入峰值（线性幅度） */
    float out_peak;                 /* 输出峰值（线性幅度） */
    float peak_over;                /* 输出峰值超天花板的误差（线性幅度，0=无超调） */
    float max_gain_reduction_db;    /* 最大压缩量 dB（正数） */
    double mean_gain_reduction_db;  /* 全样本平均压缩量 dB */
    int   over_count;               /* 瞬时电平超门限的样本数 */
    int   limit_count;              /* 落入限幅档的样本数 */
    double over_ms;                 /* 超门限累计时长 ms */
    double measured_release_ms;     /* 实测恢复时间 ms；-1=全程未超门限 */
    int   release_truncated;        /* 1=素材结束前增益未恢复到 -0.1dB 以内 */
    int   tier_hist[3];             /* 三档样本计数：低/压缩/限幅 */
} DcStats;

/* 校验参数；非法时返回负值并向 stderr 报错、指出字段名 */
int dc_validate(const DcParams *p);

/* 逐样本处理；gain_out/tier_out 可为 NULL，st 可为 NULL */
int dc_process(const DcParams *p, const float *in, float *out, int n,
               float *gain_out, unsigned char *tier_out, DcStats *st);

/* 导出增益曲线 CSV：sample,time_ms,in,level,gain_db,out,tier */
int dc_export_curve(const DcParams *p, const float *in, int n,
                    const float *gain, const unsigned char *tier, FILE *fp);
#endif
