#include "eq.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

const char *eq_strerror(int code) {
    switch (code) {
        case EQ_OK:           return "ok";
        case EQ_ERR_NULL:     return "null pointer";
        case EQ_ERR_RATE:     return "sample_rate must be > 0";
        case EQ_ERR_CHANNELS: return "channels must be in [1, EQ_MAX_CHANNELS]";
        case EQ_ERR_ENABLE:   return "enabled must be 0 or 1";
        case EQ_ERR_FLO:      return "f_lo: must be > 0 and below f_hi";
        case EQ_ERR_FHI:      return "f_hi: must be above f_lo and below Nyquist (sample_rate/2)";
        case EQ_ERR_GAIN_LO:  return "g_lo: gain must be finite and in [0, EQ_MAX_GAIN]";
        case EQ_ERR_GAIN_MID: return "g_mid: gain must be finite and in [0, EQ_MAX_GAIN]";
        case EQ_ERR_GAIN_HI:  return "g_hi: gain must be finite and in [0, EQ_MAX_GAIN]";
        case EQ_ERR_FRAMES:   return "frames must be >= 0";
        default:              return "unknown error";
    }
}

int eq_validate(const EqConfig *c) {
    if (!c) return EQ_ERR_NULL;
    if (!(c->sample_rate > 0)) return EQ_ERR_RATE;
    if (c->channels < 1 || c->channels > EQ_MAX_CHANNELS) return EQ_ERR_CHANNELS;
    if (c->enabled != 0 && c->enabled != 1) return EQ_ERR_ENABLE;
    /* 频段边界倒置（含 f_lo>=f_hi）与越界检查 */
    if (!(c->f_lo > 0.0f) || !(c->f_lo < c->f_hi)) return EQ_ERR_FLO;
    if (!(c->f_hi < 0.5f * (float)c->sample_rate)) return EQ_ERR_FHI;
    if (!isfinite(c->g_lo) || c->g_lo < 0.0f || c->g_lo > EQ_MAX_GAIN) return EQ_ERR_GAIN_LO;
    if (!isfinite(c->g_mid) || c->g_mid < 0.0f || c->g_mid > EQ_MAX_GAIN) return EQ_ERR_GAIN_MID;
    if (!isfinite(c->g_hi) || c->g_hi < 0.0f || c->g_hi > EQ_MAX_GAIN) return EQ_ERR_GAIN_HI;
    return EQ_OK;
}

/* 一阶低通（双线性变换，无频率预扭曲以外的近似）：
 * H(z) = (b0 + b1 z^-1) / (1 + a1 z^-1)，其中 b0=b1=(1+a1)/2，
 * DC 增益 1，Nyquist 增益 0；在截止频点幅频响应恰为 -3.01 dB。 */
static EqCoef make_lpf(float fc, int sr) {
    EqCoef q;
    float w = tanf((float)M_PI * fc / (float)sr);
    q.a1 = (w - 1.0f) / (w + 1.0f);
    q.b0 = 0.5f * (1.0f + q.a1);
    q.b1 = q.b0;
    return q;
}

/* 一阶高通 = 1 - H_lpf：
 * b0hp = 1-b0_lp, b1hp = a1-b1_lp, 分母相同（复数域严格互补 H_lp+H_hp=1） */
static EqCoef make_hpf(const EqCoef *lp) {
    EqCoef q;
    q.a1 = lp->a1;
    q.b0 = 1.0f - lp->b0;
    q.b1 = lp->a1 - lp->b1;
    return q;
}

static float lpf_step(const EqCoef *q, float x, float *px, float *py) {
    float y = q->b0 * x + q->b1 * *px - q->a1 * *py;
    *px = x;
    *py = y;
    return y;
}

