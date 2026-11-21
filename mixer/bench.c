#define _POSIX_C_SOURCE 199309L
#include "meter.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* 起峰后读数下降 drop_db 的首穿偏移（采样数），找不到返回 -1 */
static int fall_crossing(const float *sig, int n, int fs, float fall, float dr,
                         int start, float start_peak, float drop_db) {
    LevelMeter m;
    if (meter_init(&m, fs, fall, dr) != 0) return -1;
    float threshold = start_peak * powf(10.0f, -drop_db / 20.0f);
    for (int i = 0; i < n; ++i) {
        float p = meter_update(&m, sig[i]);
        if (i >= start && p <= threshold) return i - start;
    }
    return -1;
}

static void run_case(const char *name, int fs, float fall, float dr,
                     int n, int tone, float amp, float drop_db,
                     const char *curve_path) {
    float *sig = (float *)calloc((size_t)n, sizeof(float));
    if (!sig) { printf("CASE %s ALLOC_FAIL\n", name); return; }
    for (int i = 0; i < tone; ++i) sig[i] = amp;

    LevelMeter m;
    int rc = meter_init(&m, fs, fall, dr);
    if (rc != 0) { printf("CASE %s INIT_FAIL %d %s\n", name, rc, meter_strerror(rc)); free(sig); return; }

    /* 预热一次后计时 */
    LevelMeter warm = m; float wpk;
    meter_process(&warm, sig, n, &wpk);
    double t0 = now_s();
    LevelMeter run = m; float block_peak;
    meter_process(&run, sig, n, &block_peak);
    double t1 = now_s();
    double proc_ns = (t1 - t0) * 1e9;

    /* 回落时间（20 dB）实测 vs 理论；从最后一个保持峰值的采样 tone-1 起算，
     * 经过的采样间隔数 = 首穿采样 - 起算采样，物理时间 = 间隔数/fs。 */
    int dk = fall_crossing(sig, n, fs, fall, dr, tone - 1, amp, drop_db);
    double meas = (double)dk / fs, theory = (double)drop_db / fall;
    double err_s = meas - theory;

    int rows = meter_export_curve(&m, sig, n, 1, curve_path);

    printf("CASE %s\n", name);
    printf("FS %d\n", fs);
    printf("FALL_DB_PER_S %.6f\n", fall);
    printf("DYNAMIC_RANGE_DB %.6f\n", dr);
    printf("ALPHA %.12g\n", m.alpha);
    printf("FLOOR_LIN %.12g\n", m.floor_lin);
    printf("N %d\n", n);
    printf("BLOCK_PEAK_LIN %.12g\n", block_peak);
    printf("FALL_DROP_DB %.6f\n", drop_db);
    printf("FALL_CROSS_SAMPLES %d\n", dk);
    printf("FALL_MEAS_S %.9f\n", meas);
    printf("FALL_THEORY_S %.9f\n", theory);
    printf("FALL_ERR_S %.9f\n", err_s);
    printf("FALL_ERR_MS %.6f\n", err_s * 1000.0);
    printf("FALL_ERR_SAMPLES %.6f\n", err_s * fs);
    printf("PROCESS_NS %.3f\n", proc_ns);
    printf("NS_PER_SAMPLE %.3f\n", proc_ns / n);
    printf("CURVE %s\n", curve_path);
    printf("CURVE_ROWS %d\n", rows);

    /* 空输入报错演示 */
    float pk;
    int e1 = meter_process(&m, sig, 0, &pk);
    int e2 = meter_process(&m, NULL, n, &pk);
    printf("EMPTY_ERR %d %s\n", e1, meter_strerror(e1));
    printf("NULL_ERR %d %s\n", e2, meter_strerror(e2));
    free(sig);
}

int main(void) {
    /* 小图：10 Hz、20 dB/s、60 dB 动态，15 点 */
    run_case("small", 10, 20.0f, 60.0f, 15, 1, 1.0f, 20.0f,
             "build/curve_small.csv");
    /* 大图：48 kHz、33 dB/s、80 dB 动态，96000 点（2 s） */
    run_case("large", 48000, 33.0f, 80.0f, 96000, 12000, 0.8f, 20.0f,
             "build/curve_large.csv");
    return 0;
}
