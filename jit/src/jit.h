#ifndef JIT_H
#define JIT_H
#include <stdio.h>

/*
 * jit —— 抖动叠样（jittered overlay）
 *
 * 同一样本按固定周期反复叠加时，频谱会出现梳齿（梳状染色）。本模块把处理
 * 流程严格拆成三个独立阶段：
 *   阶段 1  jit_offsets    : 按确定性抖动序列生成连续（浮点）采样偏移
 *   阶段 2  jit_quantize   : 连续偏移按“半数远离零”规则取整为整数采样点
 *   阶段 3  jit_synthesize : 按每路权重把素材叠加到整数采样点
 *
 * 不做抖动（amp = 0）时每一路落点完全一致，逐样本等价于普通重复叠加。
 */

typedef struct {
    int N;            /* 叠加路数（voice 数）                       */
    int src_len;      /* 素材长度（采样数）                         */
    int period;       /* 重复周期 P（采样数）                       */
    double amp;       /* 抖动幅度 A（采样），0 表示不抖动           */
    unsigned seed;    /* 抖动序列 PRNG 种子                         */
    double *frac;     /* 阶段 1 输出：N 个连续偏移 d[k]             */
    int *integer;     /* 阶段 2 输出：N 个整数偏移 round(d[k])      */
    double *weights;  /* 每路权重，init 时默认 1/N                  */
    int have_frac;    /* 阶段 1 是否已执行                          */
    int have_int;     /* 阶段 2 是否已执行                          */
} JitPlan;

enum {
    JIT_OK = 0,
    JIT_EINVAL = 1,   /* 参数非法（含素材长度为零）                 */
    JIT_ENOMEM = 2,   /* 内存分配失败                               */
    JIT_ERANGE = 3,   /* 偏移越界 / 输出容量不足                    */
    JIT_EIO    = 4    /* 导出写失败                                 */
};

const char *jit_strerror(int code);
const char *jit_last_error(void);

/* 初始化 / 释放。N、src_len 为零或负、period<0、amp<0 一律报错并指出参数名 */
int  jit_plan_init(JitPlan *p, int N, int src_len, int period, double amp, unsigned seed);
void jit_plan_free(JitPlan *p);

/* 整数叠加输出长度：(N-1)*P + 2*ceil(A) + src_len */
int  jit_out_len(const JitPlan *p);

/* 阶段 1：抖动连续偏移。frac[k] = k*P + jitter[k]，jitter 均匀分布于 [-A,A] */
int  jit_offsets(JitPlan *p);

/* 阶段 2：偏移取整（半数远离零），并做越界校验 */
int  jit_quantize(JitPlan *p);

/* 阶段 3：按权重整数叠加，out 至少要有 jit_out_len() 个 float */
int  jit_synthesize(const JitPlan *p, const float *src, float *out, int cap);

/* 连续参考实现（线性插值，不取整），用于量化峰值误差对照 */
int  jit_synthesize_frac(const JitPlan *p, const float *src, float *out, int cap);

/* 取整后非零抖动的路数 */
int  jit_jitter_count(const JitPlan *p);

/* 抖动序列导出（CSV：voice,base,frac,jitter,jitter_q,integer,weight） */
int  jit_export_csv(const JitPlan *p, FILE *fp);

/* 梳状染色指标：对叠加位置构成的单位脉冲响应做 FFT 后统计（不含直流 bin） */
typedef struct {
    int nfft;          /* FFT 点数（2 的幂）                          */
    int ir_len;        /* 脉冲响应长度                                */
    int bins;          /* 参与统计的频谱 bin 数（nfft/2，去掉直流）   */
    int peak_bin;      /* 最高梳齿所在 bin                            */
    int above_half;    /* 幅度 >= 0.5*峰值 的 bin 数（梳齿数量）      */
    double crest_db;   /* 峰均幅度比 20log10(max/mean)，梳齿强度      */
    double flatness;   /* 功率几何均值/算术均值，越平越接近 1         */
} JitCombMetric;
int  jit_comb_metric(const JitPlan *p, JitCombMetric *m);

#endif