int eq_init(EqState *s, const EqConfig *c) {
    if (!s || !c) return EQ_ERR_NULL;
    s->ready = 0; /* 校验失败即保持未就绪 */
    int rc = eq_validate(c);
    if (rc != EQ_OK) return rc;
    int n = c->channels;
    s->x1 = (float*)calloc((size_t)2 * (size_t)n, sizeof(float));
    s->y1 = (float*)calloc((size_t)2 * (size_t)n, sizeof(float));
    s->peak_in = (float*)calloc((size_t)n, sizeof(float));
    s->peak_out = (float*)calloc((size_t)n, sizeof(float));
    s->overload = (long*)calloc((size_t)n, sizeof(long));
    if (!s->x1 || !s->y1 || !s->peak_in || !s->peak_out || !s->overload) {
        free(s->x1); free(s->y1); free(s->peak_in); free(s->peak_out); free(s->overload);
        s->x1 = s->y1 = s->peak_in = s->peak_out = NULL; s->overload = NULL;
        s->ready = 0;
        return EQ_ERR_NULL;
    }
    s->cfg = *c;
    EqCoef l1 = make_lpf(c->f_lo, c->sample_rate);
    EqCoef l2 = make_lpf(c->f_hi, c->sample_rate);
    s->bank.lp_lo = l1;
    s->bank.hp_lo = make_hpf(&l1);
    s->bank.lp_hi = l2;
    s->bank.hp_hi = make_hpf(&l2);
    s->samples = 0;
    s->ready = 1;
    return EQ_OK;
}

void eq_free(EqState *s) {
    if (!s) return;
    free(s->x1); free(s->y1); free(s->peak_in); free(s->peak_out); free(s->overload);
    s->x1 = s->y1 = s->peak_in = s->peak_out = NULL; s->overload = NULL;
    s->ready = 0;
}

int eq_reset(EqState *s) {
    if (!s || !s->ready) return EQ_ERR_NULL;
    int n = s->cfg.channels;
    for (int i = 0; i < 2 * n; ++i) { s->x1[i] = 0.0f; s->y1[i] = 0.0f; }
    for (int c = 0; c < n; ++c) {
        s->peak_in[c] = s->peak_out[c] = 0.0f;
        s->overload[c] = 0;
    }
    s->samples = 0;
    return EQ_OK;
}

int eq_process(EqState *s, const float *in, float *out, int frames) {
    if (!s || !s->ready || !in || !out) return EQ_ERR_NULL;
    if (frames < 0) return EQ_ERR_FRAMES;
    const int ch = s->cfg.channels;
    const EqBank *b = &s->bank;
    const EqConfig *c = &s->cfg;

    if (!c->enabled) {
        /* 旁路：逐样本一致（含原地处理） */
        for (int i = 0; i < frames * ch; ++i) {
            float v = in[i];
            out[i] = v;
            int cc = i % ch;
            float a = fabsf(v);
            if (a > s->peak_in[cc]) s->peak_in[cc] = a;
            if (a > s->peak_out[cc]) s->peak_out[cc] = a;
            if (a > 1.0f) s->overload[cc]++;
        }
        s->samples += (long)frames * ch;
        return frames;
    }

    for (int i = 0; i < frames; ++i) {
        for (int k = 0; k < ch; ++k) {
            long idx = (long)i * ch + k;
            float x = in[idx];
            /* 第一节：f_lo 低通，低频段；高通支路为互补相减 */
            float low = lpf_step(&b->lp_lo, x, &s->x1[k], &s->y1[k]);
            float high = x - low;
            /* 第二节：在高通支路上做 f_hi 低通，中频段；高频段再次互补相减 */
            float mid = lpf_step(&b->lp_hi, high, &s->x1[ch + k], &s->y1[ch + k]);
            float treble = high - mid;
            float y = c->g_lo * low + c->g_mid * mid + c->g_hi * treble;
            out[idx] = y;

            float ai = fabsf(x), ao = fabsf(y);
            if (ai > s->peak_in[k]) s->peak_in[k] = ai;
            if (ao > s->peak_out[k]) s->peak_out[k] = ao;
            if (ao > 1.0f) s->overload[k]++;
        }
    }
    s->samples += (long)frames * ch;
    return frames;
}

