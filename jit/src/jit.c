#include "jit.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static _Thread_local char jt_err[256];

static int jt_fail(int code, const char *fmt, const char *arg) {
    if (fmt && arg) snprintf(jt_err, sizeof(jt_err), fmt, arg);
    else if (fmt) snprintf(jt_err, sizeof(jt_err), "%s", fmt);
    else jt_err[0] = '\0';
    return code;
}

const char *jit_strerror(int code) {
    switch (code) {
        case JIT_OK:     return "ok";
        case JIT_EINVAL: return "invalid parameter";
        case JIT_ENOMEM: return "out of memory";
        case JIT_ERANGE: return "offset out of range / output too small";
        case JIT_EIO:    return "i/o error";
        default:         return "unknown error";
    }
}

const char *jit_last_error(void) { return jt_err; }

int jit_plan_init(JitPlan *p, int N, int src_len, int period, double amp, unsigned seed) {
    if (!p) return jt_fail(JIT_EINVAL, "parameter '%s' is NULL", "plan");
    memset(p, 0, sizeof(*p));
    if (N <= 0)       return jt_fail(JIT_EINVAL, "parameter '%s' must be > 0", "N");
    if (src_len <= 0) return jt_fail(JIT_EINVAL, "parameter '%s' must be > 0 (zero-length material)", "src_len");
    if (period < 0)   return jt_fail(JIT_EINVAL, "parameter '%s' must be >= 0", "period");
    if (amp < 0.0 || !isfinite(amp))
        return jt_fail(JIT_EINVAL, "parameter '%s' must be a finite value >= 0", "amp");

    p->frac = (double*)malloc((size_t)N * sizeof(double));
    p->integer = (int*)malloc((size_t)N * sizeof(int));
    p->weights = (double*)malloc((size_t)N * sizeof(double));
    if (!p->frac || !p->integer || !p->weights) {
        jit_plan_free(p);
        return jt_fail(JIT_ENOMEM, "allocation failed in '%s'", "jit_plan_init");
    }
    p->N = N; p->src_len = src_len; p->period = period;
    p->amp = amp; p->seed = seed;
    for (int k = 0; k < N; ++k) p->weights[k] = 1.0 / (double)N;
    return JIT_OK;
}

void jit_plan_free(JitPlan *p) {
    if (!p) return;
    free(p->frac); free(p->integer); free(p->weights);
    p->frac = NULL; p->integer = NULL; p->weights = NULL;
    p->N = p->src_len = p->period = 0;
    p->have_frac = p->have_int = 0;
}

static int jt_ceil_safe(double x) {
    double c = ceil(x);
    if (c > 2.1e9) return 2100000000;
    return (int)c;
}

int jit_out_len(const JitPlan *p) {
    if (!p || p->N <= 0 || p->src_len <= 0) return -1;
    int a = jt_ceil_safe(p->amp);
    long long L = (long long)(p->N - 1) * p->period + 2L * a + p->src_len;
    if (L > 2100000000L) return -1;
    return (int)L;
}

/* xorshift32，确定性抖动序列 */
static unsigned xorshift32(unsigned *s) {
    unsigned x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x;
    return x;
}

/* 半数远离零：round-half-away-from-zero（手工序列可逐值对照） */
static int round_hafz(double x) {
    return (int)trunc(copysign(fabs(x) + 0.5, x));
}

/* 阶段 1：按抖动序列偏移采样点（连续偏移） */
int jit_offsets(JitPlan *p) {
    if (!p) return jt_fail(JIT_EINVAL, "parameter '%s' is NULL", "plan");
    if (p->N <= 0 || p->src_len <= 0)
        return jt_fail(JIT_EINVAL, "plan fields '%s' invalid (run jit_plan_init first)", "N/src_len");
    unsigned s = p->seed ? p->seed : 1u;
    for (int k = 0; k < p->N; ++k) {
        double jitter = 0.0;
        if (p->amp > 0.0) {
            double u = (xorshift32(&s) >> 8) / 16777216.0; /* [0,1)，24 bit */
            jitter = (u * 2.0 - 1.0) * p->amp;             /* [-A,A] 均匀 */
        }
        p->frac[k] = (double)k * (double)p->period + jitter;
    }
    p->have_frac = 1;
    p->have_int = 0;
    return JIT_OK;
}

