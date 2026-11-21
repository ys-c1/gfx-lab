#ifndef MIXER_H
#define MIXER_H
typedef struct { float *buf; int frames; int channels; int cap; } MixBus;
int mx_init(MixBus *b, int cap_frames, int channels);
void mx_free(MixBus *b);
int mx_add(MixBus *b, const float *src, int frames, float gain);
/* 同位置叠加（overlay）：src 的 frames 从帧 0 起逐采样累加进总线，不移动写游标 */
int mx_mix_into(MixBus *b, const float *src, int frames, float gain);
int mx_write_wav(const MixBus *b, const char *path);

/* ---------- solo / mute 三段式混音 ---------- */

#define MX_OK 0
#define MX_ERR_ARG (-1)      /* 空指针、负值等参数错误 */
#define MX_ERR_RANGE (-2)    /* solo/mute 集合里有越界通道号 */
#define MX_ERR_DUP (-3)      /* 同一集合内通道号重复 */
#define MX_ERR_BUS (-4)      /* 输入源路由到了不存在的总线 */

typedef struct {
    const float *data; /* 交织采样，frames*channels，通道数与目标总线一致 */
    int frames;
    int bus;           /* 目标总线编号 */
    float gain;
} MixSource;

typedef struct {
    int *out_of_range;       /* 越界的通道号 */
    int  out_of_range_count;
    int *duplicate;          /* 集合内重复的通道号 */
    int  duplicate_count;
} MixReject;

void mx_reject_free(MixReject *r);

/* 校验 solo/mute 组合；合法返回 MX_OK，非法返回负错误码并把冲突项填入 err */
int mx_validate(int nsrc,
                const int *solo, int nsolo,
                const int *mute, int nmute,
                MixReject *err);

typedef struct {
    int  stages;        /* 段数，固定为 3 */
    int  solo_passed;   /* 段1（solo 集合筛选）后保留的源数 */
    int  muted;         /* 段2（mute 屏蔽）去掉的源数 */
    int  aggregated;    /* 段3（总线汇总）实际汇总次数 */
    long nsec_stage[3]; /* 三段各自耗时，纳秒 */
    long nsec_total;    /* 总耗时，纳秒 */
} MixStats;

/*
 * 三段式混音：段1 按 solo 集合筛选（集合为空则全部通过），
 * 段2 按 mute 集合屏蔽，段3 用 mx_mix_into 把存活源同位置汇总到各自总线。
 * 非法组合直接拒绝（返回负错误码、列出冲突项），且在写入任何总线之前拦截。
 */
int mx_mix(MixBus *buses, int nbus,
           const MixSource *srcs, int nsrc,
           const int *solo, int nsolo,
           const int *mute, int nmute,
           MixStats *stats, MixReject *err);
#endif
