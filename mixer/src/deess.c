#include "deess.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DS_MAX_CH 64

typedef struct {
    double x1, x2, y1, y2;      /* biquad 状态 */
    float band_e2, full_e2;     /* 检出 RMS 包络(功率域) */
} DsCh;

struct DsCtx {
    DsCfg c;
    int channels;
    double b0, b1, b2, a1, a2;  /* 齿音带 BPF (RBJ, 中心 0 dB) */
    float det;                  /* RMS 包络单极点系数 */
    DsCh ch[DS_MAX_CH];
    /* 上一次 ds_process 的逐样本轨迹(供导出) */
    int frames;
    float *band_sig;            /* 齿音带信号 */
    float *band_env_t;
    float *full_env_t;
    float *ratio_t;
    float *gain_t;
};

DsCfg ds_default_cfg(int sample_rate) {
    DsCfg c;
    memset(&c, 0, sizeof(c));
    c.enabled = 1;
    c.sample_rate = sample_rate;
    c.band_lo_hz = 5000.0f;
    c.band_hi_hz = 8000.0f;
    c.thresh_ratio = 0.5f;
    c.min_level = 0.01f;
    c.max_att_db = 12.0f;
    c.slope = 1.0f;
    c.detect_ms = 20.0f;
    c.attack_ms = 5.0f;
    c.release_ms = 40.0f;
    c.hang_ms = 30.0f;
    c.merge_gap_ms = 50.0f;
    c.min_seg_ms = 10.0f;
    return c;
}

static void put_err(char errs[][96], int max_errs, int idx, const char *msg) {
    if (idx < max_errs) snprintf(errs[idx], 96, "%s", msg);
}

int ds_validate(const DsCfg *c, char errs[][96], int max_errs) {
    int n = 0;
    if (!c) { put_err(errs, max_errs, 0, "cfg is NULL"); return 1; }
    if (c->sample_rate <= 0)
        put_err(errs, max_errs, n++, "sample_rate must be > 0");
    if (c->band_lo_hz <= 0.0f)
        put_err(errs, max_errs, n++, "band_lo_hz must be > 0");
    if (c->band_hi_hz <= c->band_lo_hz)
        put_err(errs, max_errs, n++, "band_hi_hz must be > band_lo_hz (band empty)");
    if (c->sample_rate > 0 && c->band_hi_hz >= 0.5f * (float)c->sample_rate)
        put_err(errs, max_errs, n++, "band_hi_hz must be < sample_rate/2 (Nyquist)");
    if (c->thresh_ratio <= 0.0f || c->thresh_ratio >= 1.0f)
        put_err(errs, max_errs, n++, "thresh_ratio must be in (0,1)");
    if (c->min_level <= 0.0f)
        put_err(errs, max_errs, n++, "min_level must be > 0");
    if (c->max_att_db <= 0.0f)
        put_err(errs, max_errs, n++, "max_att_db must be > 0");
    if (c->slope <= 0.0f)
        put_err(errs, max_errs, n++, "slope must be > 0");
    if (c->detect_ms <= 0.0f)
        put_err(errs, max_errs, n++, "detect_ms must be > 0");
    if (c->attack_ms <= 0.0f)
        put_err(errs, max_errs, n++, "attack_ms must be > 0");
    if (c->release_ms <= 0.0f)
        put_err(errs, max_errs, n++, "release_ms must be > 0");
    if (c->hang_ms < 0.0f)
        put_err(errs, max_errs, n++, "hang_ms must be >= 0");
    if (c->merge_gap_ms < 0.0f)
        put_err(errs, max_errs, n++, "merge_gap_ms must be >= 0");
    if (c->min_seg_ms < 0.0f)
        put_err(errs, max_errs, n++, "min_seg_ms must be >= 0");
    return n;
}

static float ms_to_coef(float ms, int sr) {
    /* 一阶泄漏积分器步进: env += a*(x-env), 时间常数 ms -> a = 1-exp(-1/τ) */
    return 1.0f - expf(-1.0f / (ms * 0.001f * (float)sr));
}

DsCtx *ds_create(const DsCfg *c, int channels) {
    if (!c || channels <= 0 || channels > DS_MAX_CH) return NULL;
    if (c->enabled) {
        char errs[16][96];
        if (ds_validate(c, errs, 16) > 0) return NULL;
    }
    DsCtx *d = (DsCtx*)calloc(1, sizeof(DsCtx));
    if (!d) return NULL;
    d->c = *c;
    d->channels = channels;
    if (c->enabled) {
        double sr = (double)c->sample_rate;
        double f0 = sqrt((double)c->band_lo_hz * (double)c->band_hi_hz);
        double q = f0 / ((double)c->band_hi_hz - (double)c->band_lo_hz);
        double w0 = 2.0 * 3.14159265358979323846 * f0 / sr;
        double alpha = sin(w0) / (2.0 * q);
        double a0 = 1.0 + alpha;
        d->b0 = alpha / a0;
        d->b1 = 0.0;
        d->b2 = -alpha / a0;
        d->a1 = -2.0 * cos(w0) / a0;
        d->a2 = (1.0 - alpha) / a0;
        d->det = ms_to_coef(c->detect_ms, c->sample_rate);
    }
    return d;
}