/* 阶段 2：按偏移取整，并校验整数落点的包络是否越界 */
int jit_quantize(JitPlan *p) {
    if (!p) return jt_fail(JIT_EINVAL, "parameter '%s' is NULL", "plan");
    if (!p->have_frac)
        return jt_fail(JIT_EINVAL, "stage order: run '%s' before quantize", "jit_offsets");
    int a = jt_ceil_safe(p->amp);
    int lo = -a, hi = (p->N - 1) * p->period + a;
    for (int k = 0; k < p->N; ++k) {
        int q = round_hafz(p->frac[k]);
        if (q < lo || q > hi) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "offset out of range: integer[%d]=%d, envelope=[%d,%d] (check parameters amp/period/N)",
                     k, q, lo, hi);
            return jt_fail(JIT_ERANGE, "%s", msg);
        }
        p->integer[k] = q;
    }
    p->have_int = 1;
    return JIT_OK;
}

static int synth_common(const JitPlan *p, const float *src, int cap, int want_int, int L) {
    if (!p)     return jt_fail(JIT_EINVAL, "parameter '%s' is NULL", "plan");
    if (!src)   return jt_fail(JIT_EINVAL, "parameter '%s' is NULL", "src");
    if (p->src_len <= 0)
        return jt_fail(JIT_EINVAL, "parameter '%s' must be > 0 (zero-length material)", "src_len");
    if (want_int && !p->have_int)
        return jt_fail(JIT_EINVAL, "stage order: run '%s' before synthesize", "jit_quantize");
    if (!want_int && !p->have_frac)
        return jt_fail(JIT_EINVAL, "stage order: run '%s' first", "jit_offsets");
    if (L <= 0)
        return jt_fail(JIT_ERANGE, "computed '%s' is invalid", "out_len");
    if (cap < L) {
        char msg[256];
        snprintf(msg, sizeof(msg), "output cap=%d smaller than required out_len=%d (parameter '%s')",
                 cap, L, "cap");
        return jt_fail(JIT_ERANGE, "%s", msg);
    }
    return JIT_OK;
}

/* 阶段 3：按权重合成（整数落点） */
int jit_synthesize(const JitPlan *p, const float *src, float *out, int cap) {
    int L = jit_out_len(p);
    int rc = synth_common(p, src, cap, 1, L);
    if (rc != JIT_OK) return rc;
    int a = jt_ceil_safe(p->amp);
    memset(out, 0, (size_t)L * sizeof(float));
    for (int k = 0; k < p->N; ++k) {
        long long start = (long long)p->integer[k] + a;
        if (start < 0 || start + (long long)p->src_len > (long long)L) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "offset out of range: voice %d start=%lld, out_len=%d (check parameter amp/period)",
                     k, start, L);
            return jt_fail(JIT_ERANGE, "%s", msg);
        }
        double w = p->weights[k];
        for (int i = 0; i < p->src_len; ++i)
            out[start + i] += (float)(w * (double)src[i]);
    }
    return JIT_OK;
}

/* 连续参考：线性插值（不取整），与阶段 3 同包络、同长度 */
int jit_synthesize_frac(const JitPlan *p, const float *src, float *out, int cap) {
    int L = jit_out_len(p);
    int rc = synth_common(p, src, cap, 0, L);
    if (rc != JIT_OK) return rc;
    int a = jt_ceil_safe(p->amp);
    int n = p->src_len;
    memset(out, 0, (size_t)L * sizeof(float));
    for (int k = 0; k < p->N; ++k) {
        double pos = p->frac[k] + a;
        long long i0 = (long long)floor(pos);
        double t = pos - (double)i0;
        if (i0 < 0 || i0 + n >= (long long)L + 1) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "offset out of range: voice %d frac=%.6f, out_len=%d (check parameter amp/period)",
                     k, p->frac[k], L);
            return jt_fail(JIT_ERANGE, "%s", msg);
        }
        double w = p->weights[k];
        for (int i = 0; i < n; ++i) {
            double v0 = (double)src[i];
            double v1 = (i + 1 < n) ? (double)src[i + 1] : (double)src[i]; /* 末尾钳位 */
            out[i0 + i] += (float)(w * (v0 + t * (v1 - v0)));
        }
    }
    return JIT_OK;
}

int jit_jitter_count(const JitPlan *p) {
    if (!p || !p->have_int) return -1;
    int c = 0;
    for (int k = 0; k < p->N; ++k)
        if (p->integer[k] != k * p->period) ++c;
    return c;
}

