#include "xfade.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef XF_PI
#define XF_PI 3.14159265358979323846
#endif

const char *xf_error_field(int rc) {
    switch (rc) {
    case XF_E_RATE:     return "sample_rate";
    case XF_E_CHANNELS: return "channels";
    case XF_E_CURVE:
    case XF_E_DURATION: return "fade_ms/curve";
    case XF_E_OVERLAP:  return "fade_ms(overlap)";
    case XF_E_CAP:      return "out_cap_frames";
    case XF_E_NULL:     return "pointer/clip-frames";
    case XF_E_IO:       return "path";
    case XF_E_NOMEM:    return "memory";
    default:            return "none";
    }
}

const char *xf_error_str(int rc) {
    switch (rc) {
    case XF_OK:          return "ok";
    case XF_E_NULL:      return "null pointer or non-positive clip length";
    case XF_E_RATE:      return "sample_rate must be > 0";
    case XF_E_CHANNELS:  return "channels must be > 0";
    case XF_E_CURVE:     return "unknown curve, or fade_ms inconsistent with curve";
    case XF_E_DURATION:   return "fade duration must be > 0 for a real curve";
    case XF_E_OVERLAP:   return "overlap longer than either clip";
    case XF_E_CAP:       return "output buffer too small";
    case XF_E_IO:        return "cannot write curve file";
    case XF_E_NOMEM:     return "allocation failed";
    default:             return "unknown error";
    }
}

const char *xf_curve_name(XfCurve c) {
    switch (c) {
    case XF_NONE:   return "none";
    case XF_LINEAR: return "linear";
    case XF_POWER:  return "equal-power";
    default:        return "invalid";
    }
}

int xf_validate(const XfConfig *cfg) {
    if (!cfg) return XF_E_NULL;
    if (cfg->sample_rate <= 0) return XF_E_RATE;
    if (cfg->channels <= 0) return XF_E_CHANNELS;
    if (cfg->curve != XF_NONE && cfg->curve != XF_LINEAR && cfg->curve != XF_POWER)
        return XF_E_CURVE;
    if (cfg->curve == XF_NONE) {
        /* 不做淡化时不允许偷偷带时长，避免静默语义混淆 */
        if (cfg->fade_ms != 0) return XF_E_CURVE;
        return XF_OK;
    }
    if (cfg->fade_ms <= 0) return XF_E_DURATION;
    return XF_OK;
}

int xf_overlap_frames(const XfConfig *cfg) {
    int rc = xf_validate(cfg);
    if (rc != XF_OK) return rc;
    if (cfg->curve == XF_NONE) return 0;
    long long num = (long long)cfg->fade_ms * cfg->sample_rate;
    long long n = (num + 500LL) / 1000LL; /* 四舍五入，避免依赖浮点 */
    if (n <= 0) return XF_E_DURATION;
    if (n > 0x7fffffffLL) return XF_E_OVERLAP;
    return (int)n;
}

/* ---------- 第 1 段：与采样率无关的权重曲线 ---------- */

int xf_curve_point(XfCurve curve, float t, float *wa, float *wb) {
    if (!wa || !wb) return XF_E_NULL;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    switch (curve) {
    case XF_NONE:
        *wa = 1.0f; *wb = 0.0f;
        return XF_OK;
    case XF_LINEAR:
        *wa = 1.0f - t;
        *wb = t;
        return XF_OK;
    case XF_POWER: {
        float ang = 0.5f * (float)XF_PI * t;
        *wa = cosf(ang);
        *wb = sinf(ang);
        return XF_OK;
    }
    default:
        return XF_E_CURVE;
    }
}

int xf_build_weights(XfCurve curve, int n, float **wa, float **wb) {
    if (!wa || !wb) return XF_E_NULL;
    *wa = NULL; *wb = NULL;
    if (n < 0) return XF_E_DURATION;
    if (curve != XF_NONE && curve != XF_LINEAR && curve != XF_POWER)
        return XF_E_CURVE;
    if (n == 0) return XF_OK;
    float *pa = (float*)malloc((size_t)n * sizeof(float));
    float *pb = (float*)malloc((size_t)n * sizeof(float));
    if (!pa || !pb) { free(pa); free(pb); return XF_E_NOMEM; }
    for (int i = 0; i < n; ++i) {
        float t = (n == 1) ? 1.0f : (float)i / (float)(n - 1);
        int rc = xf_curve_point(curve, t, &pa[i], &pb[i]);
        if (rc != XF_OK) { free(pa); free(pb); return rc; }
    }
    *wa = pa; *wb = pb;
    return XF_OK;
}

