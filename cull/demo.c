#define _POSIX_C_SOURCE 200809L
#include "cull.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* 确定性 LCG（固定种子）：同一份数据、同一棵树，结果逐字节可复现 */
static unsigned long rng_state;
static double rng_double(void) {
    rng_state = rng_state * 6364136223846793005UL + 1442695040888963407UL;
    return (double)(rng_state >> 11) / (double)(1ULL << 53);
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
}

static void emit(FILE *fp, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
}

#define ITERS 20

static void benchmark(FILE *fp, int n) {
    CullBox *boxes = (CullBox *)malloc(sizeof(CullBox) * (size_t)n);
    rng_state = 0xC0FFEEUL;
    for (int i = 0; i < n; ++i) {
        boxes[i].x = rng_double() * 9000.0;
        boxes[i].y = rng_double() * 9000.0;
        boxes[i].w = 5.0 + rng_double() * 45.0;
        boxes[i].h = 5.0 + rng_double() * 45.0;
        boxes[i].layer = i % 8;
    }

    CullError err;
    CullBVH tree;
    double t0 = now_ms();
    int rc = cull_bvh_build(boxes, n, &tree, &err);
    double build_ms = now_ms() - t0;
    if (rc != CULL_OK) {
        emit(fp, "build failed: %s\n", err.message);
        free(boxes);
        return;
    }
    emit(fp,
         "=== n=%d ===\n"
         "BVH: total_nodes=%d leaf_nodes=%d levels=%d build=%.3f ms\n",
         n, tree.node_count, tree.leaf_count, tree.levels, build_ms);

    /* 小视口（强剪枝）与全视口（最坏情况，所有对象都是候选） */
    CullView views[2] = {{1200, 2400, 300, 300}, {0, 0, 10000, 10000}};
    const char *names[2] = {"small view 300x300", "full view 10000x10000"};

    for (int k = 0; k < 2; ++k) {
        CullView view = views[k];

        /* 暴力遍历对照 */
        double bf_total = 0.0, bf_best = 1e30;
        CullResult bf = {0};
        for (int it = 0; it < ITERS; ++it) {
            double s = now_ms();
            int cr = cull_run(boxes, n, view, &bf);
            double e = now_ms() - s;
            if (cr != CULL_OK) { emit(fp, "brute force failed\n"); return; }
            bf_total += e;
            if (e < bf_best) bf_best = e;
            if (it + 1 < ITERS) free(bf.ids);
        }

        /* 层次递归剪枝（统计在最后一轮采集，每轮统计完全相同） */
        double hv_total = 0.0, hv_best = 1e30;
        CullResult hv = {0};
        CullQueryStats st = {0};
        for (int it = 0; it < ITERS; ++it) {
            double s = now_ms();
            int cr = cull_bvh_query(&tree, boxes, view, &hv, &st, &err);
            double e = now_ms() - s;
            if (cr != CULL_OK) { emit(fp, "bvh query failed: %s\n", err.message); return; }
            hv_total += e;
            if (e < hv_best) hv_best = e;
            if (it + 1 < ITERS) free(hv.ids);
        }

        int diff = cull_result_diff(&hv, &bf);
        emit(fp,
             "[%s]\n"
             "  brute force : visible=%d  avg=%.4f ms  best=%.4f ms  (tests %d objects/frame)\n"
             "  bvh culling : visible=%d  avg=%.4f ms  best=%.4f ms\n"
             "                visited_nodes=%d (%.2f%% of %d)  candidates=%d (%.2f%% of n)\n"
             "                tree_levels=%d  set_diff_vs_brute=%d  speedup=%.2fx\n",
             names[k],
             bf.count, bf_total / ITERS, bf_best, n,
             hv.count, hv_total / ITERS, hv_best,
             st.visited_nodes, 100.0 * st.visited_nodes / tree.node_count, tree.node_count,
             st.candidates, 100.0 * st.candidates / n,
             tree.levels, diff,
             hv_total > 0 ? (bf_total / ITERS) / (hv_total / ITERS) : 0.0);

        free(bf.ids);
        free(hv.ids);
    }
    cull_bvh_free(&tree);
    free(boxes);
}

/* 错误路径演示：退化包围盒与零层结构都必须报错并指出字段 */
static void demo_errors(FILE *fp) {
    CullError err;
    CullBVH tree;
    CullBox deg[3] = {{0, 0, 10, 10, 0}, {4, 4, 0, 8, 1}, {2, 2, 5, 5, 2}};
    int rc = cull_bvh_build(deg, 3, &tree, &err);
    emit(fp, "degenerate box -> rc=%d index=%d field=%s msg=%s\n",
         rc, err.index, err.field ? err.field : "(null)", err.message);

    CullBox ok[2] = {{0, 0, 10, 10, 0}, {20, 20, 10, 10, 1}};
    if (cull_bvh_build(ok, 2, &tree, &err) == CULL_OK) {
        CullView bad = {0, 0, -1, 10};
        CullResult out;
        CullQueryStats st;
        rc = cull_bvh_query(&tree, ok, bad, &out, &st, &err);
        emit(fp, "invalid view   -> rc=%d field=%s msg=%s\n",
             rc, err.field ? err.field : "(null)", err.message);
        cull_bvh_free(&tree);
    }

    CullBVH zero = {0};
    CullResult out;
    CullQueryStats st;
    CullBox one = {0, 0, 1, 1, 0};
    CullView v = {0, 0, 10, 10};
    rc = cull_bvh_query(&zero, &one, v, &out, &st, &err);
    emit(fp, "zero levels    -> rc=%d index=%d field=%s msg=%s\n",
         rc, err.index, err.field ? err.field : "(null)", err.message);
}

int main(void) {
    FILE *fp = fopen("demo.txt", "w");
    if (!fp) { perror("demo.txt"); return 1; }
    emit(fp, "gfx-lab cull: BVH build + recursive viewport culling vs brute force\n");
    emit(fp, "timing: %d iterations, average and best shown; LCG seed fixed (reproducible)\n\n",
         ITERS);
    demo_errors(fp);
    emit(fp, "\n");
    benchmark(fp, 10000);
    emit(fp, "\n");
    benchmark(fp, 100000);
    fclose(fp);
    return 0;
}
