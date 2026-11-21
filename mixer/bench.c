/* 峰值保持电平表基准：
 *   1. 小算例逐帧手算包络对照
 *   2. 2 / 6 声道的峰值误差、实测保持帧数、耗时
 *   3. 保持时长 × 回落速率对照表
 *   4. 越界块数、越界段数、越界帧数与恢复时间
 */
#define _POSIX_C_SOURCE 199309L
#include "mixer.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SR 48000
#define PULSE_GAP 24000     /* 0.5 s 一个脉冲，留出完整的保持+回落 */
#define PULSE_LEN 240
#define TAIL      24000     /* 1 s 尾部静音，让表针走完回落 */

static double now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}

/* ---- 手算包络 -----------------------------------------------------------
 * 单声道：块 4 帧，保持 6 帧，每帧回落 0.2。0.8 的脉冲落在帧 1（块 0）。
 * 手算：帧 0..5 = 0.8（块首触发，保持 6 帧）；
 *       帧 6 = 0.6, 7 = 0.4, 8 = 0.2, 9 = 0.0，其后为 0。
 * 第二个 0.4 的脉冲落在帧 12（块 3，块首触发），手算帧 12..15 = 0.4。 */
static void section_hand(void) {
    enum { N = 16, B = 4, H = 6 };
    float src[N] = {0};
    src[1] = 0.8f;
    src[12] = 0.4f;
    float hand[N] = { 0.8f, 0.8f, 0.8f, 0.8f, 0.8f, 0.8f,
                      0.6f, 0.4f, 0.2f, 0.0f, 0.0f, 0.0f,
                      0.4f, 0.4f, 0.4f, 0.4f };
    float out[N];
    MxMeterCfg cfg = { B, H, 0.2f, 1 };
    MxMeterReport rep;
    memset(&rep, 0, sizeof(rep));
    int rc = mx_meter_process(src, N, 1, &cfg, out, &rep);
    printf("== 手算包络对照（block=%d hold=%d release=%.2f）==\n", B, H, (double)cfg.release);
    printf("rc=%d  peak_err=%.1e  hold_observed=%d\n", rc, (double)rep.peak_err,
           rep.hold_observed);
    printf(" frame |   input |  output |    hand | diff\n");
    float maxdiff = 0.0f;
    for (int i = 0; i < N; ++i) {
        float d = fabsf(out[i] - hand[i]);
        if (d > maxdiff) maxdiff = d;
        printf(" %5d | %7.3f | %7.3f | %7.3f | %.1e\n",
               i, (double)src[i], (double)out[i], (double)hand[i], (double)d);
    }
    printf(" max |output - hand| = %.2e\n\n", (double)maxdiff);
}

/* ---- 2 / 6 声道：峰值误差、保持帧数、耗时 ------------------------------
 * 每 0.5 s 一个指数衰减脉冲，每声道峰值不同；末尾 1 s 静音。
 * 部分声道峰值超过 1.0，用于越界统计；不削顶，输出峰值应等于输入峰值。 */
static float env_peak(int i) {
    int phase = i % PULSE_GAP;
    if (phase >= PULSE_LEN) return 0.0f;
    return expf(-5.0f * (float)phase / (float)PULSE_LEN);
}

static void section_channels(int ch, int frames) {
    const int block = 64, hold = 4800;      /* 100 ms */
    const float release = 1.0e-4f;          /* 线性 1e-4/帧 */
    const float chan_peak[6] = { 1.2f, 0.6f, 0.9f, 1.4f, 0.3f, 0.8f };

    float *src = (float*)malloc((size_t)frames * ch * sizeof(float));
    float *out = (float*)malloc((size_t)frames * ch * sizeof(float));
    if (!src || !out) { fprintf(stderr, "oom\n"); exit(1); }
    for (int i = 0; i < frames; ++i)
        for (int c = 0; c < ch; ++c)
            src[(size_t)i * ch + c] = chan_peak[c] * env_peak(i);

    MxMeterCfg cfg = { block, hold, release, 1 };
    MxMeterReport rep;
    memset(&rep, 0, sizeof(rep));

    /* 冷启动一次，再计 21 次取中位数 */
    mx_meter_process(src, frames, ch, &cfg, out, &rep);
    double times[21];
    for (int k = 0; k < 21; ++k) {
        double t0 = now_us();
        mx_meter_process(src, frames, ch, &cfg, out, &rep);
        times[k] = now_us() - t0;
    }
    for (int a = 1; a < 21; ++a) {
        double v = times[a];
        int b = a - 1;
        while (b >= 0 && times[b] > v) { times[b + 1] = times[b]; --b; }
        times[b + 1] = v;
    }
    double med = times[10];

    /* 逐声道峰值误差：|输出峰值 - 输入峰值| */
    float maxerr = 0.0f;
    int worst_ch = 0;
    for (int c = 0; c < ch; ++c) {
        float pin = 0.0f, pout = 0.0f;
        for (int i = 0; i < frames; ++i) {
            float a = fabsf(src[(size_t)i * ch + c]);
            float b = fabsf(out[(size_t)i * ch + c]);
            if (a > pin) pin = a;
            if (b > pout) pout = b;
        }
        float e = fabsf(pout - pin);
        if (e > maxerr) { maxerr = e; worst_ch = c; }
    }

    printf("== %d 声道（%d 帧, %.1f s, block=%d hold=%d release=%.1e）==\n",
           ch, frames, (double)frames / SR, block, hold, (double)release);
    printf("峰值误差 max|peak_out-peak_in| = %.2e（声道 %d）\n",
           (double)maxerr, worst_ch);
    printf("保持帧数（最长一段等于该声道最大输出值的连续帧）= %d（配置 %d）\n",
           rep.hold_observed, hold);
    printf("耗时 中位数 %.1f us / %d 帧 = %.3f us/帧（%.2f ms/秒素材）\n",
           med, frames, med / (double)frames,
           med / ((double)frames / SR) / 1e3);
    printf("越界块 %d（%d 帧, %d 段），最后越界后恢复 %d 帧 = %.2f ms\n\n",
           rep.overrun_blocks, rep.overrun_frames, rep.overrun_runs,
           rep.recovery_frames, 1000.0 * (double)rep.recovery_frames / SR);

    free(src);
    free(out);
}