int eq_export_coef(const EqState *s, const char *path) {
    if (!s || !s->ready || !path) return EQ_ERR_NULL;
    FILE *f = fopen(path, "w");
    if (!f) return EQ_ERR_NULL;
    const EqConfig *c = &s->cfg;
    fprintf(f, "# eq3 coefficients, sample_rate=%d channels=%d f_lo=%.6f f_hi=%.6f\n",
            c->sample_rate, c->channels, c->f_lo, c->f_hi);
    fprintf(f, "# section,role,cutoff_hz,b0,b1,a1\n");
    const EqBank *b = &s->bank;
    fprintf(f, "1,lp_lo,%.6f,%.9g,%.9g,%.9g\n", c->f_lo, b->lp_lo.b0, b->lp_lo.b1, b->lp_lo.a1);
    fprintf(f, "2,hp_lo,%.6f,%.9g,%.9g,%.9g\n", c->f_lo, b->hp_lo.b0, b->hp_lo.b1, b->hp_lo.a1);
    fprintf(f, "3,lp_hi,%.6f,%.9g,%.9g,%.9g\n", c->f_hi, b->lp_hi.b0, b->lp_hi.b1, b->lp_hi.a1);
    fprintf(f, "4,hp_hi,%.6f,%.9g,%.9g,%.9g\n", c->f_hi, b->hp_hi.b0, b->hp_hi.b1, b->hp_hi.a1);
    fclose(f);
    return EQ_OK;
}

float eq_mag(const EqCoef *cf, float freq_hz, int sample_rate) {
    if (!cf || sample_rate <= 0) return 0.0f;
    float w = 2.0f * (float)M_PI * freq_hz / (float)sample_rate;
    float cw = cosf(w), sw = sinf(w);
    /* |H(e^jw)| = |b0 + b1 e^-jw| / |1 + a1 e^-jw| */
    float num_re = cf->b0 + cf->b1 * cw, num_im = -cf->b1 * sw;
    float den_re = 1.0f + cf->a1 * cw, den_im = -cf->a1 * sw;
    return sqrtf((num_re * num_re + num_im * num_im) /
                 (den_re * den_re + den_im * den_im));
}

/* 单节复频响 H(e^jw)，结果写入 re/im */
static void cf_response(const EqCoef *cf, float w, float *re, float *im) {
    float cw = cosf(w), sw = sinf(w);
    float nr = cf->b0 + cf->b1 * cw, ni = -cf->b1 * sw;
    float dr = 1.0f + cf->a1 * cw, di = -cf->a1 * sw;
    float d2 = dr * dr + di * di;
    *re = (nr * dr + ni * di) / d2;
    *im = (ni * dr - nr * di) / d2;
}

int eq_response(const EqState *s, float freq_hz, float *re, float *im) {
    if (!s || !s->ready || !re || !im) return EQ_ERR_NULL;
    if (!(freq_hz >= 0.0f) || freq_hz > 0.5f * (float)s->cfg.sample_rate) return EQ_ERR_FHI;
    const EqBank *b = &s->bank;
    float w = 2.0f * (float)M_PI * freq_hz / (float)s->cfg.sample_rate;
    float lr, li, hr, hi2, mr, mi, tr, ti;
    /* low = L_lo(x); high = H_lo(x); mid = L_hi(high); treble = H_hi(high) */
    cf_response(&b->lp_lo, w, &lr, &li);
    cf_response(&b->hp_lo, w, &hr, &hi2);
    cf_response(&b->lp_hi, w, &mr, &mi);
    cf_response(&b->hp_hi, w, &tr, &ti);
    float mid_r = mr * hr - mi * hi2, mid_i = mr * hi2 + mi * hr;
    float tre_r = tr * hr - ti * hi2, tre_i = tr * hi2 + ti * hr;
    *re = s->cfg.g_lo * lr + s->cfg.g_mid * mid_r + s->cfg.g_hi * tre_r;
    *im = s->cfg.g_lo * li + s->cfg.g_mid * mid_i + s->cfg.g_hi * tre_i;
    return EQ_OK;
}

const EqBank *eq_bank(const EqState *s) { return (s && s->ready) ? &s->bank : NULL; }
const EqConfig *eq_config_get(const EqState *s) { return (s && s->ready) ? &s->cfg : NULL; }

int eq_stats_get(const EqState *s, float *peak_in, float *peak_out, long *overloads) {
    if (!s || !s->ready) return EQ_ERR_NULL;
    int n = s->cfg.channels;
    for (int k = 0; k < n; ++k) {
        if (peak_in) peak_in[k] = s->peak_in[k];
        if (peak_out) peak_out[k] = s->peak_out[k];
        if (overloads) overloads[k] = s->overload[k];
    }
    return n;
}