int jit_export_csv(const JitPlan *p, FILE *fp) {
    if (!p) return jt_fail(JIT_EINVAL, "parameter '%s' is NULL", "plan");
    if (!fp) return jt_fail(JIT_EINVAL, "parameter '%s' is NULL", "fp");
    if (!p->have_frac)
        return jt_fail(JIT_EINVAL, "stage order: run '%s' before export", "jit_offsets");
    fprintf(fp, "voice,base,frac,jitter,jitter_q,integer,weight\n");
    for (int k = 0; k < p->N; ++k) {
        double base = (double)k * (double)p->period;
        double jf = p->frac[k] - base;
        int jq = p->have_int ? p->integer[k] - k * p->period : 0;
        int iv = p->have_int ? p->integer[k] : 0;
        fprintf(fp, "%d,%.17g,%.17g,%.17g,%d,%d,%.17g\n",
                k, base, p->frac[k], jf, jq, iv, p->weights[k]);
    }
    if (ferror(fp)) return jt_fail(JIT_EIO, "write failed in '%s'", "jit_export_csv");
    return JIT_OK;
}

/* ---- 梳状染色指标：对各 voice 位置的单位脉冲响应做 FFT ---- */

static int next_pow2(int n) {
    int m = 1;
    while (m < n) m <<= 1;
    return m;
}

static void fft_radix2(double *re, double *im, int n) {
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            double tr = re[i]; re[i] = re[j]; re[j] = tr;
            double ti = im[i]; im[i] = im[j]; im[j] = ti;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double ang = -2.0 * M_PI / (double)len;
        double wr = cos(ang), wi = sin(ang);
        for (int i = 0; i < n; i += len) {
            double cr = 1.0, ci = 0.0;
            for (int j = 0; j < len / 2; ++j) {
                double ur = re[i + j], ui = im[i + j];
                double vr = re[i + j + len / 2] * cr - im[i + j + len / 2] * ci;
                double vi = re[i + j + len / 2] * ci + im[i + j + len / 2] * cr;
                re[i + j] = ur + vr; im[i + j] = ui + vi;
                re[i + j + len / 2] = ur - vr; im[i + j + len / 2] = ui - vi;
                double ncr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = ncr;
            }
        }
    }
}

int jit_comb_metric(const JitPlan *p, JitCombMetric *m) {
    if (!p) return jt_fail(JIT_EINVAL, "parameter '%s' is NULL", "plan");
    if (!m) return jt_fail(JIT_EINVAL, "parameter '%s' is NULL", "metric");
    if (!p->have_int)
        return jt_fail(JIT_EINVAL, "stage order: run '%s' before comb metric", "jit_quantize");

    int L = jit_out_len(p);
    if (L <= 0) return jt_fail(JIT_ERANGE, "computed '%s' is invalid", "out_len");
    int nfft = next_pow2(L);
    double *re = (double*)calloc((size_t)nfft, sizeof(double));
    double *im = (double*)calloc((size_t)nfft, sizeof(double));
    if (!re || !im) {
        free(re); free(im);
        return jt_fail(JIT_ENOMEM, "allocation failed in '%s'", "jit_comb_metric");
    }
    int a = jt_ceil_safe(p->amp);
    for (int k = 0; k < p->N; ++k) {
        long long idx = (long long)p->integer[k] + a;
        if (idx < 0 || idx >= nfft) {
            free(re); free(im);
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "offset out of range: voice %d index=%lld nfft=%d (check parameter amp/period)",
                     k, idx, nfft);
            return jt_fail(JIT_ERANGE, "%s", msg);
        }
        re[idx] += p->weights[k]; /* 单位脉冲，强度按权重 */
    }
    fft_radix2(re, im, nfft);

    int bins = nfft / 2;              /* 去掉直流 bin0，取正频率 */
    double mx = 0.0, sum = 0.0, log_sum = 0.0, arith = 0.0;
    int peak_bin = 0, above_half = 0;
    for (int b = 1; b <= bins; ++b) {
        double mag = sqrt(re[b] * re[b] + im[b] * im[b]);
        if (mag > mx) { mx = mag; peak_bin = b; }
        sum += mag;
        double pw = mag * mag;
        arith += pw;
        log_sum += log(pw > 1e-300 ? pw : 1e-300);
    }
    int cnt = bins > 0 ? bins : 1;
    double mean = sum / (double)cnt;
    double geo = exp(log_sum / (double)cnt);
    arith /= (double)cnt;
    double half = 0.5 * mx;
    for (int b = 1; b <= bins; ++b) {
        double mag = sqrt(re[b] * re[b] + im[b] * im[b]);
        if (mag >= half) ++above_half;
    }

    m->nfft = nfft;
    m->ir_len = L;
    m->bins = bins;
    m->peak_bin = peak_bin;
    m->above_half = above_half;
    m->crest_db = 20.0 * log10((mx > 0 ? mx : 1e-300) / (mean > 0 ? mean : 1e-300));
    m->flatness = arith > 0 ? geo / arith : 0.0;

    free(re); free(im);
    return JIT_OK;
}
