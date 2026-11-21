#ifndef MIXER_H
#define MIXER_H
typedef struct { float *buf; int frames; int channels; int cap; } MixBus;
int mx_init(MixBus *b, int cap_frames, int channels);
void mx_free(MixBus *b);
int mx_add(MixBus *b, const float *src, int frames, float gain);
int mx_write_wav(const MixBus *b, const char *path);

/* ---- 三段式峰值保持电平表 ------------------------------------------------
 *
 * 第 1 段 mx_meter_peaks()：每 block_frames 帧取一次逐声道绝对峰值，
 *         最后一块可以不足 block_frames 帧。
 * 第 2 段 mx_meter_hold()：把块峰值按帧展开并保持。块峰值严格高于当前
 *         保持电平时抬到新值，并从该帧起保持 hold_frames 帧；否则计时递减，
 *         保持期间电平不动。输出 hold_level（逐帧保持电平）与 hold_left
 *         （该帧剩余的保持帧数，0 = 保持已到点）。
 * 第 3 段 mx_meter_release()：hold_left > 0 的帧输出保持电平；到点后每帧
 *         减去 release（线性电平/帧）。衰减不会低于 0；若本帧所属块的峰值
 *         高于衰减值，表针抬到该峰值并重新按 hold_frames 顶住——安静一段
 *         之后出现的较低新峰同样有完整的保持时长，持续音则停在信号电平上
 *         （表针不落在当前信号之下）。
 *
 * enabled = 0 时只做第 1 段：块峰值逐帧展开，输出与加入保持之前一致。
 * hold_frames / release 无论 enabled 与否都会校验，非法值一律报错。
 *
 * 缓冲区约定（均按 frames * channels 个元素，由调用方分配）：
 *   peaks      : ceil(frames / block_frames) * channels
 *   hold_level : frames * channels
 *   hold_left  : frames * channels（int）
 *   out        : frames * channels
 */
typedef struct {
    int   block_frames;  /* 每块帧数，必须 > 0 */
    int   hold_frames;   /* 保持时长（帧），必须 > 0；0 或负数报错 */
    float release;       /* 回落速率（线性电平/帧），必须有限且 >= 0；负数报错 */
    int   enabled;       /* 0 = 关闭保持与回落，仅输出块峰值 */
} MxMeterCfg;

typedef struct {
    int   frames;          /* 输入帧数 */
    int   channels;        /* 声道数 */
    int   blocks;          /* ceil(frames / block_frames) */
    float peak_in;         /* 输入最大 |x| */
    float peak_out;        /* 输出最大电平 */
    float peak_err;        /* 逐声道 |max out - max in| 的最大值 */
    int   hold_observed;   /* 各声道角标为最大输出值的连续帧数，取最大 */
    int   overrun_blocks;  /* 块峰值 > 1.0（0 dBFS）的块数 */
    int   overrun_frames;  /* 这些块覆盖的帧数 */
    int   overrun_runs;    /* 连续越界块的段数 */
    int   recovery_frames; /* 最后一个越界块结束后，各声道输出都回到
                              < 1e-3（-60 dBFS）所需的帧数；没有越界为 0，
                              缓冲区结束仍未恢复为 -1 */
} MxMeterReport;

enum {
    MX_METER_OK = 0,
    MX_METER_EARG = -1,       /* 空指针 / frames <= 0，字段 "frames" */
    MX_METER_EHOLD = -2,      /* hold_frames <= 0，字段 "hold_frames" */
    MX_METER_ERELEASE = -3,   /* release < 0 或非有限值，字段 "release" */
    MX_METER_EBLOCK = -4,     /* block_frames <= 0，字段 "block_frames" */
    MX_METER_ECHANNELS = -5,  /* channels <= 0，字段 "channels" */
    MX_METER_EMEM = -6        /* 临时缓冲区分配失败，字段 "memory" */
};

/* 第 1 段：块峰值。返回块数（= *nblocks），出错返回负的 MX_METER_* 码。 */
int mx_meter_peaks(const float *src, int frames, int channels, int block_frames,
                   float *peaks, int *nblocks);

/* 第 2 段：保持。返回 frames，出错返回负码。 */
int mx_meter_hold(const float *peaks, int frames, int channels, int block_frames,
                  int hold_frames, float *hold_level, int *hold_left);

/* 第 3 段：回落。返回 frames，出错返回负码。 */
int mx_meter_release(const float *peaks, const float *hold_level, const int *hold_left,
                     int frames, int channels, int block_frames, int hold_frames,
                     float release, float *out);

/* 三段顺序执行；rep 非空时填写统计。返回 frames，出错返回负码。 */
int mx_meter_process(const float *src, int frames, int channels, const MxMeterCfg *cfg,
                     float *out, MxMeterReport *rep);

/* 出错码对应的字段名（"hold_frames"、"release" 等）。 */
const char *mx_meter_field(int err);

#endif
