#ifndef MIXER_H
#define MIXER_H
typedef struct { float *buf; int frames; int channels; int cap; } MixBus;
int mx_init(MixBus *b, int cap_frames, int channels);
void mx_free(MixBus *b);
int mx_add(MixBus *b, const float *src, int frames, float gain);
int mx_write_wav(const MixBus *b, const char *path);

/* 交叉馈送：把对侧声道按延迟+衰减混入本侧，缓解双耳（耳机）听感头晕。
 * enabled=0 时完全旁路，缓冲区逐位不变。 */
typedef struct {
    int enabled;      /* 0 = 旁路（输出与未开启时一模一样） */
    float feed;       /* 馈送量（对侧增益），开启时须在 (0,1] */
    float delay_ms;   /* 对侧延迟，开启时须在 [0,30] ms */
    int sample_rate;  /* 采样率 Hz，开启时须在 [8000,192000] */
} MxCrossfeedCfg;

typedef struct {
    int delay_samples;  /* 实际折算的延迟（采样点） */
    float peak_before;  /* 混入前峰值 */
    float peak_after;   /* 混入后峰值 */
    int clipped;        /* peak_after > 1.0 */
} MxCrossfeedStats;

/* 三段：1) 按 delay_ms 折算对侧延迟 2) 按 feed 衰减混入 3) 检查峰值。
 * 返回 0 成功；-1 非法组合，err 中逐项列出全部冲突（"; " 分隔）。 */
int mx_crossfeed_apply(MixBus *b, const MxCrossfeedCfg *cfg,
                       MxCrossfeedStats *stats, char *err, int errcap);
#endif
