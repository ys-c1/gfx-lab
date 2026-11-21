#ifndef SHAKE_H
#define SHAKE_H

#include "cull.h"

/* 两段式抖动剔除配置 */
typedef struct {
    int enabled;     /* 0: 关闭新分支，退化为精确剔除，行为与输出照旧 */
    double base_tol; /* 基础容差（世界单位） */
    double amp_gain; /* 每单位抖动幅度追加的容差 */
    double max_tol;  /* 容差上限；超限报错并给出该上限值 */
    int tiers;       /* 容差档位数（>=1），容差按档取整以稳住负载 */
} ShakeCfg;

/* 单帧结果 */
typedef struct {
    int tier;           /* 容差档位 1..tiers；关闭新分支时为 0 */
    double tolerance;   /* 实际应用的容差 */
    int candidates;     /* 第二段按容差重算出的候选数 */
    int ref_count;      /* 参考工具（精确剔除）数量，用于对照 */
    int error;          /* candidates - ref_count */
    double elapsed_ms;  /* 本帧两段合计耗时 */
    int limit_exceeded; /* 所需容差超过上限 */
    double limit_value; /* 超限时给出的上限值 */
} ShakeFrame;

/* 汇总指标 */
typedef struct {
    int frames;
    double load_mean;
    double load_variance; /* 负载方差 */
    int load_peak;
    int load_valley;
    int peak_valley_diff; /* 负载峰谷差 */
    long total_error;
    double total_elapsed_ms;
    int limit_violations;
} ShakeStats;

/* 第一段：按抖动幅度外扩容差并按档取整。
   返回 0 成功；-1 参数非法；-3 超限（上限值即 cfg->max_tol）。 */
int shake_tolerance(const ShakeCfg *cfg, double amplitude,
                    double *tol_out, int *tier_out);

/* 两段合一：先按幅度定容差，再按容差外扩视图重算候选，
   并与参考工具（精确剔除）对照。返回 0 成功；-3 超限。 */
int shake_cull_frame(const ShakeCfg *cfg, const CullBox *boxes, int n,
                     CullView view, double amplitude,
                     ShakeFrame *frame, CullResult *out);

/* 汇总一段轨迹的负载指标 */
void shake_stats(const ShakeFrame *frames, int n, ShakeStats *stats);

#endif
