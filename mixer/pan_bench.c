/* 声像指标：等功率 vs 线性，2 与 8 声道。
 *
 * 对每条声像律、每种声道数，在 21 个位置上逐项测量：
 *   增益误差、左右能量、声像位置（由增益反解）误差、逐样本与双精度解析解的误差、
 *   通道数 / L-R 对数 / 样本数、耗时（代表性位置，取中位数）。
 * 然后另列“两种律的差异”：同一位置上左右增益差、能量差、电平差、
 * 用线性律反解等功率增益得到的位置偏差、以及同一素材下两律输出的逐样本差。
 *
 * 所有解析值都在本文件内用 double 独立计算，与库实现无关；
 * 导出 build/pan_metrics.csv、build/pan_timing.csv、build/pan_diff.csv，
 * 可由 pan/verify/handcalc.py（纯 stdlib）再次独立复算对照。
 */
#define _POSIX_C_SOURCE 199309L
#include "mixer.h"
#include "pan.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef MX_PAN_PI
#define MX_PAN_PI 3.14159265358979323846
#endif

#define FRAMES   4096
#define NPOS     21
#define NLAW     2
#define NCH      2
#define TIMING_POS 5
#define REPS     201   /* 奇数：中位数取正中间 */
#define INNER    64    /* 每次计时连续跑 INNER 遍，压过 clock_gettime 粒度 */

static const int chans[NCH] = { 2, 8 };
static const int timing_pos[TIMING_POS] = { 0, 5, 10, 15, 20 }; /* -1,-0.5,0,+0.5,+1 */

static double now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

/* mx_add 是累加；重跑同一总线前必须清零旧内容。 */
static void clear_bus(MixBus *b) {
    size_t n = (size_t)b->cap * (size_t)b->channels;
    memset(b->buf, 0, n * sizeof(float));
    b->frames = 0;
}

/* 解析增益（double） */
static void analytic_gains(MxPanLaw law, double p, double *gL, double *gR) {
    if (law == MX_PAN_LINEAR) {
        *gL = 0.5 * (1.0 - p);
        *gR = 0.5 * (1.0 + p);
    } else {
        const double theta = (p + 1.0) * (MX_PAN_PI / 4.0);
        *gL = cos(theta);
        *gR = sin(theta);
    }
}

/* 素材：每声道不同相位、不同直流偏置的正弦，便于同时验证“按对分配”
 * 和逐样本解析对照。in[i*ch+c] = bias[c] + 0.4*sin(2*pi*f*i/SR + ph[c]) */
static void make_source(float *buf, int frames, int ch) {
    const double f = 440.0, sr = 48000.0;
    for (int i = 0; i < frames; ++i) {
        for (int c = 0; c < ch; ++c) {
            double ph = 0.37 * (double)(c + 1);
            double bias = 0.05 * (double)(c / 2 + 1);
            buf[(size_t)i * ch + c] =
                (float)(bias + 0.4 * sin(2.0 * MX_PAN_PI * f * (double)i / sr + ph));
        }
    }
}

typedef struct {
    int      law;
    int      ch;
    int      pos_idx;
    double   p;
    double   gL_meas, gR_meas;   /* 由常量输入实测 */
    double   gL_ana, gR_ana;     /* 解析 */
    double   gain_max_err;       /* 每对左右增益的最大绝对误差 */
    double   energy_meas, energy_ana;
    double   energy_err;
    double   pos_est, pos_err;   /* 由实测增益按同一律反解 */
    double   sample_max_err;     /* 逐样本 vs double 解析解 */
    double   sample_rms_err;
    double   sample_max_out;     /* 输出峰值（检查不越界） */
    int      pairs;
} Row;

