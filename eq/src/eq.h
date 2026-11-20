#ifndef EQ_H
#define EQ_H

/* 三段（低/中/高）分频均衡器
 *
 * 分频结构（一阶互补，双线性变换）：
 *   low    = LPF(f_lo, x)
 *   high   = x - low                 （即 HPF(f_lo)，与 LPF 严格互补）
 *   mid    = LPF(f_hi, high)
 *   treble = high - mid              （即 HPF(f_hi)）
 *   y      = g_lo*low + g_mid*mid + g_hi*treble
 * 单位增益时 y == x（浮点舍入误差内完全重构）；enabled=0 时逐样本原样拷贝。
 */

#define EQ_MAX_CHANNELS 8
#define EQ_MAX_GAIN 16.0f /* 线性增益上限，约 +24 dB */

typedef struct {
    int sample_rate;          /* Hz，必须 > 0 */
    int channels;             /* 1..EQ_MAX_CHANNELS */
    int enabled;              /* 0=旁路(逐样本一致) 1=启用 */
    float f_lo;               /* 低/中分界 Hz */
    float f_hi;               /* 中/高分界 Hz */
    float g_lo, g_mid, g_hi;  /* 各段线性增益，范围 [0, EQ_MAX_GAIN] */
} EqConfig;

/* 差分方程: y[n] = b0*x[n] + b1*x[n-1] - a1*y[n-1] */
typedef struct { float b0, b1, a1; } EqCoef;

typedef struct {
    EqCoef lp_lo;  /* 低通：f_lo，输出低频段 */
    EqCoef hp_lo;  /* 高通：f_lo（= x - lp_lo 的等效系数） */
    EqCoef lp_hi;  /* 低通：f_hi，作用于高通支路，输出中频段 */
    EqCoef hp_hi;  /* 高通：f_hi（= high - lp_hi 的等效系数） */
} EqBank;

typedef struct {
    EqConfig cfg;
    EqBank bank;
    float *x1, *y1;            /* 每声道两节 LPF 的历史，长度 2*channels */
    float *peak_in, *peak_out; /* 每声道峰值（最大绝对值） */
    long *overload;            /* 每声道 |样本| > 1.0 的次数 */
    long samples;
    int ready;
} EqState;

enum {
    EQ_OK = 0,
    EQ_ERR_NULL = -1,
    EQ_ERR_RATE = -2,
    EQ_ERR_CHANNELS = -3,
    EQ_ERR_ENABLE = -4,
    EQ_ERR_FLO = -5,
    EQ_ERR_FHI = -6,
    EQ_ERR_GAIN_LO = -7,
    EQ_ERR_GAIN_MID = -8,
    EQ_ERR_GAIN_HI = -9,
    EQ_ERR_FRAMES = -10
};

int eq_validate(const EqConfig *c);
const char *eq_strerror(int code);

int eq_init(EqState *s, const EqConfig *c); /* 校验失败时返回错误码且不分配 */
void eq_free(EqState *s);
int eq_reset(EqState *s);                   /* 清零滤波器历史与统计 */

/* 处理 interleaved 浮点帧；支持原地处理(in == out)。返回帧数或负错误码 */
int eq_process(EqState *s, const float *in, float *out, int frames);

int eq_export_coef(const EqState *s, const char *path);
float eq_mag(const EqCoef *cf, float freq_hz, int sample_rate); /* 单节幅频响应(线性) */

/* 整个三段链路在 freq_hz 处的复频响应（按真实信号流复域求和，含支路相位关系）。
 * 结果写入 re/im；幅频 = hypotf(re,im)。返回 EQ_OK 或负错误码。 */
int eq_response(const EqState *s, float freq_hz, float *re, float *im);

const EqBank *eq_bank(const EqState *s);
const EqConfig *eq_config_get(const EqState *s);
/* 峰值/统计拷贝到调用者数组（传 NULL 的指针跳过），数组长度需 >= channels */
int eq_stats_get(const EqState *s, float *peak_in, float *peak_out, long *overloads);

#endif
