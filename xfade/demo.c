#include "xfade.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* 演示/评测：
 * 素材 A=440Hz 正弦，B=660Hz 正弦（接缝处相位刻意错开，硬切会产生明显咔哒声）。
 * 对 1s 与 10s 两种素材分别跑：硬切(XF_NONE)、线性淡化(对照)、等功率曲线，
 * 比较接缝样本跳变、对双精度参考实现的样本误差、以及耗时；权重曲线落盘 CSV。
 */

#define SR 48000
#define CH 1
#define FADE_MS 100
#define REPS_1S 300
#define REPS_10S 30

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static double now_sec(void) {
    return (double)clock() / (double)CLOCKS_PER_SEC;   /* 标准 C11，CPU 耗时 */
}

static void make_tone(float *buf, int frames, double freq, double phase, double amp) {
    for (int i = 0; i < frames; ++i)
        buf[i] = (float)(amp * sin(2.0 * M_PI * freq * (double)i / SR + phase));
}

/* 双精度参考交叉合成，用于度量单精度实现的样本误差。 */
static void ref_cross(const float *a, int na, const float *b, int nb,
                      XfCurve curve, int n, double *ref) {
    int head = na - n, total = na + nb - n;
    for (int i = 0; i < head; ++i) ref[i] = a[i];
    for (int i = 0; i < n; ++i) {
        double t = (n == 1) ? 1.0 : (double)i / (double)(n - 1);
        double wa, wb, ang = 0.5 * M_PI * t;
        if (curve == XF_LINEAR) { wa = 1.0 - t; wb = t; }
        else if (curve == XF_POWER) { wa = cos(ang); wb = sin(ang); }
        else { wa = 1.0; wb = 0.0; }
        ref[head + i] = (double)a[head + i] * wa + (double)b[i] * wb;
    }
    for (int i = n; i < nb; ++i) ref[head + i] = (double)b[i];
    (void)total;
}

static int write_wav_mono(const char *path, const float *buf, int frames) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    unsigned int data_bytes = (unsigned int)(frames * 2), riff = 36 + data_bytes;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    unsigned int sz = 16, rate = SR, byte_rate = SR * 2;
    unsigned short fmt = 1, ch = 1, align = 2, bits = 16;
    fwrite(&sz, 4, 1, f); fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f);
    fwrite(&rate, 4, 1, f); fwrite(&byte_rate, 4, 1, f);
    fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data_bytes, 4, 1, f);
    for (int i = 0; i < frames; ++i) {
        float v = buf[i];
        if (v > 1.0f) v = 1.0f;
        if (v < -1.0f) v = -1.0f;
        short s = (short)lrint(v * 32767.0);
        fwrite(&s, 2, 1, f);
    }
    return fclose(f);
}

typedef struct {
    const char *tag;
    XfCurve curve;
    int fade_ms;
} Variant;