static void measure_row(Row *r, MixBus *b, float *src, float *work,
                        MxPanLaw law, int ch, int pos_idx) {
    const int pairs = ch / 2;
    memset(r, 0, sizeof *r);
    r->law = law; r->ch = ch; r->pos_idx = pos_idx;
    r->p = -1.0 + 2.0 * (double)pos_idx / (double)(NPOS - 1);
    analytic_gains(law, r->p, &r->gL_ana, &r->gR_ana);
    r->pairs = pairs;
    r->energy_ana = r->gL_ana * r->gL_ana + r->gR_ana * r->gR_ana;

    MxPanCfg cfg = { 1, (int)law, (float)r->p };

    /* --- 常量输入测每对增益（第 c 个采样 = 1 + 0.01*c，互不相同） --- */
    for (int i = 0; i < FRAMES; ++i)
        for (int c = 0; c < ch; ++c)
            src[(size_t)i * ch + c] = 1.0f + 0.01f * (float)c;
    clear_bus(b);
    mx_add(b, src, FRAMES, 1.0f);
    MxPanStats st;
    MxPanErr err;
    if (mx_pan_apply(b, &cfg, &st, &err) != MX_PAN_OK) {
        printf("pan apply failed: %s\n", err.msg);
        exit(1);
    }
    r->pos_est = (double)st.pos_est;
    r->pos_err = fabs(r->pos_est - r->p);

    double gL0 = (double)b->buf[0] / (double)src[0];
    double gR0 = (double)b->buf[1] / (double)src[1];
    /* 其余各对必须与第一对完全一致（增益按对复制） */
    for (int i = 0; i < FRAMES; i += 97) {
        for (int k = 1; k < pairs; ++k) {
            size_t il = (size_t)i * ch + 2 * k;
            double fl = (double)b->buf[il] / (double)src[il];
            double fr = (double)b->buf[il + 1] / (double)src[il + 1];
            if (fabs(fl - gL0) > r->gain_max_err) r->gain_max_err = fabs(fl - gL0);
            if (fabs(fr - gR0) > r->gain_max_err) r->gain_max_err = fabs(fr - gR0);
        }
    }
    r->gL_meas = gL0;
    r->gR_meas = gR0;
    {
        double e = fabs(gL0 - r->gL_ana);
        if (e > r->gain_max_err) r->gain_max_err = e;
        e = fabs(gR0 - r->gR_ana);
        if (e > r->gain_max_err) r->gain_max_err = e;
    }
    r->energy_meas = gL0 * gL0 + gR0 * gR0;
    r->energy_err = fabs(r->energy_meas - r->energy_ana);

    /* --- 正弦素材逐样本对照 double 解析解 --- */
    make_source(src, FRAMES, ch);
    clear_bus(b);
    mx_add(b, src, FRAMES, 1.0f);
    if (mx_pan_apply(b, &cfg, &st, &err) != MX_PAN_OK) {
        printf("pan apply failed: %s\n", err.msg);
        exit(1);
    }
    double sumsq = 0.0;
    for (int i = 0; i < FRAMES; ++i) {
        for (int c = 0; c < ch; ++c) {
            size_t idx = (size_t)i * ch + c;
            double g = (c % 2 == 0) ? r->gL_ana : r->gR_ana;
            double ref = (double)src[idx] * g;
            double d = fabs((double)b->buf[idx] - ref);
            if (d > r->sample_max_err) r->sample_max_err = d;
            if (fabs((double)b->buf[idx]) > r->sample_max_out)
                r->sample_max_out = fabs((double)b->buf[idx]);
            sumsq += d * d;
        }
    }
    r->sample_rms_err = sqrt(sumsq / ((double)FRAMES * (double)ch));
    (void)work;
}

/* 单次 mx_pan_apply 的中位耗时（ns）。只计时管线本身：先按 cfg 的增益
 * 手动从快照恢复缓冲，再计时一次 apply，避免把 memcpy 算进去；内层连续跑
 * INNER 遍、每遍前都恢复，压过 clock_gettime 约 1.8 us 的粒度。 */
static double time_row(MixBus *b, const float *clean, MxPanCfg cfg) {
    double t[REPS];
    size_t bytes = (size_t)b->frames * (size_t)b->channels * sizeof(float);
    float gL = 0.0f, gR = 0.0f;
    if (mx_pan_gains((MxPanLaw)cfg.law, cfg.position, &gL, &gR) != MX_PAN_OK) exit(1);
    for (int r = 0; r < REPS; ++r) {
        double t0 = now_ns();
        for (int k = 0; k < INNER; ++k) {
            memcpy(b->buf, clean, bytes);
            if (mx_pan_apply(b, &cfg, NULL, NULL) != MX_PAN_OK) exit(1);
        }
        t[r] = (now_ns() - t0) / (double)INNER;
    }
    for (int i = 0; i < REPS; ++i)
        for (int j = i + 1; j < REPS; ++j)
            if (t[j] < t[i]) { double tmp = t[i]; t[i] = t[j]; t[j] = tmp; }
    return t[REPS / 2];
}

