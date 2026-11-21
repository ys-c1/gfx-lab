#include "cull.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Deterministic LCG so the demo prints the same numbers every run. */
static unsigned long long seed = 12345;
static double rnd01(void) {
    seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
    return (double)(seed >> 11) / (double)(1ULL << 53);
}

static int check_legacy(const CullBox *b, int n, CullView v, const CullResult *r) {
    CullHashOpts o;
    CullHashError err;
    CullHashResult lr;
    memset(&o, 0, sizeof o);
    o.legacy = 1;
    if (cull_run_hashed(b, n, v, &o, &lr, &err) != CULL_OK) return 0;
    int same = lr.count == r->count;
    for (int i = 0; same && i < r->count; ++i) same = lr.ids[i] == r->ids[i];
    cull_hash_result_free(&lr);
    return same;
}

static void scene_grid(void) {
    enum { N = 64 };
    CullBox b[N];
    for (int i = 0; i < N; ++i) {
        b[i].x = (i % 8) * 20; b[i].y = (i / 8) * 20;
        b[i].w = 18; b[i].h = 18; b[i].layer = i % 3;
    }
    CullView v = {0, 0, 100, 100};

    CullResult r;
    if (cull_run(b, N, v, &r) != 0) return;
    printf("[规则网格] 逐对象扫描: 可见 %d / %d\n", r.count, N);

    CullHashOpts o;
    CullHashError err;
    CullHash *h = NULL;
    memset(&o, 0, sizeof o);
    o.cell_w = 24; o.cell_h = 24; /* 手选格子，便于和对象尺寸对照 */
    if (cull_hash_build(b, N, &o, &h, &err) != CULL_OK) {
        printf("  build error %d: %s\n", err.code, err.message);
        free(r.ids);
        return;
    }
    CullHashResult q;
    if (cull_hash_query(h, v, &q, &err) != CULL_OK) {
        printf("  query error %d: %s\n", err.code, err.message);
        cull_hash_free(h); free(r.ids);
        return;
    }
    printf("  两段式: 可见 %d，候选 %ld，桶 %d（访问 %d），冲突 %ld，冲突率 %.2f%%\n",
           q.count, q.candidates, q.stats.buckets, q.visited_buckets,
           q.stats.collisions, q.stats.collision_rate);
    printf("  关闭新分支: 输出%s\n", check_legacy(b, N, v, &r) ? "逐项照旧" : "不一致!");
    if (cull_hash_export_buckets(h, "build/demo_buckets.txt") == CULL_OK)
        printf("  桶表已导出: build/demo_buckets.txt\n");
    cull_hash_result_free(&q);
    cull_hash_free(h);
    free(r.ids);
}

static void scene_dense(void) {
    enum { N = 5000 };
    CullBox *b = (CullBox*)malloc(sizeof(CullBox) * N);
    if (!b) return;
    for (int i = 0; i < N; ++i) {
        b[i].x = rnd01() * 4000.0;
        b[i].y = rnd01() * 4000.0;
        b[i].w = 4.0 + rnd01() * 40.0;
        b[i].h = 4.0 + rnd01() * 40.0;
        b[i].layer = i % 8;
    }
    /* 小视口：大部分对象不再被逐个测试 */
    CullView v = {1000, 1000, 200, 150};

    CullResult r;
    if (cull_run(b, N, v, &r) != 0) { free(b); return; }
    CullHashOpts o;
    CullHashError err;
    CullHash *h = NULL;
    memset(&o, 0, sizeof o); /* 自动选格子 */
    if (cull_hash_build(b, N, &o, &h, &err) != CULL_OK) {
        printf("  build error %d: %s\n", err.code, err.message);
        free(r.ids); free(b);
        return;
    }
    CullHashResult q;
    if (cull_hash_query(h, v, &q, &err) != CULL_OK) {
        printf("  query error %d: %s\n", err.code, err.message);
        cull_hash_free(h); free(r.ids); free(b);
        return;
    }
    printf("\n[5000 个随机对象，小视口] 逐对象扫描: 可见 %d\n", r.count);
    printf("  两段式: 可见 %d，候选 %ld / %d（%.1fx 更少测试），桶 %d（访问 %d）\n",
           q.count, q.candidates, N, (double)N / (double)(q.candidates ? q.candidates : 1),
           q.stats.buckets, q.visited_buckets);
    printf("  冲突 %ld，冲突率 %.2f%%；误差 %ld\n",
           q.stats.collisions, q.stats.collision_rate,
           (long)(r.count - q.count));
    printf("  关闭新分支: 输出%s\n", check_legacy(b, N, v, &r) ? "逐项照旧" : "不一致!");
    cull_hash_result_free(&q);
    cull_hash_free(h);
    free(r.ids);
    free(b);
}

int main(void) {
    scene_grid();
    scene_dense();
    return 0;
}
