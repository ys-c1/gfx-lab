#include "shake.h"
#include <math.h>
#include <time.h>

static double now_ms(void) {
    return (double)clock() * 1000.0 / (double)CLOCKS_PER_SEC;
}

static CullView expand_view(CullView v, double t) {
    CullView e = { v.x - t, v.y - t, v.w + 2.0 * t, v.h + 2.0 * t };
    return e;
}

int shake_tolerance(const ShakeCfg *cfg, double amplitude,
                    double *tol_out, int *tier_out) {
    if (!cfg || !tol_out || !tier_out) return -1;
    if (cfg->tiers < 1 || cfg->max_tol <= 0.0) return -1;
    if (amplitude < 0.0) amplitude = 0.0;
    /* 第一段：按抖动幅度外扩容差 */
    double raw = cfg->base_tol + cfg->amp_gain * amplitude;
    if (raw > cfg->max_tol) return -3; /* 超限：上限值即 cfg->max_tol */
    /* 容差按档取整，避免逐帧微抖引起负载抖动 */
    double width = cfg->max_tol / (double)cfg->tiers;
    int tier = (int)ceil(raw / width);
    if (tier < 1) tier = 1;
    if (tier > cfg->tiers) tier = cfg->tiers;
    *tier_out = tier;
    *tol_out = tier * width;
    return 0;
}

int shake_cull_frame(const ShakeCfg *cfg, const CullBox *boxes, int n,
                     CullView view, double amplitude,
                     ShakeFrame *frame, CullResult *out) {
    if (!cfg || !frame || !out) return -1;
    double t0 = now_ms();
    frame->tier = 0;
    frame->tolerance = 0.0;
    frame->candidates = 0;
    frame->error = 0;
    frame->limit_exceeded = 0;
    frame->limit_value = cfg->max_tol;

    /* 参考工具对照：精确剔除计数 */
    frame->ref_count = cull_count_within(boxes, n, view);
    if (frame->ref_count < 0) return frame->ref_count;

    if (!cfg->enabled) {
        /* 关闭新分支：精确剔除，行为与输出照旧 */
        int rc = cull_run(boxes, n, view, out);
        if (rc != 0) return rc;
        frame->candidates = out->count;
        frame->elapsed_ms = now_ms() - t0;
        return 0;
    }

    double tol = 0.0;
    int tier = 0;
    int rc = shake_tolerance(cfg, amplitude, &tol, &tier);
    if (rc == -3) {
        frame->limit_exceeded = 1;
        frame->limit_value = cfg->max_tol;
        frame->elapsed_ms = now_ms() - t0;
        return -3;
    }
    if (rc != 0) return rc;

    /* 第二段：按容差外扩视图，重算候选 */
    rc = cull_run(boxes, n, expand_view(view, tol), out);
    if (rc != 0) return rc;
    frame->tier = tier;
    frame->tolerance = tol;
    frame->candidates = out->count;
    frame->error = out->count - frame->ref_count;
    frame->elapsed_ms = now_ms() - t0;
    return 0;
}

void shake_stats(const ShakeFrame *frames, int n, ShakeStats *stats) {
    if (!stats) return;
    stats->frames = n;
    stats->load_mean = 0.0;
    stats->load_variance = 0.0;
    stats->load_peak = 0;
    stats->load_valley = 0;
    stats->peak_valley_diff = 0;
    stats->total_error = 0;
    stats->total_elapsed_ms = 0.0;
    stats->limit_violations = 0;
    if (!frames || n <= 0) return;

    double sum = 0.0;
    int peak = frames[0].candidates, valley = frames[0].candidates;
    for (int i = 0; i < n; ++i) {
        sum += frames[i].candidates;
        if (frames[i].candidates > peak) peak = frames[i].candidates;
        if (frames[i].candidates < valley) valley = frames[i].candidates;
        stats->total_error += frames[i].error;
        stats->total_elapsed_ms += frames[i].elapsed_ms;
        if (frames[i].limit_exceeded) ++stats->limit_violations;
    }
    double mean = sum / (double)n;
    double var = 0.0;
    for (int i = 0; i < n; ++i) {
        double d = frames[i].candidates - mean;
        var += d * d;
    }
    stats->load_mean = mean;
    stats->load_variance = var / (double)n;
    stats->load_peak = peak;
    stats->load_valley = valley;
    stats->peak_valley_diff = peak - valley;
}
