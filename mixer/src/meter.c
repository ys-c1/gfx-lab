#include "mixer.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* 恢复判定门限：线性 -60 dBFS。所有声道都低于它才算恢复。 */
#define MX_METER_RECOVERY_LEVEL 1e-3f

/* 0 dBFS：块峰值超过 1.0 记为一次越界。 */
#define MX_METER_OVERRUN_LEVEL 1.0f

/* 测量“保持帧数”时判定两个电平相等的容差。 */
#define MX_METER_EQ_EPS 1e-7f

static int meter_blocks(int frames, int block_frames) {
    return (frames + block_frames - 1) / block_frames;
}

const char *mx_meter_field(int err) {
    switch (err) {
        case MX_METER_OK:        return "ok";
        case MX_METER_EARG:      return "frames";
        case MX_METER_EHOLD:     return "hold_frames";
        case MX_METER_ERELEASE:  return "release";
        case MX_METER_EBLOCK:    return "block_frames";
        case MX_METER_ECHANNELS: return "channels";
        case MX_METER_EMEM:      return "memory";
        default:                 return "unknown";
    }
}

/* 三个数值字段共用一套校验；hold 必须 > 0，release 必须有限且 >= 0。 */
static int meter_validate(int frames, int channels, int block_frames,
                          int hold_frames, float release) {
    if (frames <= 0) return MX_METER_EARG;
    if (channels <= 0) return MX_METER_ECHANNELS;
    if (block_frames <= 0) return MX_METER_EBLOCK;
    if (hold_frames <= 0) return MX_METER_EHOLD;
    if (!isfinite(release) || !(release >= 0.0f)) return MX_METER_ERELEASE;
    return MX_METER_OK;
}

/* ---- 第 1 段：按块取峰值 ------------------------------------------------ */

int mx_meter_peaks(const float *src, int frames, int channels, int block_frames,
                   float *peaks, int *nblocks) {
    if (!src || !peaks || !nblocks) return MX_METER_EARG;
    if (frames <= 0) return MX_METER_EARG;
    if (channels <= 0) return MX_METER_ECHANNELS;
    if (block_frames <= 0) return MX_METER_EBLOCK;

    const int blocks = meter_blocks(frames, block_frames);
    for (size_t k = 0; k < (size_t)blocks * (size_t)channels; ++k) peaks[k] = 0.0f;

    for (int i = 0; i < frames; ++i) {
        const int b = i / block_frames;
        for (int c = 0; c < channels; ++c) {
            float v = fabsf(src[(size_t)i * channels + c]);
            float *p = &peaks[(size_t)b * channels + c];
            if (v > *p) *p = v;
        }
    }
    *nblocks = blocks;
    return blocks;
}

/* ---- 第 2 段：按保持时长顶住 --------------------------------------------
 *
 * 块峰值只在块的起始帧参与判定：严格低于当前保持电平的块不打断计时，
 * 达到或超过（含相等，保证重复的等幅瞬态每次都重新顶住）则抬到该峰值
 * 并把计时重置为 hold_frames 帧。计时期间电平不动。
 * hold_left[i] 是该帧剩余的保持帧数，因此触发帧起连续 hold_frames 帧
 * 均为正，第 hold_frames 帧之后的那一帧为 0。 */

int mx_meter_hold(const float *peaks, int frames, int channels, int block_frames,
                  int hold_frames, float *hold_level, int *hold_left) {
    if (!peaks || !hold_level || !hold_left) return MX_METER_EARG;
    if (frames <= 0) return MX_METER_EARG;
    if (channels <= 0) return MX_METER_ECHANNELS;
    if (block_frames <= 0) return MX_METER_EBLOCK;
    if (hold_frames <= 0) return MX_METER_EHOLD;

    for (int c = 0; c < channels; ++c) {
        float level = 0.0f;
        int left = 0;
        for (int i = 0; i < frames; ++i) {
            if (i % block_frames == 0) {
                float p = peaks[(size_t)(i / block_frames) * channels + c];
                if (p > 0.0f && p >= level) {
                    level = p;
                    left = hold_frames;
                }
            }
            hold_level[(size_t)i * channels + c] = level;
            hold_left[(size_t)i * channels + c] = left;
            if (left > 0) --left;
        }
    }
    return frames;
}