int xf_save_curve(const char *path, XfCurve curve, int points) {
    if (!path) return XF_E_NULL;
    if (points <= 0) return XF_E_DURATION;
    if (curve != XF_LINEAR && curve != XF_POWER && curve != XF_NONE)
        return XF_E_CURVE;
    FILE *f = fopen(path, "w");
    if (!f) return XF_E_IO;
    fprintf(f, "i,t,wa,wb,curve\n");
    for (int i = 0; i < points; ++i) {
        float t = (points == 1) ? 1.0f : (float)i / (float)(points - 1);
        float wa, wb;
        xf_curve_point(curve, t, &wa, &wb);
        fprintf(f, "%d,%.8f,%.8f,%.8f,%s\n", i, t, wa, wb, xf_curve_name(curve));
    }
    int bad = ferror(f);
    if (fclose(f) != 0) bad = 1;
    return bad ? XF_E_IO : XF_OK;
}

int xf_export_curve(const char *path, const XfConfig *cfg, int points) {
    int rc = xf_validate(cfg);
    if (rc != XF_OK) return rc;
    return xf_save_curve(path, cfg->curve, points);
}

/* ---------- 第 2 段：逐样本交叉合成 ---------- */

int xf_cross(const float *a, int na, const float *b, int nb, int channels,
             const float *wa, const float *wb, int n, float *out, int out_cap_frames) {
    if (!a || !b || !out || channels <= 0 || na <= 0 || nb <= 0 || n < 0)
        return XF_E_NULL;
    if (n > na || n > nb) return XF_E_OVERLAP;
    if (n > 0 && (!wa || !wb)) return XF_E_NULL;
    int total = na + nb - n;
    if (out_cap_frames < total) return XF_E_CAP;

    int head = na - n;                 /* A 的纯头部帧 */
    for (int i = 0; i < head; ++i)
        for (int c = 0; c < channels; ++c)
            out[(size_t)i * channels + c] = a[(size_t)i * channels + c];

    for (int i = 0; i < n; ++i) {     /* 交叉区：逐样本加权 */
        int oi = head + i;
        const float *af = &a[(size_t)(head + i) * channels];
        const float *bf = &b[(size_t)i * channels];
        float *of = &out[(size_t)oi * channels];
        for (int c = 0; c < channels; ++c)
            of[c] = af[c] * wa[i] + bf[c] * wb[i];
    }

    int tail = nb - n;                 /* B 的纯尾部帧 */
    for (int i = 0; i < tail; ++i) {
        int si = n + i, oi = head + n + i;
        for (int c = 0; c < channels; ++c)
            out[(size_t)oi * channels + c] = b[(size_t)si * channels + c];
    }
    return total;
}

int xf_run(const XfConfig *cfg, const float *a, int na, const float *b, int nb,
           float *out, int out_cap_frames, int *out_n) {
    int rc = xf_validate(cfg);
    if (rc != XF_OK) return rc;
    if (!a || !b || !out || na <= 0 || nb <= 0) return XF_E_NULL;

    int n = xf_overlap_frames(cfg);   /* XF_NONE 时为 0 */
    if (n < 0) return n;
    if (n > na || n > nb) return XF_E_OVERLAP;

    float *wa = NULL, *wb = NULL;
    if (n > 0) {
        rc = xf_build_weights(cfg->curve, n, &wa, &wb);
        if (rc != XF_OK) return rc;
    }
    int total = xf_cross(a, na, b, nb, cfg->channels, wa, wb, n, out, out_cap_frames);
    free(wa); free(wb);
    if (total < 0) return total;
    if (out_n) *out_n = total;
    return XF_OK;
}

/* ---------- 第 3 段：接缝处样本跳变检查 ---------- */

int xf_seam_jump(const float *sig, int frames, int channels, int seam, int radius,
                 float *jump, int *at) {
    if (!sig || !jump || frames < 2 || channels <= 0 || radius < 0)
        return XF_E_NULL;
    /* seam 是接缝左帧，seam+1 必须存在 */
    if (seam < 0 || seam >= frames - 1) return XF_E_NULL;

    /* 检查左帧下标落在 [seam-radius, seam+radius] 内的相邻样本对 */
    int lo = seam - radius;
    int hi = seam + radius;
    if (lo < 0) lo = 0;
    if (hi > frames - 2) hi = frames - 2;

    float best = 0.0f;
    int best_at = lo;
    for (int i = lo; i <= hi; ++i) {
        for (int c = 0; c < channels; ++c) {
            float d = fabsf(sig[(size_t)(i + 1) * channels + c]
                          - sig[(size_t)i * channels + c]);
            if (d > best) { best = d; best_at = i; }
        }
    }
    *jump = best;
    if (at) *at = best_at;
    return XF_OK;
}
