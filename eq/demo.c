#define _POSIX_C_SOURCE 199309L
#include "eq.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SR 48000
#define FRAMES SR /* 1 秒 */

static float db(float x) { return 20.0f * log10f(x); }

/* Goertzel：估计 interleaved 缓冲中声道 k 的单频正弦振幅（取后半段稳态，避开暂态）。
 * 相位形式 s[n]=x[n]+2cos(w)s[n-1]-s[n-2]；P=s1^2+s2^2-2cos(w)s1*s2，
 * 整数周期时振幅 A = sqrt(4P)/N（已用已知 0.25 纯音标定，误差 < 0.004 dB）。 */
static float goertzel_amp(const float *x, int frames, int ch, int k, float freq) {
    float w = 2.0f * (float)M_PI * freq / (float)SR;
    float cw = 2.0f * cosf(w);
    float s1 = 0.0f, s2 = 0.0f;
    int start = frames / 2;
    for (int n = start; n < frames; ++n) {
        float v = x[(long)n * ch + k];
        float s0 = v + cw * s1 - s2;
        s2 = s1; s1 = s0;
    }
    int cnt = frames - start;
    float power = s1 * s1 + s2 * s2 - cw * s1 * s2;
    if (power < 0.0f) power = 0.0f;
    return sqrtf(4.0f * power) / (float)cnt;
}

static void gen_signal(float *buf, int frames, int ch) {
    const float f[3] = {80.0f, 1000.0f, 10000.0f};
    const float a[3] = {0.25f, 0.25f, 0.25f};
    for (int n = 0; n < frames; ++n) {
        for (int k = 0; k < ch; ++k) {
            float ph = (float)k * 0.37f; /* 声道间相位差，保持各声道独立 */
            float v = 0.0f;
            for (int t = 0; t < 3; ++t)
                v += a[t] * sinf(2.0f * (float)M_PI * f[t] * (float)n / SR + ph * (float)(t + 1));
            buf[(long)n * ch + k] = v;
        }
    }
}

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void write_wav(const char *path, const float *buf, int frames, int ch) {
    FILE *f = fopen(path, "wb");
    if (!f) { printf("cannot write %s\n", path); return; }
    unsigned int data_bytes = (unsigned int)(frames * ch * 2);
    unsigned int riff = 36 + data_bytes;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    unsigned int sz = 16; fwrite(&sz, 4, 1, f);
    unsigned short fmt = 1, c = (unsigned short)ch;
    unsigned int rate = SR, byte_rate = rate * c * 2;
    unsigned short align = (unsigned short)(c * 2), bits = 16;
    fwrite(&fmt, 2, 1, f); fwrite(&c, 2, 1, f); fwrite(&rate, 4, 1, f);
    fwrite(&byte_rate, 4, 1, f); fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data_bytes, 4, 1, f);
    for (long i = 0; i < (long)frames * ch; ++i) {
        float v = buf[i];
        if (v > 1.0f) v = 1.0f;
        if (v < -1.0f) v = -1.0f;
        short s = (short)lrintf(v * 32767.0f);
        fwrite(&s, 2, 1, f);
    }
    fclose(f);
}