int main(void) {
    const Variant vars[3] = {
        {"hard-cut   ", XF_NONE,   0},
        {"linear     ", XF_LINEAR, FADE_MS},
        {"equal-power", XF_POWER,  FADE_MS},
    };
    const int lens[2] = {SR, 10 * SR};          /* 1s 与 10s */
    const int reps[2] = {REPS_1S, REPS_10S};

    /* 权重曲线保存（每条 481 点，便于直接画图） */
    XfConfig lin_cfg = {SR, CH, XF_LINEAR, FADE_MS};
    XfConfig pwr_cfg = {SR, CH, XF_POWER, FADE_MS};
    int rc1 = xf_export_curve("curve_linear.csv", &lin_cfg, 481);
    int rc2 = xf_export_curve("curve_equal_power.csv", &pwr_cfg, 481);
    printf("curve save: linear=%d (%s), power=%d (%s)\n\n",
           rc1, xf_error_str(rc1), rc2, xf_error_str(rc2));

    for (int L = 0; L < 2; ++L) {
        int nclip = lens[L];
        float *a = malloc(sizeof(float) * nclip);
        float *b = malloc(sizeof(float) * nclip);
        float *out = malloc(sizeof(float) * (2 * nclip));
        double *ref = malloc(sizeof(double) * (2 * nclip));
        make_tone(a, nclip, 440.0, 0.0, 0.5);
        make_tone(b, nclip, 660.0, M_PI / 2.0, 0.5); /* 接缝相位错开 -> 硬切咔哒声 */

        printf("================ clip %d s (%d frames each), fade %d ms ================\n",
               L == 0 ? 1 : 10, nclip, FADE_MS);
        printf("%-12s %8s %12s %12s %12s %12s %12s\n",
               "variant", "out-frm", "cut-jump", "region-jump", "max-err", "rms-err", "us/call");

        for (int v = 0; v < 3; ++v) {
            XfConfig cfg = {SR, CH, vars[v].curve, vars[v].fade_ms};
            int n = xf_overlap_frames(&cfg);
            int on = 0;
            int rc = xf_run(&cfg, a, nclip, b, nclip, out, 2 * nclip, &on);
            if (rc != XF_OK) { printf("%s run failed: %d %s [%s]\n",
                                      vars[v].tag, rc, xf_error_str(rc), xf_error_field(rc));
                continue;
            }

            /* 接缝跳变：切点位置的单样本跳变（三种处理同一口径） */
            int seam = nclip - 1;
            float cut_jump; int at;
            xf_seam_jump(out, on, CH, seam, 0, &cut_jump, &at);

            /* 整个交叉窗口内的最大相邻样本跳变；硬切窗口退化为切点本身 */
            float region_jump;
            int radius = n > 0 ? n / 2 : 0;
            int center = n > 0 ? (nclip - n) + n / 2 : seam;
            xf_seam_jump(out, on, CH, center, radius, &region_jump, &at);

            /* 对双精度参考的样本误差 */
            ref_cross(a, nclip, b, nclip, vars[v].curve, n, ref);
            double max_err = 0.0, sum_sq = 0.0;
            for (int i = 0; i < on; ++i) {
                double d = (double)out[i] - ref[i];
                if (fabs(d) > max_err) max_err = fabs(d);
                sum_sq += d * d;
            }
            double rms = sqrt(sum_sq / on);

            /* 耗时：完整 xf_run（建权重 + 合成），多次平均 */
            double t0 = now_sec();
            for (int k = 0; k < reps[L]; ++k) {
                int rr = xf_run(&cfg, a, nclip, b, nclip, out, 2 * nclip, &on);
                if (rr != XF_OK) { printf("run failed in bench\n"); break; }
            }
            double us_run = (now_sec() - t0) * 1e6 / reps[L];

            printf("%-12s %8d %12.6f %12.6f %12.2e %12.2e %12.1f\n",
                   vars[v].tag, on, cut_jump, region_jump, max_err, rms, us_run);

            /* 单独量一次建权重耗时（第 1 段成本） */
            if (n > 0) {
                float *wa, *wb;
                double w0 = now_sec();
                int wr = xf_build_weights(vars[v].curve, n, &wa, &wb);
                double us_w = (now_sec() - w0) * 1e6;
                if (wr == XF_OK)
                    printf("  └ weight build: n=%d frames, %.1f us (%.3f us/1000 frames)\n",
                           n, us_w, us_w / n * 1000.0);
                free(wa); free(wb);
            }

            /* 1 秒素材导出可听 WAV */
            if (L == 0) {
                char path[64];
                snprintf(path, sizeof path, "demo_1s_%s.wav",
                         vars[v].curve == XF_NONE ? "hardcut"
                         : vars[v].curve == XF_LINEAR ? "linear" : "power");
                write_wav_mono(path, out, on);
            }
        }
        printf("\n");
        free(a); free(b); free(out); free(ref);
    }

    /* 错误路径演示：字段定位 */
    XfConfig bad1 = {SR, CH, XF_LINEAR, 0};                 /* 时长为零 */
    XfConfig bad2 = {SR, CH, (XfCurve)42, 100};             /* 曲线非法 */
    XfConfig bad3 = {SR, CH, XF_LINEAR, 100000};            /* 重叠超长 */
    const XfConfig *bads[3] = {&bad1, &bad2, &bad3};
    float dummy[8] = {0};
    for (int i = 0; i < 3; ++i) {
        int rc = xf_run(bads[i], dummy, 4, dummy, 4, dummy, 8, NULL);
        printf("error case %d: rc=%d field=%s (%s)\n",
               i + 1, rc, xf_error_field(rc), xf_error_str(rc));
    }
    return 0;
}
