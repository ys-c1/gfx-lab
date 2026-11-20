#ifndef ROOM_H
#define ROOM_H

/* 室内早期反射处理器，分三段：
 *   1) room_reflect — 每条延迟线（带反馈的梳状延迟）产生一路早期反射；
 *   2) room_sum     — 各路反射按衰减系数汇总成湿声总线；
 *   3) room_mix     — 干湿比例合成输出。
 * dry=1 且 wet=0 时输出与输入逐样本一致。 */

typedef struct {
    int channels;
    int taps;
    int *delay;    /* 每路延迟，采样数，必须 > 0 */
    float *fb;     /* 每路反馈系数，必须满足 |fb| <= 1 */
    float *decay;  /* 每路输出衰减增益 */
    float dry, wet;
    float **line;  /* 每路一条环形延迟线，delay[t] * channels 个 float */
    int *pos;      /* 每路写位置 */
} Room;

enum {
    ROOM_OK = 0,
    ROOM_ERR_ARG = -1,       /* 空指针或尺寸非法 */
    ROOM_ERR_CHANNELS = -2,  /* 声道数非法或与初始化不符 */
    ROOM_ERR_DELAY = -3,     /* delay 必须 > 0 */
    ROOM_ERR_FEEDBACK = -4,  /* feedback 必须满足 |fb| <= 1 */
    ROOM_ERR_NOMEM = -5
};

/* 返回指明出错参数的可读描述。 */
const char *room_strerror(int code);

/* bad_tap 可为 NULL；非 NULL 时写入第一个非法参数的 tap 下标。 */
int room_init(Room *r, int channels, int taps,
              const int *delay, const float *feedback, const float *decay,
              int *bad_tap);
void room_free(Room *r);

void room_set_mix(Room *r, float dry, float wet);

/* 第一段：逐路生成早期反射。
 * tap_out 需容纳 taps * frames * channels 个 float（tap 为主序）。 */
int room_reflect(Room *r, const float *in, float *tap_out, int frames, int channels);

/* 第二段：各路反射按 decay 汇总到湿声总线 wet（frames * channels）。 */
int room_sum(const Room *r, const float *tap_in, float *wet, int frames, int channels);

/* 第三段：out = dry*in + wet*wet_in。dry=1、wet=0 时逐样本拷贝。 */
int room_mix(const Room *r, const float *in, const float *wet_in, float *out,
             int frames, int channels);

/* 三段一次走完（内部分配临时缓冲）。 */
int room_process(Room *r, const float *in, float *out, int frames, int channels);

#endif