/* ---- 第 3 段：到点后按回落速率衰减 --------------------------------------
 *
 * hold_left > 0 的帧原样输出保持电平。计时到点后每帧从上一帧输出减去
 * release，下限为 0。若本帧所属块的峰值高于衰减值，表针抬到该峰值并从
 * 这里重新顶住 hold_frames 帧（回落期间出现的较低新峰也有完整保持）。
 * 块峰值就是当前信号的实际电平，持续音因此不会落到它以下。 */

int mx_meter_release(const float *peaks, const float *hold_level, const int *hold_left,
                     int frames, int channels, int block_frames, int hold_frames,
                     float release, float *out) {
    if (!peaks || !hold_level || !hold_left || !out) return MX_METER_EARG;
    if (frames <= 0) return MX_METER_EARG;
    if (channels <= 0) return MX_METER_ECHANNELS;
    if (block_frames <= 0) return MX_METER_EBLOCK;
    if (hold_frames <= 0) return MX_METER_EHOLD;
    if (!isfinite(release) || !(release >= 0.0f)) return MX_METER_ERELEASE;

    for (int c = 0; c < channels; ++c) {
        float prev = 0.0f;
        int local_left = 0;
        for (int i = 0; i < frames; ++i) {
            float level;
            if (hold_left[(size_t)i * channels + c] > 0) {
                level = hold_level[(size_t)i * channels + c];
                local_left = 0;
            } else if (local_left > 0) {
                --local_left;
                level = prev;
            } else {
                level = prev - release;
                if (level < 0.0f) level = 0.0f;
                float p = peaks[(size_t)(i / block_frames) * channels + c];
                if (p > level) {
                    level = p;
                    local_left = hold_frames - 1;
                }
            }
            out[(size_t)i * channels + c] = level;
            prev = level;
        }
    }
    return frames;
}

/* ---- 统计 --------------------------------------------------------------- */

/* 各声道最大输出值的连续帧数，取各声道最长的一段。 */
static int hold_observed(const float *out, int frames, int channels) {
    int best = 0;
    for (int c = 0; c < channels; ++c) {
        float peak = 0.0f;
        for (int i = 0; i < frames; ++i) {
            float v = out[(size_t)i * channels + c];
            if (v > peak) peak = v;
        }
        int run = 0;
        for (int i = 0; i < frames; ++i) {
            float v = out[(size_t)i * channels + c];
            if (fabsf(v - peak) <= MX_METER_EQ_EPS) {
                if (++run > best) best = run;
            } else {
                run = 0;
            }
        }
    }
    return best;
}

static void meter_report(const float *src, const float *out, const float *peaks,
                         int frames, int channels, int block_frames, int blocks,
                         MxMeterReport *rep) {
    float *peak_in = (float*)calloc((size_t)channels, sizeof(float));
    float *peak_out = (float*)calloc((size_t)channels, sizeof(float));
    if (!peak_in || !peak_out) { free(peak_in); free(peak_out); return; }

    float all_in = 0.0f, all_out_peak = 0.0f;
    for (int i = 0; i < frames; ++i) {
        for (int c = 0; c < channels; ++c) {
            float a = fabsf(src[(size_t)i * channels + c]);
            float b = fabsf(out[(size_t)i * channels + c]);
            if (a > peak_in[c]) peak_in[c] = a;
            if (b > peak_out[c]) peak_out[c] = b;
            if (a > all_in) all_in = a;
            if (b > all_out_peak) all_out_peak = b;
        }
    }
    float peak_err = 0.0f;
    for (int c = 0; c < channels; ++c) {
        float e = fabsf(peak_out[c] - peak_in[c]);
        if (e > peak_err) peak_err = e;
    }
    free(peak_in);
    free(peak_out);

    int over_blocks = 0, over_frames = 0, over_runs = 0, run = 0, last_over = -1;
    for (int b = 0; b < blocks; ++b) {
        int over = 0;
        for (int c = 0; c < channels; ++c)
            if (peaks[(size_t)b * channels + c] > MX_METER_OVERRUN_LEVEL) over = 1;
        if (over) {
            ++over_blocks;
            int f0 = b * block_frames;
            int f1 = f0 + block_frames;
            if (f1 > frames) f1 = frames;
            over_frames += f1 - f0;
            last_over = b;
            if (run == 0) ++over_runs;
            ++run;
        } else {
            run = 0;
        }
    }

    int recovery = 0;
    if (over_blocks > 0) {
        int end = (last_over + 1) * block_frames;
        if (end > frames) end = frames;
        recovery = -1;                       /* 到缓冲区结束仍未恢复 */
        for (int i = end; i < frames; ++i) {
            int quiet = 1;
            for (int c = 0; c < channels; ++c)
                if (fabsf(out[(size_t)i * channels + c]) >= MX_METER_RECOVERY_LEVEL) {
                    quiet = 0;
                    break;
                }
            if (quiet) { recovery = i - end; break; }
        }
    }

    if (rep) {
        rep->frames = frames;
        rep->channels = channels;
        rep->blocks = blocks;
        rep->peak_in = all_in;
        rep->peak_out = all_out_peak;
        rep->peak_err = peak_err;
        rep->hold_observed = hold_observed(out, frames, channels);
        rep->overrun_blocks = over_blocks;
        rep->overrun_frames = over_frames;
        rep->overrun_runs = over_runs;
        rep->recovery_frames = recovery;
    }
}

