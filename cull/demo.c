#include "cull.h"
#include "shake.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned rng_state = 1u;
static void rng_seed(unsigned s) { rng_state = s ? s : 1u; }
static double frand(void) { /* LCG -> [0,1)，确定性轨迹可复现 */
    rng_state = rng_state * 1664525u + 1013904223u;
    return (double)(rng_state >> 8) / (double)(1u << 24);
}

#define FRAMES 120
#define VIEW_W 100.0
#define VIEW_H 100.0
#define SCENE 300.0

static void gen_scene(CullBox *b, int n, unsigned seed) {
    rng_seed(seed);
    for (int i = 0; i < n; ++i) {
        b[i].x = frand() * SCENE;
        b[i].y = frand() * SCENE;
        b[i].w = 6.0 + frand() * 12.0;
        b[i].h = 6.0 + frand() * 12.0;
        b[i].layer = i % 3;
    }
}

/* 模拟一次抖动轨迹：基础路径 + 随机抖动，逐帧两段式剔除并记录指标 */
static int run_scale(int n, const ShakeCfg *cfg, const char *csv_path) {
    CullBox *boxes = (CullBox *)malloc(sizeof(CullBox) * (size_t)n);
    ShakeFrame *fr = (ShakeFrame *)malloc(sizeof(ShakeFrame) * FRAMES);
    int *tier_hist = (int *)calloc((size_t)cfg->tiers + 1, sizeof(int));
    if (!boxes || !fr || !tier_hist) { free(boxes); free(fr); free(tier_hist); return 1; }
    gen_scene(boxes, n, (unsigned)(n * 7 + 13));

    FILE *csv = fopen(csv_path, "w");
    if (!csv) { fprintf(stderr, "cannot open %s\n", csv_path); free(boxes); free(fr); free(tier_hist); return 1; }
    fprintf(csv, "frame,cam_x,cam_y,amplitude,tier,tolerance,candidates,ref_count,error,elapsed_ms\n");

    rng_seed((unsigned)n);
    double prev_x = 0.0, prev_y = 0.0;
    int max_tier = 0, max_err = 0, violations = 0;
    long total_cand = 0;
    for (int f = 0; f < FRAMES; ++f) {
        double t = (double)f / (double)FRAMES;
        /* 基础路径绕场景缓慢移动 */
        double bx = 40.0 + 180.0 * t;
        double by = 100.0 + 60.0 * sin(2.0 * 3.14159265358979 * t);
        /* 镜头抖动：幅度随时间起伏 */
        double shake = 4.0 + 6.0 * sin(6.0 * 3.14159265358979 * t);
        double jx = (frand() - 0.5) * 2.0 * shake;
        double jy = (frand() - 0.5) * 2.0 * shake;
        double cx = bx + jx, cy = by + jy;
        double amp = (f == 0) ? 0.0 : hypot(cx - prev_x, cy - prev_y);
        prev_x = cx; prev_y = cy;

        CullView v = { cx, cy, VIEW_W, VIEW_H };
        CullResult r;
        int rc = shake_cull_frame(cfg, boxes, n, v, amp, &fr[f], &r);
        if (rc == -3) {
            /* 超限：报错并给出上限值 */
            fprintf(stderr, "ERROR frame %d: required tolerance exceeds limit %.2f\n",
                    f, fr[f].limit_value);
            ++violations;
            fr[f].candidates = fr[f].ref_count;
        } else if (rc != 0) {
            fprintf(stderr, "ERROR frame %d: rc=%d\n", f, rc);
            free(r.ids); free(boxes); free(fr); free(tier_hist); fclose(csv);
            return 1;
        }
        free(r.ids);
        if (fr[f].tier > max_tier) max_tier = fr[f].tier;
        if (fr[f].tier >= 0 && fr[f].tier <= cfg->tiers) ++tier_hist[fr[f].tier];
        if (fr[f].error > max_err) max_err = fr[f].error;
        total_cand += fr[f].candidates;
        fprintf(csv, "%d,%.3f,%.3f,%.3f,%d,%.3f,%d,%d,%d,%.4f\n",
                f, cx, cy, amp, fr[f].tier, fr[f].tolerance,
                fr[f].candidates, fr[f].ref_count, fr[f].error, fr[f].elapsed_ms);
    }
    fclose(csv);

    ShakeStats st;
    shake_stats(fr, FRAMES, &st);
    printf("== shake cull: scale n=%d, frames=%d ==\n", n, FRAMES);
    printf("  load: mean=%.2f variance=%.2f peak=%d valley=%d peak-valley=%d\n",
           st.load_mean, st.load_variance, st.load_peak, st.load_valley,
           st.peak_valley_diff);
    printf("  tiers: max used=%d of %d |", max_tier, cfg->tiers);
    for (int t = 1; t <= cfg->tiers; ++t) printf(" t%d=%d", t, tier_hist[t]);
    printf("\n");
    printf("  candidates: total=%ld\n", total_cand);
    printf("  error vs reference: total=%ld max=%d\n", st.total_error, max_err);
    printf("  time: total=%.3fms avg=%.4fms\n",
           st.total_elapsed_ms, st.total_elapsed_ms / FRAMES);
    if (violations)
        printf("  limit violations: %d (limit %.2f)\n", violations, cfg->max_tol);
    printf("  trajectory: %s\n", csv_path);

    free(boxes); free(fr); free(tier_hist);
    return 0;
}

int main(void) {
    enum { N = 64 };
    CullBox b[N];
    for (int i = 0; i < N; ++i) { b[i].x = (i % 8) * 20; b[i].y = (i / 8) * 20; b[i].w = 18; b[i].h = 18; b[i].layer = i % 3; }
    CullView v = {0, 0, 100, 100};
    CullResult r;
    if (cull_run(b, N, v, &r) != 0) return 1;
    printf("visible %d of %d\n", r.count, N);
    for (int i = 0; i < r.count; ++i) printf("  %d\n", r.ids[i]);
    free(r.ids);

    /* 两段式抖动剔除：两个规模分别记误差与耗时，并与参考工具对照 */
    ShakeCfg cfg = { 1, 2.0, 1.0, 24.0, 6 };
    if (run_scale(256, &cfg, "jitter_trajectory_256.csv") != 0) return 1;
    if (run_scale(1024, &cfg, "jitter_trajectory_1024.csv") != 0) return 1;
    return 0;
}
