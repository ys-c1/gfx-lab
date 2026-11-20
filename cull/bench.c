#define _POSIX_C_SOURCE 199309L
#include "cull.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

static unsigned long rng_state = 12345;
static unsigned rnd(void) {
    rng_state = rng_state * 6364136223846793005UL + 1442695040888963407UL;
    return (unsigned)(rng_state >> 33);
}

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* 参考：qsort + (layer, weight, 原下标) 决胜，结果等价于稳定排序 */
static const CullBox *g_boxes;
static const double *g_w;
static long g_cmp;
static int ref_cmp(const void *pa, const void *pb) {
    int a = *(const int*)pa, b = *(const int*)pb;
    ++g_cmp;
    if (g_boxes[a].layer != g_boxes[b].layer)
        return (g_boxes[a].layer > g_boxes[b].layer) - (g_boxes[a].layer < g_boxes[b].layer);
    if (g_w[a] != g_w[b]) return g_w[a] < g_w[b] ? -1 : 1;
    return (a > b) - (a < b);
}

static double mean(const double *v, int n) {
    double s = 0;
    for (int i = 0; i < n; ++i) s += v[i];
    return s / n;
}
static double stddev(const double *v, int n, double m) {
    double s = 0;
    for (int i = 0; i < n; ++i) s += (v[i] - m) * (v[i] - m);
    return sqrt(s / n);
}

int main(void) {
    const int sizes[] = {10000, 100000};
    enum { REPS = 7 };
    printf("%8s | %12s %12s | %8s %10s | %10s %10s | %6s\n",
           "n", "cmp(排序)", "cmp(参考)", "顺序差异", "相对误差", "耗时ms", "参考ms", "照旧");
    for (int s = 0; s < 2; ++s) {
        int n = sizes[s];
        CullBox *boxes = (CullBox*)malloc(sizeof(CullBox) * (size_t)n);
        CullWeight *we = (CullWeight*)malloc(sizeof(CullWeight) * (size_t)n);
        int *ids = (int*)malloc(sizeof(int) * (size_t)n);
        int *out = (int*)malloc(sizeof(int) * (size_t)n);
        int *ref = (int*)malloc(sizeof(int) * (size_t)n);
        double *w = (double*)malloc(sizeof(double) * (size_t)n);
        if (!boxes || !we || !ids || !out || !ref || !w) return 1;
        rng_state = 12345;
        for (int i = 0; i < n; ++i) {
            boxes[i].x = (double)(rnd() % 1000); boxes[i].y = (double)(rnd() % 1000);
            boxes[i].w = boxes[i].h = 1;
            boxes[i].layer = (int)(rnd() % 8);
            w[i] = (double)(rnd() % 1000);   /* 值域小，并列多，考验稳定性 */
            we[i].id = i; we[i].weight = w[i];
            ids[i] = i;
        }
        CullWeightTable t = {we, n};
        double to[REPS], tr[REPS];
        long cmp_o = 0, cmp_r = 0;
        for (int r = 0; r < REPS; ++r) {
            long c = 0; int bad = -1;
            double t0 = now();
            if (cull_order(boxes, ids, n, &t, 1, out, &c, &bad) != 0) {
                printf("cull_order 出错, bad_id=%d\n", bad);
                return 1;
            }
            to[r] = now() - t0; cmp_o = c;
            g_boxes = boxes; g_w = w; g_cmp = 0;
            memcpy(ref, ids, sizeof(int) * (size_t)n);
            t0 = now();
            qsort(ref, (size_t)n, sizeof(int), ref_cmp);
            tr[r] = now() - t0; cmp_r = g_cmp;
        }
        /* 顺序差异与误差：对照稳定排序参考结果 */
        long diff = 0;
        for (int i = 0; i < n; ++i) if (out[i] != ref[i]) ++diff;
        /* 关掉排序：输出必须与输入照旧一致 */
        int bad = -1;
        if (cull_order(boxes, ids, n, &t, 0, out, NULL, &bad) != 0) return 1;
        int ident = 1;
        for (int i = 0; i < n; ++i) if (out[i] != ids[i]) { ident = 0; break; }
        double mo = mean(to, REPS), mr = mean(tr, REPS);
        printf("%8d | %12ld %12ld | %8ld %10.2e | %7.2f±%-5.2f %7.2f±%-5.2f | %s\n",
               n, cmp_o, cmp_r, diff, n ? (double)diff / n : 0.0,
               mo * 1e3, stddev(to, REPS, mo) * 1e3,
               mr * 1e3, stddev(tr, REPS, mr) * 1e3,
               ident ? "OK" : "FAIL");
        free(boxes); free(we); free(ids); free(out); free(ref); free(w);
    }
    return 0;
}