int main(void) {
    if (system("mkdir -p build") != 0) return 1;
    FILE *fm = fopen("build/pan_metrics.csv", "w");
    FILE *ft = fopen("build/pan_timing.csv", "w");
    FILE *fd = fopen("build/pan_diff.csv", "w");
    if (!fm || !ft || !fd) { printf("cannot write build/pan_*.csv\n"); return 1; }

    fprintf(fm, "# gfx-lab mixer pan metrics\n"
                "law,channels,pairs,frames,position,"
                "gL_meas,gR_meas,gL_ana,gR_ana,gain_max_err,energy_meas,energy_ana,"
                "energy_err,pos_est,pos_err,sample_max_err,sample_rms_err,sample_peak\n");
    fprintf(ft, "# timing: median of %d calls, only the gain-spread stage\n"
                "law,channels,pairs,frames,position,median_ns,ns_per_sample,"
                "samples_per_us\n", REPS);
    fprintf(fd, "# law difference at equal-power request positions\n"
                "position,gL_linear,gR_linear,gL_ep,gR_ep,dgL,dgR,"
                "energy_linear,energy_ep,level_gain_db,pos_linear_reads_ep,pos_read_err,"
                "output_max_diff,output_rms_diff\n");

    float *src = (float*)malloc(sizeof(float) * FRAMES * 8);
    float *clean = (float*)malloc(sizeof(float) * FRAMES * 8);
    if (!src || !clean) return 1;

    Row rows[NLAW][NCH][NPOS];
    double tmed[NLAW][NCH][NPOS];
    MixBus b;
    if (mx_init(&b, FRAMES, 8) != 0) return 1;

    for (int li = 0; li < NLAW; ++li) {
        MxPanLaw law = li == 0 ? MX_PAN_LINEAR : MX_PAN_EQUAL_POWER;
        for (int ci = 0; ci < NCH; ++ci) {
            int ch = chans[ci];
            b.channels = ch;
            for (int pi = 0; pi < NPOS; ++pi) {
                measure_row(&rows[li][ci][pi], &b, src, clean, law, ch, pi);
                Row *r = &rows[li][ci][pi];
                fprintf(fm, "%s,%d,%d,%d,%.6f,%.9f,%.9f,%.9f,%.9f,%.3e,"
                            "%.9f,%.9f,%.3e,%.9f,%.3e,%.3e,%.3e,%.9f\n",
                        mx_pan_law_name(law), ch, r->pairs, FRAMES, r->p,
                        r->gL_meas, r->gR_meas, r->gL_ana, r->gR_ana,
                        r->gain_max_err, r->energy_meas, r->energy_ana,
                        r->energy_err, r->pos_est, r->pos_err,
                        r->sample_max_err, r->sample_rms_err, r->sample_max_out);
                tmed[li][ci][pi] = -1.0;
            }
            /* 代表性位置计时：用常量素材，快照后反复套增益 */
            for (int i = 0; i < FRAMES; ++i)
                for (int c = 0; c < ch; ++c)
                    src[(size_t)i * ch + c] = 1.0f + 0.01f * (float)c;
            clear_bus(&b);
            mx_add(&b, src, FRAMES, 1.0f);
            memcpy(clean, b.buf, (size_t)FRAMES * ch * sizeof(float));
            for (int ti = 0; ti < TIMING_POS; ++ti) {
                int pi = timing_pos[ti];
                float p = (float)(-1.0 + 2.0 * (double)pi / (double)(NPOS - 1));
                MxPanCfg cfg = { 1, (int)law, p };
                double med = time_row(&b, clean, cfg);
                tmed[li][ci][pi] = med;
                double samples = (double)FRAMES * (double)ch;
                fprintf(ft, "%s,%d,%d,%d,%.6f,%.1f,%.4f,%.3f\n",
                        mx_pan_law_name(law), ch, ch / 2, FRAMES, p, med,
                        med / samples, samples / (med / 1e3));
                memcpy(b.buf, clean, (size_t)FRAMES * ch * sizeof(float));
            }
        }
    }

    /* ---- 控制台：逐律逐声道汇总 ---- */
    printf("gfx-lab mixer pan benchmark\n");
    printf("frames=%d, positions=%d (-1..+1), timing=%d calls median, 8ch bus reused\n\n",
           FRAMES, NPOS, REPS);
    for (int li = 0; li < NLAW; ++li) {
        MxPanLaw law = li == 0 ? MX_PAN_LINEAR : MX_PAN_EQUAL_POWER;
        printf("== law=%s ==\n", mx_pan_law_name(law));
        printf("  ch  pairs   gain_err(max)  energy_err(max)  pos_err(max)  "
               "sample_err(max/rms)     peak      median_us  ns/sample\n");
        for (int ci = 0; ci < NCH; ++ci) {
            int ch = chans[ci];
            double ge = 0, ee = 0, pe = 0, smx = 0, srms = 0, pk = 0, tm = -1;
            for (int pi = 0; pi < NPOS; ++pi) {
                Row *r = &rows[li][ci][pi];
                if (r->gain_max_err > ge) ge = r->gain_max_err;
                if (r->energy_err > ee) ee = r->energy_err;
                if (r->pos_err > pe) pe = r->pos_err;
                if (r->sample_max_err > smx) smx = r->sample_max_err;
                if (r->sample_rms_err > srms) srms = r->sample_rms_err;
                if (r->sample_max_out > pk) pk = r->sample_max_out;
                if (tmed[li][ci][pi] > tm) tm = tmed[li][ci][pi];
            }
            double samples = (double)FRAMES * (double)ch;
            printf("  %d    %d     %.3e      %.3e        %.3e      %.3e / %.3e  "
                   "%.6f  %.2f       %.3f\n",
                   ch, ch / 2, ge, ee, pe, smx, srms, pk, tm / 1e3, tm / samples);
        }
        printf("\n");
    }

    /* ---- 两种律的差异：同一请求位置逐项对照 ---- */
    printf("== 两种律的差异（同一请求位置，等功率 - 线性） ==\n");
    printf("  pos    gL_lin   gL_ep    dgL      gR_lin   gR_ep    dgR      "
           "energy_lin energy_ep  level_dB  pos_lin(ep)  read_err\n");
    for (int pi = 0; pi < NPOS; ++pi) {
        Row *rl = &rows[0][0][pi];   /* linear, 2ch */
        Row *re = &rows[1][0][pi];   /* equal-power, 2ch */
        double dgl = re->gL_ana - rl->gL_ana;
        double dgr = re->gR_ana - rl->gR_ana;
        double epl = re->gL_ana * re->gL_ana + re->gR_ana * re->gR_ana;
        double ell = rl->gL_ana * rl->gL_ana + rl->gR_ana * rl->gR_ana;
        /* 把等功率增益按线性律反解会读到什么位置 */
        float pf = 0.0f;
        (void)mx_pan_position(MX_PAN_LINEAR, (float)re->gL_ana, (float)re->gR_ana, &pf);
        double pos_lin_ep = (double)pf;
        double read_err = fabs(pos_lin_ep - re->p);

        /* 同一素材下两律输出的逐样本最大差 / rms 差：
         * 源与两律增益都是解析已知，直接按 x*(g_ep-g_lin) 求差。 */
        double dmax = 0.0, dsumsq = 0.0;
        for (int i = 0; i < FRAMES; ++i)
            for (int c = 0; c < 2; ++c) {
                double ph = 0.37 * (double)(c + 1);
                double bias = 0.05 * (double)(c / 2 + 1);
                double x = bias + 0.4 * sin(2.0 * MX_PAN_PI * 440.0 * (double)i / 48000.0 + ph);
                double dg = (c == 0) ? (re->gL_ana - rl->gL_ana)
                                     : (re->gR_ana - rl->gR_ana);
                double d = fabs(x * dg);
                if (d > dmax) dmax = d;
                dsumsq += d * d;
            }
        double drms = sqrt(dsumsq / ((double)FRAMES * 2.0));
        printf("  %+.2f  %.6f %.6f %+.6f   %.6f %.6f %+.6f   %.6f   %.6f   "
               "%+.4f   %+.6f     %.3e\n",
               re->p, rl->gL_ana, re->gL_ana, dgl, rl->gR_ana, re->gR_ana, dgr,
               ell, epl, 10.0 * log10(epl / ell), pos_lin_ep, read_err);
        fprintf(fd, "%.6f,%.9f,%.9f,%.9f,%.9f,%.6e,%.6e,%.9f,%.9f,%.4f,%.9f,%.3e,%.3e,%.3e\n",
                re->p, rl->gL_ana, rl->gR_ana, re->gL_ana, re->gR_ana, dgl, dgr,
                ell, epl, 10.0 * log10(epl / ell), pos_lin_ep, read_err,
                dmax, drms);
    }
    printf("\n  说明: level_dB = 10*log10(energy_ep/energy_lin)，中点即 +3.0103 dB；\n"
           "        pos_lin(ep) 是把等功率增益按线性律反解得到的位置（端点重合、中间偏移）；\n"
           "        output_*_diff 是同一素材下两律输出的逐样本差（见 build/pan_diff.csv）。\n");
    printf("\nCSV: build/pan_metrics.csv, build/pan_timing.csv, build/pan_diff.csv\n");

    fclose(fm); fclose(ft); fclose(fd);
    mx_free(&b);
    free(src); free(clean);
    return 0;
}
