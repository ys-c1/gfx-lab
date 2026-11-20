#include "sweep.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>

/* 确定性 LCG, 保证两档数据可复现 */
static unsigned long rng_state;
static double rng_double(void) {
    rng_state = rng_state * 6364136223846793005UL + 1442695040888963407UL;
    return (double)(rng_state >> 11) / (double)(1UL << 53);
}

/* 每帧深度抖动: 对象按 id 两两成对, 同对两成员反相、且符号随检查点翻转,
 * 使同深度对的相对遮挡序在相邻帧必然颠倒。
 * 扫描线模式只在 ENTER 采样一次并持有 -> 跳变恒 0, 但偏离实时基准(误差);
 * 非扫描线模式每检查点重采样并整表重排 -> 紧跟基准(误差 0), 但次序来回跳。 */
static double jitter(const SweepObj *o, int cp, void *user) {
    (void)user;
    int member = o->id & 1;                 /* 同对的两个成员: 0 与 1 */
    double sgn = ((cp + member) & 1) ? 1.0 : -1.0;
    return o->depth + sgn * 0.4;
}

/* 成对数据: id=2k 与 2k+1 同基准深度、区间完全重叠(同生共死),
 * 制造确定会发生遮挡翻转的对象对; 不同对沿扫描轴错开且宽度随机。 */
static SweepObj *gen(int n, double span, int cluster) {
    rng_state = 0xC0FFEEUL ^ (unsigned long)n;
    SweepObj *p = malloc(sizeof(SweepObj) * (size_t)n);
    if (!p) return NULL;
    int pairs = (n + 1) / 2;
    for (int k = 0; k < pairs; ++k) {
        double x0 = ((double)k / (double)pairs) * span;
        double w  = span * (0.02 + rng_double() * 0.03);  /* 覆盖若干相邻对 */
        int depth_cluster = k % cluster;
        for (int member = 0; member < 2; ++member) {
            int i = 2 * k + member;
            if (i >= n) break;
            p[i].id = i;
            p[i].x0 = x0;                  /* 同对共享区间 => 始终同时活动 */
            p[i].x1 = x0 + w;
            p[i].depth = (double)depth_cluster;
        }
    }
    return p;
}


static void pout(FILE *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
}

static void run_tier(FILE *rep, int n, int export_csv) {
    const double span = 1000.0;
    SweepObj *p = gen(n, span, 8);
    if (!p) { pout(rep, "alloc failed at n=%d\n", n); return; }

    char name[64];
    FILE *cs1 = NULL, *cs2 = NULL;
    if (export_csv) {                          /* 仅小档导出完整活动表, 避免巨型 CSV */
        snprintf(name, sizeof name, "active_%d_sweep.csv", n);
        cs1 = fopen(name, "w");
        snprintf(name, sizeof name, "active_%d_naive.csv", n);
        cs2 = fopen(name, "w");
    }

    SweepOpt so = {1, jitter, NULL, cs1, NULL, NULL};
    SweepOpt no = {0, jitter, NULL, cs2, NULL, NULL};
    SweepStats s1, s2;
    int rc1 = sweep_run(p, n, &so, &s1);
    int rc2 = sweep_run(p, n, &no, &s2);
    if (rc1 != SWEEP_OK || rc2 != SWEEP_OK) {
        pout(rep, "run failed n=%d: %d %d\n", n, rc1, rc2);
        free(p);
        return;
    }

    pout(rep, "== n = %d ==\n", n);
    pout(rep, "%-22s %16s %16s\n", "metric", "sweep(两层)", "naive(不走线)");
    pout(rep, "%-22s %16d %16d\n", "events 事件数", s1.events, s2.events);
    pout(rep, "%-22s %16d %16d\n", "checkpoints 检查点", s1.checkpoints, s2.checkpoints);
    pout(rep, "%-22s %16d %16d\n", "max_active 活动峰值", s1.max_active, s2.max_active);
    pout(rep, "%-22s %16lld %16lld\n", "compares 候选数", s1.compares, s2.compares);
    pout(rep, "%-22s %16lld %16lld\n", "pair_candidates 对", s1.pair_candidates, 0LL);
    pout(rep, "%-22s %16lld %16lld\n", "ref_compares 参考排序", s1.ref_compares, 0LL);
    pout(rep, "%-22s %16lld %16lld\n", "jumps 跳变量", s1.jumps, s2.jumps);
    pout(rep, "%-22s %16lld %16lld\n", "errors 误差(逆序对)", s1.errors, s2.errors);
    pout(rep, "%-22s %16lld %16lld\n", "set_changes 集合变化", s1.set_changes, s2.set_changes);
    pout(rep, "%-22s %13lld us %13lld us\n", "algo_time 算法净耗时",
          s1.algo_ns / 1000, s2.algo_ns / 1000);
    pout(rep, "%-22s %13lld us %13lld us\n", "total_time 含度量总耗时",
          s1.elapsed_ns / 1000, s2.elapsed_ns / 1000);
    /* 每检查点均值, 口径对齐后再比 */
    pout(rep, "%-22s %16.2f %16.2f\n", "jumps/cp 跳变均值",
          (double)s1.jumps / s1.checkpoints, (double)s2.jumps / s2.checkpoints);
    pout(rep, "%-22s %16.2f %16.2f\n", "errors/cp 误差均值",
          (double)s1.errors / s1.checkpoints, (double)s2.errors / s2.checkpoints);
    pout(rep, "%-22s %16.2f %16.2f\n", "compares/cp 候选均值",
          (double)s1.compares / s1.checkpoints, (double)s2.compares / s2.checkpoints);
    if (export_csv)
        pout(rep, "活动表已导出: active_%d_sweep.csv (%d 行), active_%d_naive.csv (%d 行)\n\n",
              n, s1.checkpoints, n, s2.checkpoints);
    else
        pout(rep, "(n=%d 档不导出完整活动表, 避免巨型 CSV; 数据见上方统计)\n\n", n);

    if (cs1) fclose(cs1);
    if (cs2) fclose(cs2);
    free(p);
}

/* 错误演示: 空扫描线 / 区间倒置带序号 */
static void demo_errors(FILE *rep) {
    SweepStats st;
    int bad = -1;
    SweepObj empty[1] = {{0, 0, 1, 1}};
    pout(rep, "validate n=0 -> %d (SWEEP_ERR_EMPTY=%d)\n",
         sweep_validate(empty, 0, &bad), SWEEP_ERR_EMPTY);
    SweepObj inv[3] = {{0, 0, 5, 1}, {1, 9, 2, 1}, {2, 0, 5, 1}};
    int rc = sweep_validate(inv, 3, &bad);
    pout(rep, "validate inverted -> %d (SWEEP_ERR_INVERTED=%d), bad_index=%d\n",
         rc, SWEEP_ERR_INVERTED, bad);
    pout(rep, "run inverted -> %d\n\n", sweep_run(inv, 3, NULL, &st));
}

int main(void) {
    FILE *rep = fopen("demo.txt", "w");
    if (!rep) return 1;
    pout(rep, "sweep: 扫描线活动收集 + 入出事件遮挡排序 基准报告\n\n");
    demo_errors(rep);
    run_tier(rep, 1000, 1);    /* 小档: 导出完整活动表 CSV */
    run_tier(rep, 10000, 0);   /* 大档: 只出统计, 不写巨型 CSV */
    fclose(rep);
    return 0;
}
