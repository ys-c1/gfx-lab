#define _POSIX_C_SOURCE 200809L
#include "dcomp.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
}

static void report(FILE *fp, const char *tag, const DcParams *p,
                   int n, double elapsed, const DcStats *st) {
    fprintf(fp, "===== %s（%d 样本 / %.3f 秒 @ %dHz）=====\n",
            tag, n, (double)n / p->sample_rate, p->sample_rate);
    fprintf(fp, "参数: 门限=%.3f 比率=%.1f:1 天花板=%.3f 启动=%.2fms 恢复=%.2fms\n",
            p->threshold, p->ratio, p->ceiling, p->attack_ms, p->release_ms);
    fprintf(fp, "[1 门限分档] 低档=%d 压缩档=%d 限幅档=%d\n",
            st->tier_hist[DC_TIER_LOW], st->tier_hist[DC_TIER_COMPRESS],
            st->tier_hist[DC_TIER_LIMIT]);
    fprintf(fp, "[2 比率压缩] 最大压缩量=%.3fdB 平均压缩量=%.3fdB\n",
            st->max_gain_reduction_db, st->mean_gain_reduction_db);
    fprintf(fp, "[3 限幅兜底] 输入峰值=%.6f 输出峰值=%.6f 峰值误差(超天花板)=%.6f\n",
            st->in_peak, st->out_peak, st->peak_over);
    fprintf(fp, "门限时间: 超门限样本=%d（%.3fms）限幅样本=%d；"
            "恢复配置=%.2fms 实测=%s\n",
            st->over_count, st->over_ms, st->limit_count, p->release_ms,
            st->measured_release_ms < 0.0 ? "未触发" :
            (st->release_truncated ? "素材结束未恢复" : "见下"));
    if (st->measured_release_ms >= 0.0)
        fprintf(fp, "实测恢复时间(末样本超门限->增益回 -0.1dB 内)=%.3fms%s\n",
                st->measured_release_ms, st->release_truncated ? "（截断）" : "");
    fprintf(fp, "耗时: %.3fms（%.1f 样本/微秒）\n\n",
            elapsed, (double)n / (elapsed * 1000.0));
}

static int run_case(const char *tag, const DcParams *p, const float *in, int n,
                    const char *csv_path, FILE *rep) {
    float *out = (float*)malloc((size_t)n * sizeof(float));
    float *gain = (float*)malloc((size_t)n * sizeof(float));
    unsigned char *tier = (unsigned char*)malloc((size_t)n);
    if (!out || !gain || !tier) { fprintf(stderr, "alloc failed\n"); return 1; }

    double t0 = now_ms();
    DcStats st;
    int rc = dc_process(p, in, out, n, gain, tier, &st);
    double t1 = now_ms();
    if (rc != 0) return 1;

    report(stdout, tag, p, n, t1 - t0, &st);
    report(rep, tag, p, n, t1 - t0, &st);

    FILE *fp = fopen(csv_path, "wb");
    if (!fp) { fprintf(stderr, "cannot open %s\n", csv_path); return 1; }
    dc_export_curve(p, in, n, gain, tier, fp);
    fclose(fp);
    printf("增益曲线已导出: %s\n\n", csv_path);

    free(out); free(gain); free(tier);
    return 0;
}

int main(void) {
    const int sr = 48000;
    /* 大声压不下去、小声又听不见：交替安静段与冲击段的复合素材 */
    DcParams p = { 0.5f, 4.0f, 0.9f, 5.0f, 100.0f, sr, 0 };

    FILE *rep = fopen("demo.txt", "wb");
    if (!rep) return 1;

    int rc = 0;
    for (int sec = 1; sec <= 10; sec += 9) {
        int n = sr * sec;
        float *in = (float*)calloc((size_t)n, sizeof(float));
        if (!in) return 1;
        int tail = sr / 2;                         /* 末尾 0.5s 静默，供增益完整恢复 */
        int active = n - tail;
        for (int i = 0; i < active; ++i) {
            double t = (double)i / sr;
            double tone = sin(2.0 * M_PI * 440.0 * t);
            /* 每 0.25s 一个 80ms 冲击，冲击幅度 1.2，底噪 0.08 */
            double phase = fmod(t, 0.25);
            double hit = (phase < 0.08) ? 1.2 : 0.0;
            in[i] = (float)(0.08 * tone + hit * sin(2.0 * M_PI * 880.0 * t));
        }
        char csv[64];
        snprintf(csv, sizeof csv, "gain_curve_%ds.csv", sec);
        char tag[32];
        snprintf(tag, sizeof tag, "%d 秒素材", sec);
        rc |= run_case(tag, &p, in, n, csv, rep);
        free(in);
    }

    /* 对照：不加处理时逐样本一致 */
    {
        DcParams bp = p; bp.bypass = 1;
        int n = 1024;
        float in[1024], out[1024];
        for (int i = 0; i < n; ++i) in[i] = 0.7f * sinf((float)i * 0.05f);
        DcStats st;
        if (dc_process(&bp, in, out, n, NULL, NULL, &st) != 0) rc = 1;
        int same = 1;
        for (int i = 0; i < n; ++i) if (in[i] != out[i]) { same = 0; break; }
        printf("bypass 逐样本一致: %s\n", same ? "是" : "否");
        fprintf(rep, "bypass 逐样本一致: %s\n", same ? "是" : "否");
    }
    fclose(rep);
    printf("完整报告已写入 demo.txt\n");
    return rc;
}
