#include "dcomp.h"
#include <math.h>
#include <stdlib.h>

static int dc_fail(const char *field, const char *reason) {
    fprintf(stderr, "dcomp: invalid parameter '%s': %s\n", field, reason);
    return -1;
}

int dc_validate(const DcParams *p) {
    if (!p) return dc_fail("params", "null pointer");
    if (!(p->threshold > 0.0f))
        return dc_fail("threshold", "must be > 0 (zero/negative threshold not allowed)");
    if (p->threshold > 1.0f)
        return dc_fail("threshold", "must be <= 1.0");
    if (isnan(p->ratio) || p->ratio < 1.0f)
        return dc_fail("ratio", "must be >= 1.0 (negative ratio not allowed)");
    if (!(p->ceiling > 0.0f) || p->ceiling > 1.0f)
        return dc_fail("ceiling", "must be in (0, 1.0]");
    if (p->ceiling < p->threshold)
        return dc_fail("ceiling", "must be >= threshold");
    if (isnan(p->attack_ms) || p->attack_ms < 0.0f)
        return dc_fail("attack_ms", "must be >= 0");
    if (isnan(p->release_ms) || p->release_ms < 0.0f)
        return dc_fail("release_ms", "must be >= 0");
    if (p->sample_rate <= 0)
        return dc_fail("sample_rate", "must be > 0");
    return 0;
}

/* 静态目标增益（瞬时，未包络）。三段：低=1 / 压缩区=1/ratio / 限幅=ceiling/|x| */
static float dc_target_gain(const DcParams *p, float x) {
    float a = fabsf(x);
    if (a <= p->threshold) return 1.0f;
    if (a < p->ceiling) {
        float y = p->threshold + (a - p->threshold) / p->ratio;
        return y / a;
    }
    return p->ceiling / a;
}

static int dc_tier_of(const DcParams *p, float x) {
    float a = fabsf(x);
    if (a <= p->threshold) return DC_TIER_LOW;
    if (a < p->ceiling) return DC_TIER_COMPRESS;
    return DC_TIER_LIMIT;
}

static double dc_coef(double time_ms, double sample_rate) {
    if (time_ms <= 0.0) return 1.0; /* 瞬时 */
    return 1.0 - exp(-1000.0 / (time_ms * sample_rate));
}

/* 实测恢复：最后一次超门限样本之后，增益首次回到 -0.1dB（0.988553）以内的时间 */
static double dc_release_ms(const float *gain, int n, int over_idx, int *truncated) {
    const double rel_thresh = 0.988553094652676; /* 10^(-0.1/20) */
    *truncated = 0;
    if (over_idx < 0) return -1.0;
    for (int i = over_idx + 1; i < n; ++i) {
        if ((double)gain[i] >= rel_thresh)
            return (double)(i - over_idx); /* 先返回样本数，调用方乘采样周期 */
    }
    *truncated = 1;
    return (double)(n - 1 - over_idx);
}

int dc_process(const DcParams *p, const float *in, float *out, int n,
               float *gain_out, unsigned char *tier_out, DcStats *st) {
    if (dc_validate(p) != 0) return -1;
    if (!in || !out || n <= 0) return dc_fail("buffer", "null input/output or n<=0");

    const double sr = (double)p->sample_rate;
    const double atk = dc_coef((double)p->attack_ms, sr);
    const double rel = dc_coef((double)p->release_ms, sr);

    /* 实测恢复时间需要逐样本增益；调用方未给缓冲时内部分配 */
    float *gain_internal = NULL;
    float *gcur = gain_out;
    if (st && !gcur) {
        gain_internal = (float*)malloc((size_t)n * sizeof(float));
        if (!gain_internal) return dc_fail("memory", "allocation failed");
        gcur = gain_internal;
    }

    if (st) {
        DcStats z;
        for (int k = 0; k < 3; ++k) z.tier_hist[k] = 0;
        z.in_peak = z.out_peak = z.peak_over = 0.0f;
        z.max_gain_reduction_db = 0.0f;
        z.mean_gain_reduction_db = 0.0;
        z.over_count = z.limit_count = 0;
        z.over_ms = z.measured_release_ms = 0.0;
        z.release_truncated = 0;
        *st = z;
    }

    double env = 1.0;       /* 平滑后的增益（线性），向 1.0 恢复 */
    double gr_sum_db = 0.0;
    int last_over = -1;

    for (int i = 0; i < n; ++i) {
        float x = in[i];
        int tier = dc_tier_of(p, x);
        float gt = p->bypass ? 1.0f : dc_target_gain(p, x);

        if (!p->bypass) {
            double coef = (gt < env) ? atk : rel; /* 增益减小走启动，增大走恢复 */
            env += coef * ((double)gt - env);
        } else {
            env = 1.0;
        }
        float g = (float)env;
        float y = x * g;

        /* 第三段：限幅兜底。包络启动期来不及下压时，硬钳到天花板 */
        if (!p->bypass && fabsf(y) > p->ceiling) {
            g = p->ceiling / fabsf(x);
            y = (x < 0.0f ? -p->ceiling : p->ceiling);
        }

        out[i] = y;
        if (gcur) gcur[i] = g;
        if (tier_out) tier_out[i] = (unsigned char)tier;

        float ax = fabsf(x), ay = fabsf(y);
        double gr_db = -20.0 * log10((double)g);
        if (gr_db < 0.0) gr_db = 0.0;
        gr_sum_db += gr_db;

        if (st) {
            if (ax > st->in_peak) st->in_peak = ax;
            if (ay > st->out_peak) st->out_peak = ay;
            if (gr_db > (double)st->max_gain_reduction_db)
                st->max_gain_reduction_db = (float)gr_db;
            st->tier_hist[tier]++;
            if (ax > p->threshold) { st->over_count++; last_over = i; }
            if (tier == DC_TIER_LIMIT) st->limit_count++;
        }
    }

    if (st) {
        st->mean_gain_reduction_db = gr_sum_db / (double)n;
        st->over_ms = (double)st->over_count * 1000.0 / sr;
        if (st->out_peak > p->ceiling)
            st->peak_over = st->out_peak - p->ceiling;
        else
            st->peak_over = 0.0f;
        int trunc = 0;
        double samples = dc_release_ms(gcur, n, last_over, &trunc);
        st->release_truncated = trunc;
        st->measured_release_ms = (samples < 0.0) ? -1.0 : samples * 1000.0 / sr;
    }
    free(gain_internal);
    return 0;
}

int dc_export_curve(const DcParams *p, const float *in, int n,
                    const float *gain, const unsigned char *tier, FILE *fp) {
    if (dc_validate(p) != 0) return -1;
    if (!in || !gain || !fp || n <= 0) return dc_fail("export", "null argument or n<=0");
    const double sr = (double)p->sample_rate;
    fputs("sample,time_ms,in,level,gain_db,out,tier\n", fp);
    for (int i = 0; i < n; ++i) {
        float g = gain[i];
        double gdb = 20.0 * log10((double)g);
        const char *name = "low";
        if (tier) {
            if (tier[i] == DC_TIER_COMPRESS) name = "compress";
            else if (tier[i] == DC_TIER_LIMIT) name = "limit";
        }
        fprintf(fp, "%d,%.4f,%.9g,%.9g,%.6f,%.9g,%s\n",
                i, (double)i * 1000.0 / sr, (double)in[i], (double)fabsf(in[i]),
                gdb, (double)(in[i] * g), name);
    }
    return 0;
}