void ds_destroy(DsCtx *d) {
    if (!d) return;
    free(d->band_sig); free(d->band_env_t); free(d->full_env_t);
    free(d->ratio_t); free(d->gain_t);
    free(d);
}

static void free_trace(DsCtx *d) {
    free(d->band_sig); free(d->band_env_t); free(d->full_env_t);
    free(d->ratio_t); free(d->gain_t);
    d->band_sig = d->band_env_t = d->full_env_t = d->ratio_t = d->gain_t = NULL;
    d->frames = 0;
}

static int alloc_trace(DsCtx *d, int frames) {
    free_trace(d);
    size_t n = (size_t)frames * (size_t)d->channels;
    d->band_sig   = (float*)malloc(n * sizeof(float));
    d->band_env_t = (float*)malloc(n * sizeof(float));
    d->full_env_t = (float*)malloc(n * sizeof(float));
    d->ratio_t    = (float*)malloc(n * sizeof(float));
    d->gain_t     = (float*)malloc(n * sizeof(float));
    if (!d->band_sig || !d->band_env_t || !d->full_env_t || !d->ratio_t || !d->gain_t) {
        free_trace(d);
        return -1;
    }
    d->frames = frames;
    return 0;
}

typedef struct { int start, end; } Run;

/* 段1: 逐样本检出 -> 段列表(合并/保持/最短时长) */
static int detect_segments(const DsCtx *d, const unsigned char *mask, int ch_idx,
                           int frames, Run **runs_out) {
    int cap = 16, n = 0;
    Run *runs = (Run*)malloc((size_t)cap * sizeof(Run));
    if (!runs) return -1;
    const unsigned char *m = mask + ch_idx;
    int stride = d->channels;
    int i = 0;
    while (i < frames) {
        if (m[(size_t)i * stride]) {
            int j = i + 1;
            while (j < frames && m[(size_t)j * stride]) ++j;
            if (n == cap) {
                cap *= 2;
                Run *nr = (Run*)realloc(runs, (size_t)cap * sizeof(Run));
                if (!nr) { free(runs); return -1; }
                runs = nr;
            }
            runs[n].start = i; runs[n].end = j; ++n;
            i = j;
        } else ++i;
    }
    /* 合并间隔 <= merge_gap 的相邻段 */
    int gap = (int)(d->c.merge_gap_ms * 0.001f * (float)d->c.sample_rate);
    int w = 0;
    for (int r = 0; r < n; ++r) {
        if (w > 0 && runs[r].start - runs[w-1].end <= gap) {
            if (runs[r].end > runs[w-1].end) runs[w-1].end = runs[r].end;
        } else runs[w++] = runs[r];
    }
    n = w;
    /* 段尾保持 hang */
    int hang = (int)(d->c.hang_ms * 0.001f * (float)d->c.sample_rate);
    for (int r = 0; r < n; ++r) {
        runs[r].end += hang;
        if (runs[r].end > frames) runs[r].end = frames;
    }
    /* hang 后可能重叠, 再合并一次 */
    w = 0;
    for (int r = 0; r < n; ++r) {
        if (w > 0 && runs[r].start <= runs[w-1].end) {
            if (runs[r].end > runs[w-1].end) runs[w-1].end = runs[r].end;
        } else runs[w++] = runs[r];
    }
    n = w;
    /* 丢弃短于 min_seg 的段 */
    int min_len = (int)(d->c.min_seg_ms * 0.001f * (float)d->c.sample_rate);
    w = 0;
    for (int r = 0; r < n; ++r)
        if (runs[r].end - runs[r].start >= min_len) runs[w++] = runs[r];
    *runs_out = runs;
    return w;
}