/* ---- 保持 × 回落对照表 --------------------------------------------------
 * 单声道，帧 0 一个 0.9 的脉冲，之后静音。表里给出实测保持帧数、输出峰值、
 * 以及表针回到 -60 dBFS 的帧数（= 保持 + 回落）。 */
static void section_sweep(void) {
    enum { N = 48000, BLOCK = 64 };
    float *src = (float*)calloc(N, sizeof(float));
    float *out = (float*)malloc((size_t)N * sizeof(float));
    if (!src || !out) { fprintf(stderr, "oom\n"); exit(1); }
    src[0] = 0.9f;

    const int holds[3] = { 480, 4800, 19200 };            /* 10 / 100 / 400 ms */
    const float rels[3] = { 1.0e-4f, 1.0e-3f, 1.0e-2f };

    printf("== 保持与回落对照表（1 声道, 单脉冲 0.9, block=%d, %d 帧）==\n",
           BLOCK, N);
    printf(" hold_ms | hold_frames | release | hold_obs | peak_out | settle_frames | hand | diff\n");
    printf("---------+-------------+---------+----------+----------+---------------+------+-----\n");
    for (int h = 0; h < 3; ++h) {
        for (int r = 0; r < 3; ++r) {
            MxMeterCfg cfg = { BLOCK, holds[h], rels[r], 1 };
            MxMeterReport rep;
            memset(&rep, 0, sizeof(rep));
            mx_meter_process(src, N, 1, &cfg, out, &rep);

            int settle = -1;
            for (int i = 0; i < N; ++i)
                if (out[i] < 1e-3f) { settle = i; break; }
            /* 手算：保持 hold 帧后从 0.9 每帧减 release，落到 < 1e-3 需要
             * (0.9 - 1e-3)/release 步。 */
            int hand = holds[h] + (int)((0.9 - 1e-3) / (double)rels[r]);

            printf(" %7.0f | %11d | %.0e | %8d | %8.4f | %13d | %4d | %+d\n",
                   1000.0 * holds[h] / SR, holds[h], (double)rels[r],
                   rep.hold_observed, (double)rep.peak_out, settle, hand,
                   settle - hand);
        }
    }
    printf(" diff 来自 float 逐帧累减的舍入（<= 1 帧）。\n\n");
    free(src);
    free(out);
}

/* ---- 越界统计 ----------------------------------------------------------
 * 两段越界，中间隔着一段安静块：段 1（块 1..2）幅度 1.6 / 1.3，
 * 段 2（块 5）幅度 2.0。报告越界块数、帧数、段数与恢复时间。 */
static void section_overrun(void) {
    enum { N = 320, CH = 2, BLOCK = 16, HOLD = 32 };
    float src[N * CH];
    memset(src, 0, sizeof(src));
    for (int i = 16; i < 48; ++i) {                     /* 块 1..2 */
        src[i * CH + 0] = (i < 32) ? 1.6f : 1.3f;
        src[i * CH + 1] = -(i < 32 ? 1.6f : 1.3f);
    }
    for (int i = 80; i < 96; ++i) {                     /* 块 5 */
        src[i * CH + 0] = 2.0f;
        src[i * CH + 1] = 2.0f;
    }

    float out[N * CH];
    MxMeterCfg cfg = { BLOCK, HOLD, 0.05f, 1 };
    MxMeterReport rep;
    memset(&rep, 0, sizeof(rep));
    mx_meter_process(src, N, CH, &cfg, out, &rep);

    printf("== 越界与恢复（2 声道, block=%d hold=%d release=0.05）==\n",
           BLOCK, HOLD);
    printf("越界块 %d / %d，越界帧 %d / %d，越界段 %d\n",
           rep.overrun_blocks, rep.blocks, rep.overrun_frames, rep.frames,
           rep.overrun_runs);
    printf("最后越界块结束后恢复（所有声道 < 1e-3）用时 %d 帧 = %.2f ms\n",
           rep.recovery_frames, 1000.0 * (double)rep.recovery_frames / SR);
    /* 越界只报告，不削顶 */
    printf("输入峰值 %.3f，输出峰值 %.3f（不削顶）\n\n",
           (double)rep.peak_in, (double)rep.peak_out);
}

int main(void) {
    printf("mixer peak-hold meter benchmark\n\n");
    section_hand();
    section_channels(2, 48000 * 2);
    section_channels(6, 48000 * 2);
    section_sweep();
    section_overrun();
    return 0;
}