static void run_case(int ch, float glo, float gmid, float ghi) {
    EqConfig cfg = { SR, ch, 1, 250.0f, 4000.0f, glo, gmid, ghi };
    EqState s;
    int rc = eq_init(&s, &cfg);
    if (rc != EQ_OK) { printf("init error: %s\n", eq_strerror(rc)); return; }

    float *in = malloc((size_t)FRAMES * ch * sizeof(float));
    float *out = malloc((size_t)FRAMES * ch * sizeof(float));
    gen_signal(in, FRAMES, ch);

    double t0 = now_sec();
    eq_process(&s, in, out, FRAMES);
    double t1 = now_sec();

    const float freqs[3] = {80.0f, 1000.0f, 10000.0f};
    /* 理论链路幅频：复域按真实信号流求和（eq_response 内部处理支路相位），
     * 不能对各节独立幅度相乘——串联支路与互补相减都带相位。 */
    float mag[3];
    for (int t = 0; t < 3; ++t) {
        float re, im;
        if (eq_response(&s, freqs[t], &re, &im) == EQ_OK)
            mag[t] = hypotf(re, im);
        else
            mag[t] = 0.0f / 0.0f;
    }

    /* 增益误差用纯音单独测：每频点一条独立状态，避免多音邻频泄漏。
     * Goertzel 已用已知振幅标定，直接取 输出振幅/输入振幅(0.25) 得实测链路幅频。 */
    float meas_mag[3], gain_err[3];
    for (int t = 0; t < 3; ++t) {
        EqState ts;
        EqConfig tcfg = cfg;
        meas_mag[t] = 0.0f / 0.0f; gain_err[t] = meas_mag[t];
        if (eq_init(&ts, &tcfg) != EQ_OK) continue;
        float *ti = malloc((size_t)FRAMES * ch * sizeof(float));
        float *to = malloc((size_t)FRAMES * ch * sizeof(float));
        for (int n = 0; n < FRAMES; ++n)
            for (int k = 0; k < ch; ++k)
                ti[(long)n * ch + k] = 0.25f *
                    sinf(2.0f * (float)M_PI * freqs[t] * (float)n / SR);
        eq_process(&ts, ti, to, FRAMES);
        float a_out = goertzel_amp(to, FRAMES, ch, 0, freqs[t]);
        meas_mag[t] = a_out / 0.25f;
        gain_err[t] = db(meas_mag[t] / mag[t]);
        free(ti); free(to);
        eq_free(&ts);
    }

    float pin[EQ_MAX_CHANNELS], pout[EQ_MAX_CHANNELS]; long ov[EQ_MAX_CHANNELS];
    eq_stats_get(&s, pin, pout, ov);

    printf("=== %d 声道，增益 低%.2f 中%.2f 高%.2f ===\n", ch, glo, gmid, ghi);
    printf("band  freq(Hz)  手算期望幅频   纯音实测幅频   增益误差(dB)\n");
    long total_ov = 0;
    for (int t = 0; t < 3; ++t) {
        printf("%s %8.0f  %12.5f  %12.5f  %8.3f\n",
               t == 0 ? "low " : t == 1 ? "mid " : "high",
               freqs[t], mag[t], meas_mag[t], gain_err[t]);
    }
    for (int k = 0; k < ch; ++k) {
        printf("  ch%d: peak_in=%.4f peak_out=%.4f 峰值变化=%+.4f 过载=%ld\n",
               k, pin[k], pout[k], pout[k] - pin[k], ov[k]);
        total_ov += ov[k];
    }
    printf("  过载总数=%ld  耗时=%.3f ms (%.1f 样本/µs)\n",
           total_ov, (t1 - t0) * 1e3,
           (double)FRAMES * ch / ((t1 - t0) * 1e6));
    if (ch == 2) {
        eq_export_coef(&s, "demo_coef.csv");
        write_wav("demo_eq.wav", out, FRAMES, ch);
    }
    free(in); free(out);
    eq_free(&s);
}

int main(void) {
    /* 手算参考（SR=48000, f_lo=250, f_hi=4000）：
     *   w_lo = tan(pi*250/48000) = 0.016366, a1_lo = (w-1)/(w+1) = -0.96774
     *   w_hi = tan(pi*4000/48000) = 0.26795,  a1_hi = -0.57735
     * 单节幅频: 80Hz |L_lo|=0.9524 |H_lo|=0.3048；10kHz |H_lo|=0.9987 |H_hi|=0.9285
     * 注意总响应必须复域求和（见 eq_response），各支路相位不同，幅度不能直接相加。
     * 增益(0.75,1.5,0.5)下复域总幅频：80Hz=0.8530 1kHz=1.4581 10kHz=0.6909
     */
    run_case(2, 0.75f, 1.5f, 0.5f);   /* 低频太闷->衰减；高频刺->衰减；中段提升 */
    run_case(6, 0.75f, 1.5f, 0.5f);
    run_case(2, 16.0f, 16.0f, 16.0f); /* 重增益：制造并统计过载 */

    /* 参数错误演示：逐字段报错 */
    EqConfig bad = { SR, 2, 1, 5000.0f, 4000.0f, 1.0f, 1.0f, 1.0f };
    int rc = eq_validate(&bad);
    printf("倒置边界 f_lo=5000 > f_hi=4000 -> code %d: %s\n", rc, eq_strerror(rc));
    bad = (EqConfig){ SR, 2, 1, 250.0f, 4000.0f, 1.0f, 20.0f, 1.0f };
    rc = eq_validate(&bad);
    printf("增益过重 g_mid=20 -> code %d: %s\n", rc, eq_strerror(rc));
    return 0;
}
