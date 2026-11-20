#include "cull.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

int main(void) {
    CullBox b[4] = {{0,0,10,10,0,0},{100,100,10,10,1,1},{50,50,20,20,0,2},{-5,-5,10,10,2,3}};
    CullView v = {0, 0, 60, 60};
    CullResult r;
    CK(cull_run(b, 4, v, &r) == 0);
    CK(r.count == 3);
    CK(r.ids[0] == 0 && r.ids[1] == 2 && r.ids[2] == 3);
    CK(cull_count_within(b, 4, v) == 3);
    CullView bad = {0, 0, 0, 10};
    CK(cull_run(b, 4, bad, &r) == -2);
    free(r.ids);

    /* 分层: 层间按层号升序, 桶内按 seq 升序 */
    CullBox lb[6] = {
        {0,0,10,10, 2, 0},
        {0,0,10,10, 0, 5},
        {0,0,10,10, 2, 1},
        {0,0,10,10, 0, 2},
        {0,0,10,10, 1, 3},
        {1000,1000,10,10, 0, 4}, /* 被裁 */
    };
    CullView lv = {0, 0, 100, 100};
    CullLayered L; CullStatus st;
    CK(cull_run_layered(lb, 6, lv, 1, &L, &st) == CULL_OK);
    CK(L.count == 5);
    int expect[5] = {3, 1, 4, 0, 2}; /* 层0: seq2,5 -> 下标3,1; 层1: 4; 层2: seq0,1 -> 0,2 */
    CK(memcmp(L.ids, expect, sizeof(expect)) == 0);
    CK(L.bucket_count == 3);
    CK(L.buckets[0].layer == 0 && L.buckets[0].visible == 2 && L.buckets[0].clipped == 1);
    CK(L.buckets[1].layer == 1 && L.buckets[1].visible == 1 && L.buckets[1].clipped == 0);
    CK(L.buckets[2].layer == 2 && L.buckets[2].visible == 2 && L.buckets[2].clipped == 0);
    cull_layered_free(&L);
    CK(L.ids == 0 && L.buckets == 0 && L.count == 0);

    /* 关闭分层: 可见集合与顺序和 cull_run 逐项相同 */
    CullLayered P;
    CullResult base;
    CK(cull_run_layered(lb, 6, lv, 0, &P, &st) == CULL_OK);
    CK(cull_run(lb, 6, lv, &base) == 0);
    CK(P.count == base.count);
    CK(memcmp(P.ids, base.ids, sizeof(int) * (size_t)base.count) == 0);
    CK(P.bucket_count == 3); /* 统计仍然给出 */
    cull_layered_free(&P);
    free(base.ids);

    /* 拒绝: layer 为负, 指出字段与下标 */
    CullBox neg[2] = {{0,0,1,1,0,0},{0,0,1,1,-3,1}};
    CK(cull_run_layered(neg, 2, lv, 1, &L, &st) == CULL_ERR_LAYER_NEGATIVE);
    CK(st.code == CULL_ERR_LAYER_NEGATIVE && st.index == 1);
    CK(st.field && strcmp(st.field, "layer") == 0);

    /* 拒绝: seq 重复, 报告后插入者下标 */
    CullBox dup[3] = {{0,0,1,1,0,7},{0,0,1,1,0,3},{0,0,1,1,0,7}};
    CK(cull_run_layered(dup, 3, lv, 1, &L, &st) == CULL_ERR_SEQ_DUP);
    CK(st.index == 2);
    CK(st.field && strcmp(st.field, "seq") == 0);

    /* 拒绝: 桶数超上限 */
    CullBox many[CULL_MAX_BUCKETS + 1];
    for (int i = 0; i <= CULL_MAX_BUCKETS; ++i) {
        many[i].x = 0; many[i].y = 0; many[i].w = 1; many[i].h = 1;
        many[i].layer = i; many[i].seq = i;
    }
    CK(cull_run_layered(many, CULL_MAX_BUCKETS + 1, lv, 1, &L, &st) == CULL_ERR_BUCKETS_OVERFLOW);
    CK(st.index == CULL_MAX_BUCKETS);
    CK(st.field && strcmp(st.field, "layer") == 0);

    /* 边界: n=0 / 空视口 / 空指针 */
    CK(cull_run_layered(lb, 0, lv, 1, &L, &st) == CULL_OK && L.count == 0 && L.bucket_count == 0);
    cull_layered_free(&L);
    CK(cull_run_layered(lb, 6, bad, 1, &L, &st) == CULL_ERR_VIEW);
    CK(cull_run_layered(0, 6, lv, 1, &L, &st) == CULL_ERR_NULL);

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