/* ---- 三段封装 ----------------------------------------------------------- */

int mx_meter_process(const float *src, int frames, int channels, const MxMeterCfg *cfg,
                     float *out, MxMeterReport *rep) {
    if (!src || !out || !cfg) return MX_METER_EARG;

    int rc = meter_validate(frames, channels, cfg->block_frames,
                            cfg->hold_frames, cfg->release);
    if (rc != MX_METER_OK) {
        if (rc == MX_METER_EHOLD)
            fprintf(stderr, "mx_meter_process: parameter 'hold_frames' must be > 0 (got %d)\n",
                    cfg->hold_frames);
        else if (rc == MX_METER_ERELEASE)
            fprintf(stderr, "mx_meter_process: parameter 'release' must be finite and >= 0 (got %g)\n",
                    (double)cfg->release);
        else if (rc == MX_METER_EBLOCK)
            fprintf(stderr, "mx_meter_process: parameter 'block_frames' must be > 0 (got %d)\n",
                    cfg->block_frames);
        else if (rc == MX_METER_ECHANNELS)
            fprintf(stderr, "mx_meter_process: parameter 'channels' must be > 0 (got %d)\n",
                    channels);
        else
            fprintf(stderr, "mx_meter_process: parameter 'frames' must be > 0 (got %d)\n",
                    frames);
        return rc;
    }

    const int blocks = meter_blocks(frames, cfg->block_frames);
    const size_t samples = (size_t)frames * (size_t)channels;

    float *peaks = (float*)malloc((size_t)blocks * (size_t)channels * sizeof(float));
    if (!peaks) return MX_METER_EMEM;

    int nb = 0;
    mx_meter_peaks(src, frames, channels, cfg->block_frames, peaks, &nb);

    if (cfg->enabled) {
        float *hold_level = (float*)malloc(samples * sizeof(float));
        int *hold_left = (int*)malloc(samples * sizeof(int));
        if (!hold_level || !hold_left) {
            free(hold_level); free(hold_left); free(peaks);
            return MX_METER_EMEM;
        }
        mx_meter_hold(peaks, frames, channels, cfg->block_frames, cfg->hold_frames,
                      hold_level, hold_left);
        mx_meter_release(peaks, hold_level, hold_left, frames, channels,
                         cfg->block_frames, cfg->hold_frames, cfg->release, out);
        free(hold_level);
        free(hold_left);
    } else {
        /* 关闭保持：只做第 1 段，块峰值逐帧展开，输出与加入保持之前一致。 */
        for (int i = 0; i < frames; ++i) {
            int b = i / cfg->block_frames;
            for (int c = 0; c < channels; ++c)
                out[(size_t)i * channels + c] = peaks[(size_t)b * channels + c];
        }
    }

    meter_report(src, out, peaks, frames, channels, cfg->block_frames, blocks, rep);
    free(peaks);
    return frames;
}
