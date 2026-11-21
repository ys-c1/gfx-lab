#ifndef MX_PAN_H
#define MX_PAN_H

#include "mixer.h"

/*
 * 等功率声像（mixer 扩展）
 * =======================
 *
 * 旧的线性声像把左右增益写成 (1-p)/2、(1+p)/2：声像居中时左右各 0.5，
 * 总功率只有 0.5（-3.01 dB），中间听起来发空。本模块把声像拆成两段：
 *
 *   段 1  mx_pan_gains()   按声像律算左右增益（只算数，不碰缓冲）
 *           equal-power: theta = (p+1)*pi/4, gL = cos(theta), gR = sin(theta)
 *                        恒有 gL^2 + gR^2 = 1
 *           linear     : gL = (1-p)/2, gR = (1+p)/2（旧行为，作对照）
 *
 *   段 2  mx_pan_spread()  按增益分配到通道
 *           总线按 L/R 对解释：(0,1), (2,3), …；每对的左声道乘 gL，
 *           右声道乘 gR。2 声道即一对，8 声道即四对，各对使用同一组增益。
 *
 * 关闭（enabled = 0）时 mx_pan_apply() 在参数校验与任何写入之前直接返回，
 * 总线逐位不变，与未接入声像时一模一样（可用 memcmp 校验）。因此旧的
 * mx_add()/mx_write_wav() 路径和 demo.wav 也不受任何影响。
 *
 * 非法组合一次列全：mx_pan_validate()/mx_pan_apply() 把所有冲突项写进
 * MxPanErr（item[] 逐项、msg 用 "; " 连接）；拒绝发生在写缓冲之前，
 * 被拒绝的总线一个样本都不会被改动。
 *
 * 位置 p 的取值范围是 [-1, +1]：-1 硬左，0 居中，+1 硬右。
 */

#define MX_PAN_MAX_CHANNELS   8   /* 总线最多 8 声道（4 个 L/R 对） */
#define MX_PAN_MAX_CONFLICTS  8   /* 最多逐项记录的冲突数 */
#define MX_PAN_CONFLICT_LEN  96   /* 单个冲突项的文本长度 */
#define MX_PAN_MSG_LEN      768   /* 汇总文本长度 */

typedef enum {
    MX_PAN_LINEAR      = 0,       /* 线性（旧行为，对照） */
    MX_PAN_EQUAL_POWER = 1        /* 等功率（cos/sin） */
} MxPanLaw;

typedef struct {
    int   enabled;   /* 0 = 关闭（逐位旁路）；1 = 启用；其他值非法 */
    int   law;       /* MxPanLaw */
    float position;  /* [-1, +1]，必须有限（非 NaN / Inf） */
} MxPanCfg;

/* 一次成功调用的测量值。mx_pan_spread() 单独调用时 law = -1、
 * position / pos_est 为 NaN，因为这两项要由第一段与声像律给出。 */
typedef struct {
    int       law;        /* 生效的声像律 */
    float     position;   /* 请求的声像位置 */
    float     pos_est;    /* 由左右增益按同一律反解出的位置 */
    float     gL, gR;     /* 第一段算出的左右增益 */
    float     energy;     /* gL^2 + gR^2（equal-power 恒为 1） */
    int       channels;   /* 总线通道数 */
    int       pairs;      /* channels/2，实际套用增益的 L/R 对数 */
    int       frames;     /* 处理的帧数 */
    long long samples;    /* frames * channels */
} MxPanStats;

/* 拒绝结果：item[] 逐项列出冲突，msg 为 "; " 连接的汇总；
 * count 是冲突总数，可能大于 MX_PAN_MAX_CONFLICTS（此时只保留前 8 项文本）。 */
typedef struct {
    int  count;
    char item[MX_PAN_MAX_CONFLICTS][MX_PAN_CONFLICT_LEN];
    char msg[MX_PAN_MSG_LEN];
} MxPanErr;

enum {
    MX_PAN_OK          =  0,
    MX_PAN_ERR_NULL    = -1,  /* bus 或 cfg 为空指针 */
    MX_PAN_ERR_INVALID = -2   /* 一个或多个字段/组合非法 */
};

const char *mx_pan_law_name(MxPanLaw law);   /* "linear" / "equal-power" / "invalid" */

/* 清空拒绝结果（count=0，msg 为空串）。err 可为 NULL。 */
void mx_pan_err_clear(MxPanErr *err);

/*
 * 校验配置与总线。返回冲突项数量，0 表示合法。enabled = 0 时视为旁路，
 * 不检查其余字段也不检查总线布局（保持与旧路径完全一致）。
 * 检查项：
 *   cfg.enabled  必须为 0 或 1
 *   cfg.law      必须为 MX_PAN_LINEAR 或 MX_PAN_EQUAL_POWER
 *   cfg.position 必须有限且落在 [-1, +1]
 *   bus.buf      非空
 *   bus.channels 必须为偶数且落在 [2, MX_PAN_MAX_CHANNELS]
 *   bus.frames   >= 0 且 <= bus.cap
 * 所有命中项都会追加到 err（不提前返回），便于一次列出全部冲突。
 */
int mx_pan_validate(const MxPanCfg *cfg, const MixBus *b, MxPanErr *err);

/*
 * 第一段：按声像律算左右增益，不触碰任何缓冲。
 * 返回 MX_PAN_OK；law 未知、position 非有限或越界返回 MX_PAN_ERR_INVALID；
 * gL/gR 为空返回 MX_PAN_ERR_NULL。
 */
int mx_pan_gains(MxPanLaw law, float position, float *gL, float *gR);

/*
 * 第一段的逆运算：由左右增益反解声像位置（线性用 (gR-gL)/(gL+gR)，
 * 等功率用 theta = atan2(gR,gL)、p = 4*theta/pi - 1）。增益须有限且非负，
 * 且不能同时为零，否则返回 MX_PAN_ERR_INVALID。
 */
int mx_pan_position(MxPanLaw law, float gL, float gR, float *position);

/*
 * 第二段：把已算好的增益分配到总线的每个 L/R 对（原地）。
 * 要求 bus 已初始化、声道数为偶数且在范围内、frames <= cap；
 * 增益须有限且落在 [0, 1]。非法时返回 MX_PAN_ERR_INVALID 且不写缓冲。
 * stats 可为 NULL；非空时填 gL/gR/energy/channels/pairs/frames/samples，
 * 并把 law 置 -1、position/pos_est 置 NaN。
 */
int mx_pan_spread(MixBus *b, float gL, float gR, MxPanStats *stats);

/*
 * 两段合并：先校验，再 mx_pan_gains()，最后 mx_pan_spread()。
 *   - 关闭：返回 MX_PAN_OK，缓冲逐位不变，stats 清零；
 *   - 非法：返回 MX_PAN_ERR_INVALID，err 列出全部冲突，缓冲不变；
 *   - 成功：stats（若非空）含 law/position/pos_est/gL/gR/energy/路数与样本数。
 */
int mx_pan_apply(MixBus *b, const MxPanCfg *cfg, MxPanStats *stats,
                 MxPanErr *err);

#endif
