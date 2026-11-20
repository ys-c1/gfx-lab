#ifndef MIXER_H
#define MIXER_H
typedef struct { float *buf; int frames; int channels; int cap; } MixBus;
int mx_init(MixBus *b, int cap_frames, int channels);
void mx_free(MixBus *b);
int mx_add(MixBus *b, const float *src, int frames, float gain);
int mx_write_wav(const MixBus *b, const char *path);

/* 侧链闪避：按主轨（旁白）电平包络压背景增益。
 * 包络为瞬时起音 + 指数释放的峰值跟随器：
 *   env[n] = max(|main[n]|, env[n-1] * coef), coef = exp(-1 / (release_s * rate))
 * 归一化 x = min(env / level, 1)，背景增益 g = 1 - (1 - gain) * x。 */
typedef struct {
    float level;       /* 主轨触发电平（线性，必须 > 0），达到即满深度闪避 */
    float gain;        /* 满闪避时背景增益（必须 >= 0；1 = 不闪避） */
    float release_ms;  /* 释放时间常数（毫秒，必须 >= 0；0 = 瞬时释放） */
    int enabled;       /* 0 = 旁通：bg 完全不被改写，逐样本相同 */
} MxDuck;

enum {
    MX_DUCK_OK = 0,
    MX_DUCK_EARG = -1,      /* 空指针 / 帧数、声道数、采样率非法 */
    MX_DUCK_ELEVEL = -2,    /* level 为零、为负或非有限值 */
    MX_DUCK_EGAIN = -3,     /* gain 为负或非有限值 */
    MX_DUCK_ERELEASE = -4   /* release_ms 为负或非有限值 */
};

/* 就地压 bg 增益；depth_db 非空时回报实测压缩深度（正数 dB，满闪避到 0 时为 INFINITY）。
 * cfg 为 NULL 或 enabled == 0 时直接返回 MX_DUCK_OK，bg 逐样本不变。 */
int mx_duck(float *bg, const float *main, int frames, int channels, float rate,
            const MxDuck *cfg, float *depth_db);
#endif