int ds_process(DsCtx *d, const float *in, float *out, int frames,
               DsSeg *segs, int max_segs, DsStats *stats) {
    if (!d || !in || !out || frames <= 0) return -1;
    DsStats st;
    memset(&st, 0, sizeof(st));
    int chs = d->channels;
    size_t total = (size_t)frames * (size_t)chs;

    if (!d->c.enabled) {                    /* 旁通: 逐位一致 */
        memcpy(out, in, total * sizeof(float));
        if (segs && max_segs > 0) memset(segs, 0, (size_t)max_segs * sizeof(DsSeg));
        if (stats) *stats = st;
        return 0;
    }
    if (alloc_trace(d, frames) != 0) return -1;
    unsigned char *mask = (unsigned char*)calloc(total, 1);
    if (!mask) return -1;

    /* ---- 段1: 按频段检出 ---- */
    for (int i = 0; i < frames; ++i) {
        for (int c = 0; c < chs; ++c) {
            DsCh *s = &d->ch[c];
            size_t idx = (size_t)i * chs + c;
            double x = in[idx];
            double y = d->b0 * x + d->b1 * s->x1 + d->b2 * s->x2
                     - d->a1 * s->y1 - d->a2 * s->y2;
            s->x2 = s->x1; s->x1 = x;
            s->y2 = s->y1; s->y1 = y;
            float band = (float)y;
            float af = fabsf((float)x);
            /* 对称单极点 RMS: 两包络共用同一线性滤波器,
             * 稳态严格等于均方值, 比值有干净解析解 */
            s->band_e2 += d->det * (band * band - s->band_e2);
            s->full_e2 += d->det * ((float)x * (float)x - s->full_e2);
            float band_rms = sqrtf(s->band_e2 > 0.0f ? s->band_e2 : 0.0f);
            float full_rms = sqrtf(s->full_e2 > 1e-24f ? s->full_e2 : 1e-24f);
            float ratio = band_rms / full_rms;
            d->band_sig[idx] = band;
            d->band_env_t[idx] = band_rms;
            d->full_env_t[idx] = full_rms;
            d->ratio_t[idx] = ratio;
            d->gain_t[idx] = 1.0f;
            if (ratio >= d->c.thresh_ratio && band_rms >= d->c.min_level)
                mask[idx] = 1;
            if (af > st.peak_in) st.peak_in = af;
        }
    }

    /* ---- 段2: 按强度算衰减量 + 段3: 逐段套用 ---- */
    int seg_total = 0, seg_written = 0;
    for (int c = 0; c < chs; ++c) {
        Run *runs = NULL;
        int nruns = detect_segments(d, mask, c, frames, &runs);
        if (nruns < 0) { free(mask); return -1; }
        for (int r = 0; r < nruns; ++r) {
            int s0 = runs[r].start, s1 = runs[r].end;
            float peak_ratio = 0.0f;
            for (int i = s0; i < s1; ++i) {
                float v = d->ratio_t[(size_t)i * chs + c];
                if (v > peak_ratio) peak_ratio = v;
            }
            float att_db = 20.0f * log10f(peak_ratio / d->c.thresh_ratio) * d->c.slope;
            if (att_db > d->c.max_att_db) att_db = d->c.max_att_db;
            if (att_db < 0.0f) att_db = 0.0f;
            float g = powf(10.0f, -att_db / 20.0f);
            if (att_db > st.max_att_db) st.max_att_db = att_db;
            ++seg_total;
            if (segs && seg_written < max_segs) {
                DsSeg *sg = &segs[seg_written++];
                sg->channel = c; sg->start_frame = s0; sg->end_frame = s1;
                sg->att_db = att_db; sg->peak_ratio = peak_ratio;
            }
            /* 梯形增益: attack 下压 -> 保持 g -> release 恢复 */
            int len = s1 - s0;
            int att_n = (int)(d->c.attack_ms * 0.001f * (float)d->c.sample_rate);
            int rel_n = (int)(d->c.release_ms * 0.001f * (float)d->c.sample_rate);
            if (att_n < 1) att_n = 1;
            if (rel_n < 1) rel_n = 1;
            if (att_n + rel_n > len) {      /* 段太短, 等比压缩两斜坡 */
                att_n = (int)((long long)len * att_n / (att_n + rel_n));
                rel_n = len - att_n;
                if (rel_n < 1) { rel_n = 1; att_n = len - 1; }
                if (att_n < 1) att_n = 1;
            }
            for (int k = 0; k < len; ++k) {
                float gk;
                if (k < att_n) gk = 1.0f + (g - 1.0f) * (float)(k + 1) / (float)att_n;
                else if (k >= len - rel_n) gk = g + (1.0f - g) * (float)(k - (len - rel_n) + 1) / (float)rel_n;
                else gk = g;
                d->gain_t[(size_t)(s0 + k) * chs + c] = gk;
            }
        }
        free(runs);
    }
    free(mask);

    for (size_t idx = 0; idx < total; ++idx) {
        float g = d->gain_t[idx];
        float o = in[idx] - d->band_sig[idx] * (1.0f - g);
        out[idx] = o;
        float ao = fabsf(o);
        if (ao > st.peak_out) st.peak_out = ao;
    }
    st.segments = seg_written;
    st.segments_total = seg_total;
    if (stats) *stats = st;
    return 0;
}

int ds_export_csv(const DsCtx *d, const char *path) {
    if (!d || !path || !d->ratio_t || !d->gain_t) return -1;
    FILE *f = fopen(path, "w");
    if (!f) return -2;
    fprintf(f, "frame,ch,band_env,full_env,ratio,gain\n");
    int chs = d->channels;
    for (int i = 0; i < d->frames; ++i)
        for (int c = 0; c < chs; ++c) {
            size_t idx = (size_t)i * chs + c;
            fprintf(f, "%d,%d,%.8g,%.8g,%.8g,%.8g\n", i, c,
                    (double)d->band_env_t[idx], (double)d->full_env_t[idx],
                    (double)d->ratio_t[idx], (double)d->gain_t[idx]);
        }
    fclose(f);
    return 0;
}
