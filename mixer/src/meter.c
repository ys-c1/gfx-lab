#include "meter.h"
#include <math.h>
#include <stdio.h>

const char *meter_strerror(int err) {
    switch (err) {
    case METER_OK:       return "OK";
    case METER_E_NULL:   return "空指针：meter 或输入缓冲区为 NULL";
    case METER_E_EMPTY:  return "空输入：frames<=0，没有任何采样可测量";
    case METER_E_PARAM:  return "参数非法：采样率或动态范围必须为正";
    default:             return "未知错误";
    }
}

int meter_fall_alpha(int fs, float fall_db_per_s, float *out_alpha) {
    if (!out_alpha) return METER_E_NULL;
    if (fs <= 0) return METER_E_PARAM;
    if (fall_db_per_s <= 0.0f) {
        *out_alpha = 0.0f;   /* 退化：无阻尼，立即回落，等价旧行为 */
        return METER_OK;
    }
    /* 每秒线性衰减 g = 10^(-v/20)，每采样 alpha = g^(1/fs) */
    *out_alpha = powf(10.0f, -fall_db_per_s / (20.0f * (float)fs));
    return METER_OK;
}

int meter_init(LevelMeter *m, int fs, float fall_db_per_s, float dynamic_range_db) {
    if (!m) return METER_E_NULL;
    if (fs <= 0 || dynamic_range_db <= 0.0f) return METER_E_PARAM;
    int rc = meter_fall_alpha(fs, fall_db_per_s, &m->alpha);
    if (rc != METER_OK) return rc;
    m->fs = fs;
    m->fall_db_per_s = fall_db_per_s;
    m->dynamic_range_db = dynamic_range_db;
    m->floor_lin = powf(10.0f, -dynamic_range_db / 20.0f);
    m->peak = 0.0f;
    return METER_OK;
}

float meter_update(LevelMeter *m, float x) {
    float target = fabsf(x);
    if (target >= m->peak) {
        m->peak = target;                 /* 攻击：即时冲上去 */
    } else {
        float damped = m->peak * m->alpha;
        m->peak = target > damped ? target : damped;  /* 回落：阻尼，但不低于真实信号 */
    }
    return m->peak;
}

int meter_process(LevelMeter *m, const float *src, int frames, float *out_peak) {
    if (!m) return METER_E_NULL;
    if (!src) return METER_E_NULL;
    if (frames <= 0) return METER_E_EMPTY;
    float block_peak = m->peak;
    for (int i = 0; i < frames; ++i) {
        float p = meter_update(m, src[i]);
        if (p > block_peak) block_peak = p;
    }
    if (out_peak) *out_peak = block_peak;
    return METER_OK;
}

float meter_peak_db(const LevelMeter *m) {
    if (!m || m->peak <= 0.0f) return -INFINITY;
    return 20.0f * log10f(m->peak);
}

int meter_export_curve(const LevelMeter *m, const float *src, int frames,
                       int step, const char *path) {
    if (!m || !src || !path) return METER_E_NULL;
    if (frames <= 0) return METER_E_EMPTY;
    if (step <= 0) return METER_E_PARAM;
    FILE *f = fopen(path, "w");
    if (!f) return METER_E_NULL;
    /* 在本地副本上推进，导出不改变原表的当前读数（const 语义） */
    LevelMeter local = *m;
    fprintf(f, "sample,t_s,target_lin,peak_lin,peak_db\n");
    int rows = 0;
    for (int i = 0; i < frames; ++i) {
        float p = meter_update(&local, src[i]);
        if (i % step == 0 || i == frames - 1) {
            float db = p > 0.0f ? 20.0f * log10f(p) : -INFINITY;
            if (isfinite(db))
                fprintf(f, "%d,%.9f,%.9g,%.9g,%.6f\n", i,
                        (double)i / (double)m->fs, fabsf(src[i]), p, db);
            else
                fprintf(f, "%d,%.9f,%.9g,%.9g,-inf\n", i,
                        (double)i / (double)m->fs, fabsf(src[i]), p);
            ++rows;
        }
    }
    fclose(f);
    return rows;
}
