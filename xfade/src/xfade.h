#ifndef XFADE_H
#define XFADE_H

/* 切歌交叉淡化模块。
 * 三段式：
 *   1) 权重曲线在归一化时间 t∈[0,1] 上定义，与采样率无关；
 *   2) 逐样本交叉合成 out = wa*a + wb*b；
 *   3) 接缝处逐帧检查相邻样本跳变。
 * 不做淡化（XF_NONE）时输出等于两首素材直接拼接，数值照旧。
 */

typedef enum {
    XF_NONE = 0,   /* 不淡化：硬切拼接 */
    XF_LINEAR = 1, /* 线性淡化（对照曲线） */
    XF_POWER = 2   /* 等功率曲线：cos/sin，交叉区能量恒定 */
} XfCurve;

typedef struct {
    int sample_rate;   /* Hz，>0；仅用于把 fade_ms 换算成帧数 */
    int channels;      /* 每帧声道数，>0 */
    XfCurve curve;     /* 权重曲线 */
    int fade_ms;       /* 交叉淡化时长，毫秒；真实曲线必须 >0，XF_NONE 时必须为 0 */
} XfConfig;

enum {
    XF_OK = 0,
    XF_E_NULL = -1,     /* 空指针或素材帧数非法 */
    XF_E_RATE = -2,     /* sample_rate 非法 */
    XF_E_CHANNELS = -3, /* channels 非法 */
    XF_E_CURVE = -4,    /* curve 非法，或 fade_ms 与曲线不匹配 */
    XF_E_DURATION = -5, /* fade_ms <= 0（真实曲线） */
    XF_E_OVERLAP = -6,  /* 淡化帧数超过任一首素材长度 */
    XF_E_CAP = -7,      /* 输出缓冲容量不足 */
    XF_E_IO = -8,       /* 权重曲线文件写入失败 */
    XF_E_NOMEM = -9     /* 内部分配失败 */
};

/* 出错字段名，配合负错误码定位参数；无错或无对应字段时返回 "none"。 */
const char *xf_error_field(int rc);
/* 错误码的人类可读说明。 */
const char *xf_error_str(int rc);
/* 曲线名，用于报表与曲线文件。 */
const char *xf_curve_name(XfCurve c);

/* 配置自检：返回 XF_OK 或负错误码（字段可经 xf_error_field 定位）。 */
int xf_validate(const XfConfig *cfg);
/* 淡化重叠帧数（四舍五入）；XF_NONE 返回 0；非法配置返回负错误码。 */
int xf_overlap_frames(const XfConfig *cfg);

/* 第 1 段：曲线上单点权重。t 自动夹到 [0,1]，与采样率无关。
 * wa = A（淡出）权重，wb = B（淡入）权重，二者在 LINEAR 下恒等和为 1。 */
int xf_curve_point(XfCurve curve, float t, float *wa, float *wb);
/* 第 1 段：预生成 n 个重叠帧的权重表，调用方负责 free(*wa)。 */
int xf_build_weights(XfCurve curve, int n, float **wa, float **wb);
/* 权重曲线落盘：CSV 行 "i,t,wa,wb"，可直接画图复核。 */
int xf_save_curve(const char *path, XfCurve curve, int points);
/* 便捷封装：按配置生成权重表并写文件。 */
int xf_export_curve(const char *path, const XfConfig *cfg, int points);

/* 第 2 段：逐样本交叉合成。
 * 重叠 n 帧：A 的末尾 n 帧与 B 的开头 n 帧逐样本加权；
 * 输出帧数 = na + nb - n；XF_NONE 时 n=0，即原样拼接。
 * out_cap 以“帧”计，须不小于 na+nb-n。 */
int xf_cross(const float *a, int na, const float *b, int nb, int channels,
             const float *wa, const float *wb, int n, float *out, int out_cap_frames);

/* 一站式：校验配置 -> 算重叠帧 -> 建权重 -> 逐样本合成。
 * *out_n 写入实际输出帧数；内部自行分配权重表并释放。 */
int xf_run(const XfConfig *cfg, const float *a, int na, const float *b, int nb,
           float *out, int out_cap_frames, int *out_n);

/* 第 3 段：接缝跳变检查。
 * seam 为接缝左帧下标（seam+1 必须存在），检查左帧落在
 * [seam-radius, seam+radius] 内的相邻样本对、逐声道取最大绝对差；
 * radius=0 恰好只看切点那一对。*jump 写入最大跳变，*at 写入其左帧下标。 */
int xf_seam_jump(const float *sig, int frames, int channels, int seam, int radius,
                 float *jump, int *at);

#endif
