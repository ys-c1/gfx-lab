#include "cull.h"
#include <stdio.h>
#include <stdlib.h>
static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

/* 参考排序：qsort + (layer, weight, 原下标) 决胜，结果等价于稳定排序 */
static const CullBox *g_boxes;
static const double *g_w;
static int ref_cmp(const void *pa, const void *pb) {
    int a = *(const int*)pa, b = *(const int*)pb;
    if (g_boxes[a].layer != g_boxes[b].layer)
        return (g_boxes[a].layer > g_boxes[b].layer) - (g_boxes[a].layer < g_boxes[b].layer);
    if (g_w[a] != g_w[b]) return g_w[a] < g_w[b] ? -1 : 1;
    return (a > b) - (a < b);
}

static unsigned long rng_state = 88172645463325252UL;
static unsigned rnd(void) {
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 7; rng_state ^= rng_state << 17;
    return (unsigned)rng_state;
}

static void test_order(void) {
    CullBox b[5] = {{0,0,1,1,0},{0,0,1,1,0},{0,0,1,1,1},{0,0,1,1,0},{0,0,1,1,1}};
    int ids[5] = {0,1,2,3,4};
    CullWeight we[3] = {{0,5},{1,2},{3,7}};
    CullWeightTable t = {we, 3};
    int out[5]; long cmp = -1; int bad = -1;
    /* 同层按权重升序；跨层先按层号；未登记权重为 0 */
    CK(cull_order(b, ids, 5, &t, 1, out, &cmp, &bad) == 0);
    CK(out[0]==1 && out[1]==0 && out[2]==3 && out[3]==2 && out[4]==4);
    CK(cmp > 0);
    /* 关掉排序，顺序必须照旧 */
    cmp = -1;
    CK(cull_order(b, ids, 5, &t, 0, out, &cmp, &bad) == 0);
    CK(out[0]==0 && out[1]==1 && out[2]==2 && out[3]==3 && out[4]==4);
    CK(cmp == 0);
    /* 负权重：抛错并指明对象 */
    CullWeight wneg[2] = {{2,-1},{0,1}};
    CullWeightTable tn = {wneg, 2};
    CK(cull_order(b, ids, 5, &tn, 1, out, &cmp, &bad) == -3);
    CK(bad == 2);
    CK(cull_weights_validate(&tn, &bad) == -3 && bad == 2);
    /* 同对象重复登记且权重冲突：抛错并指明对象 */
    CullWeight wdup[2] = {{3,1},{3,2}};
    CullWeightTable td = {wdup, 2};
    CK(cull_order(b, ids, 5, &td, 1, out, &cmp, &bad) == -4);
    CK(bad == 3);
    /* 重复登记但同值：不冲突 */
    CullWeight wsame[2] = {{3,1},{3,1}};
    CullWeightTable ts = {wsame, 2};
    CK(cull_order(b, ids, 5, &ts, 1, out, &cmp, &bad) == 0);
    /* 未登记对象权重为 0 */
    CK(cull_weight_of(&t, 99) == 0.0);
    CK(cull_weight_of(&t, 0) == 5.0);
    /* 空表 / NULL 表：合法，全部默认权重 */
    CK(cull_order(b, ids, 5, NULL, 1, out, &cmp, &bad) == 0);
    CK(out[0]==0 && out[4]==4); /* 全同键，稳定保持原序 */
}

/* 排序稳定性单独核对 */
static void test_stability(void) {
    enum { N = 2000, ROUNDS = 20 };
    CullBox *b = (CullBox*)malloc(sizeof(CullBox) * N);
    CullWeight *we = (CullWeight*)malloc(sizeof(CullWeight) * N);
    int *ids = (int*)malloc(sizeof(int) * N);
    int *out = (int*)malloc(sizeof(int) * N);
    int *ref = (int*)malloc(sizeof(int) * N);
    double *w = (double*)malloc(sizeof(double) * N);
    int *pos = (int*)malloc(sizeof(int) * N);
    CK(b && we && ids && out && ref && w && pos);
    for (int round = 0; round < ROUNDS; ++round) {
        for (int i = 0; i < N; ++i) {
            b[i].x = b[i].y = 0; b[i].w = b[i].h = 1;
            b[i].layer = (int)(rnd() % 4);          /* 少量层，大量同层 */
            w[i] = (double)(rnd() % 5);             /* 少量权重值，大量并列 */
            we[i].id = i; we[i].weight = w[i];
            ids[i] = i;
        }
        CullWeightTable t = {we, N};
        long cmp; int bad;
        CK(cull_order(b, ids, N, &t, 1, out, &cmp, &bad) == 0);
        /* 1) 与参考稳定排序逐位一致 */
        g_boxes = b; g_w = w;
        for (int i = 0; i < N; ++i) ref[i] = i;
        qsort(ref, N, sizeof(int), ref_cmp);
        int same = 1;
        for (int i = 0; i < N; ++i) if (out[i] != ref[i]) { same = 0; break; }
        CK(same);
        /* 2) 并列键保持原相对顺序 */
        for (int i = 0; i < N; ++i) pos[out[i]] = i;
        int stable = 1;
        for (int i = 0; i < N && stable; ++i)
            for (int j = i + 1; j < N; ++j)
                if (b[i].layer == b[j].layer && w[i] == w[j] && pos[i] > pos[j]) { stable = 0; break; }
        CK(stable);
    }
    /* 全部同键：输出必须与输入完全一致 */
    for (int i = 0; i < N; ++i) { b[i].layer = 0; we[i].weight = 3.0; ids[i] = i; }
    CullWeightTable t = {we, N};
    CK(cull_order(b, ids, N, &t, 1, out, NULL, NULL) == 0);
    int ident = 1;
    for (int i = 0; i < N; ++i) if (out[i] != i) { ident = 0; break; }
    CK(ident);
    free(b); free(we); free(ids); free(out); free(ref); free(w); free(pos);
}

static void test_export(void) {
    CullWeight we[3] = {{7, 1.5}, {2, 0.25}, {9, 4}};
    CullWeightTable t = {we, 3};
    CK(cull_weights_export(&t, "build/test_weights.txt") == 0);
    FILE *f = fopen("build/test_weights.txt", "r");
    CK(f != NULL);
    if (f) {
        int id; double w; int n = 0, ok = 1;
        while (fscanf(f, "%d %lf", &id, &w) == 2) {
            if (id != we[n].id || w != we[n].weight) ok = 0;
            ++n;
        }
        fclose(f);
        CK(n == 3 && ok);
    }
    CK(cull_weights_export(&t, NULL) == -1);
    CK(cull_weights_export(NULL, "build/x.txt") == -1);
}

int main(void) {
    CullBox b[4] = {{0,0,10,10,0},{100,100,10,10,1},{50,50,20,20,0},{-5,-5,10,10,2}};
    CullView v = {0, 0, 60, 60};
    CullResult r;
    CK(cull_run(b, 4, v, &r) == 0);
    CK(r.count == 3);
    CK(r.ids[0] == 0 && r.ids[1] == 2 && r.ids[2] == 3);
    CK(cull_count_within(b, 4, v) == 3);
    CullView bad = {0, 0, 0, 10};
    CK(cull_run(b, 4, bad, &r) == -2);
    free(r.ids);
    test_order();
    test_stability();
    test_export();
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
