/*
 * cull 分层两段剔除：规模对照。
 *
 * 对每个规模分别记录：
 *   - 与参考工具的误差（可见集合对称差、各组候选数/可见数差）
 *   - 耗时（分组段、剔除段、合计）
 * 并报告：分组数、各组层号与候选数、分组边界、组间串扰。
 *
 * 参考工具在这里独立实现（不复用库里的判定函数）。
 */
#define _POSIX_C_SOURCE 199309L
#include "cull.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static unsigned long long rng_state = 0x9e3779b97f4a7c15ull;
static unsigned long long rnd(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static int cmp_int(const void *a, const void *b) {
    int x = *(const int*)a, y = *(const int*)b;
    return (x > y) - (x < y);
}

/* ---- 参考工具（独立实现）----
   逐对象暴力判定视口重叠；按层过滤后统计候选数/可见数。 */
static int ref_hit(const CullBox *b, CullView v) {
    double l = b->x, r = b->x + b->w;
    double t = b->y, bo = b->y + b->h;
    if (r < v.x) return 0;
    if (l > v.x + v.w) return 0;
    if (bo < v.y) return 0;
    if (t > v.y + v.h) return 0;
    return 1;
}

static int ref_candidates(const CullBox *b, int n, int layer) {
    int c = 0;
    for (int i = 0; i < n; ++i) if (b[i].layer == layer) ++c;
    return c;
}

static int ref_visible(const CullBox *b, int n, int layer, CullView v) {
    int c = 0;
    for (int i = 0; i < n; ++i)
        if (b[i].layer == layer && ref_hit(&b[i], v)) ++c;
    return c;
}

typedef struct {
    int groups;
    int candidates_total;
    int visible_total;
    long set_error;    /* 可见集合对称差规模 */
    long count_error;  /* 各组候选数与可见数相对参考的绝对差之和 */
    long crosstalk;
} BenchReport;

static int bench(int n) {
    CullBox *b = (CullBox*)malloc(sizeof(CullBox) * (size_t)n);
    if (!b) return 1;
    for (int i = 0; i < n; ++i) {
        b[i].x = (double)(rnd() % 4000) - 1000.0;
        b[i].y = (double)(rnd() % 4000) - 1000.0;
        b[i].w = 20.0 + (double)(rnd() % 80);
        b[i].h = 20.0 + (double)(rnd() % 80);
        b[i].layer = (int)(rnd() % 8); /* 8 个不同层号 */
    }
    CullView v = {0, 0, 1500, 1500};
    CullError st;
    CullLayered L;

    /* 段一、段二分别计时：多次运行取最优，减小调度噪声。 */
    enum { REPS = 5 };
    CullGrouping g;
    double tg = 1e30, tc = 1e30, t0, t1, t2;
    int rc = CULL_OK;
    for (int rep = 0; rep < REPS; ++rep) {
        t0 = now_s();
        rc = cull_group_by_layer(b, n, &g, &st);
        t1 = now_s();
        if (rc != CULL_OK) break;
        rc = cull_run_grouped(b, &g, v, &L, &st);
        t2 = now_s();
        cull_grouping_free(&g);
        if (rc != CULL_OK) break;
        if (t1 - t0 < tg) tg = t1 - t0;
        if (t2 - t1 < tc) tc = t2 - t1;
        if (rep < REPS - 1) cull_layered_free(&L);
    }
    if (rc != CULL_OK) { printf("规模 n=%d 失败 rc=%d\n", n, rc); free(b); return 1; }

    BenchReport rep;
    memset(&rep, 0, sizeof(rep));
    rep.groups = L.group_count;

    /* 候选总数与可见总数。 */
    for (int k = 0; k < L.group_count; ++k) rep.candidates_total += L.groups[k].candidates;
    rep.visible_total = L.count;

    /* 误差 1：可见集合与参考工具（逐层暴力）的对称差。 */
    {
        int *ref = (int*)malloc(sizeof(int) * (size_t)n);
        int *got = (int*)malloc(sizeof(int) * (size_t)L.count);
        int *sorted = (int*)malloc(sizeof(int) * (size_t)L.count);
        int rn = 0;
        for (int i = 0; i < n; ++i) if (ref_hit(&b[i], v)) ref[rn++] = i;
        for (int k = 0; k < L.count; ++k) sorted[k] = L.ids[k];
        qsort(sorted, (size_t)L.count, sizeof(int), cmp_int);
        for (int k = 0; k < L.count; ++k) got[k] = sorted[k];
        int i = 0, j = 0;
        long diff = 0;
        while (i < rn || j < L.count) {
            if (j >= L.count || (i < rn && ref[i] < got[j])) { ++diff; ++i; }
            else if (i >= rn || got[j] < ref[i]) { ++diff; ++j; }
            else { ++i; ++j; }
        }
        rep.set_error = diff;
        free(ref); free(got); free(sorted);
    }

    /* 误差 2：各组候选数、可见数与参考工具逐组对照。 */
    for (int k = 0; k < L.group_count; ++k) {
        rep.count_error += labs((long)L.groups[k].candidates - ref_candidates(b, n, L.groups[k].layer));
        rep.count_error += labs((long)L.groups[k].count - ref_visible(b, n, L.groups[k].layer, v));
    }

    /* 串扰：把每层单独喂给旧接口，分组输出必须逐项一致；
       且每个区间里的 id 必须属于本层。 */
    for (int k = 0; k < L.group_count; ++k) {
        int layer = L.groups[k].layer;
        for (int m = 0; m < L.groups[k].count; ++m)
            if (b[L.ids[L.groups[k].first + m]].layer != layer) ++rep.crosstalk;

        CullBox *sub = (CullBox*)malloc(sizeof(CullBox) * (size_t)n);
        int *map = (int*)malloc(sizeof(int) * (size_t)n);
        int sn = 0;
        for (int i = 0; i < n; ++i)
            if (b[i].layer == layer) { sub[sn] = b[i]; map[sn] = i; ++sn; }
        CullResult rr;
        if (cull_run(sub, sn, v, &rr) == 0) {
            if (rr.count != L.groups[k].count) ++rep.crosstalk;
            for (int m = 0; m < rr.count && m < L.groups[k].count; ++m)
                if (map[rr.ids[m]] != L.ids[L.groups[k].first + m]) ++rep.crosstalk;
            free(rr.ids);
        } else {
            ++rep.crosstalk;
        }
        free(sub); free(map);
    }

    printf("规模 n=%d\n", n);
    printf("  分组数 %d（上限 %d）\n", rep.groups, L.layer_limit);
    printf("  参考工具对照: 候选 %d, 可见 %d; 分层 可见 %d\n",
           rep.candidates_total, rep.visible_total, L.count);
    printf("  误差: 可见集合对称差 %ld, 各组候选/可见计数绝对差合计 %ld\n",
           rep.set_error, rep.count_error);
    printf("  组间串扰: %ld\n", rep.crosstalk);
    printf("  分组边界（半开区间 [first, first+count)）:\n");
    printf("    层号  候选数  可见数  ids 区间\n");
    for (int k = 0; k < L.group_count; ++k)
        printf("    %4d  %6d  %6d  [%d, %d)\n",
               L.groups[k].layer, L.groups[k].candidates, L.groups[k].count,
               L.groups[k].first, L.groups[k].first + L.groups[k].count);
    printf("  耗时: 分组 %.3f ms, 剔除 %.3f ms, 合计 %.3f ms\n",
           tg * 1e3, tc * 1e3, (tg + tc) * 1e3);
    printf("  逐项核对: %s\n\n",
           (rep.set_error == 0 && rep.count_error == 0 && rep.crosstalk == 0)
               ? "PASS" : "FAIL");

    int ok = (rep.set_error == 0 && rep.count_error == 0 && rep.crosstalk == 0);
    cull_layered_free(&L);
    free(b);
    return ok ? 0 : 1;
}

int main(void) {
    int rc = 0;
    rc |= bench(50000);
    rc |= bench(200000);
    return rc;
}
