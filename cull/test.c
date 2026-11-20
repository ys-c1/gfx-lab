#include "cull.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

/* 确定性 LCG，保证测试可复现 */
static unsigned long rng_state = 0x12345678UL;
static double rng_double(void) {
    rng_state = rng_state * 6364136223846793005UL + 1442695040888963407UL;
    return (double)(rng_state >> 11) / (double)(1ULL << 53);
}

int main(void) {
    /* ---- 原暴力 API 回归 ---- */
    CullBox b[4] = {{0,0,10,10,0},{100,100,10,10,1},{50,50,20,20,0},{-5,-5,10,10,2}};
    CullView v = {0, 0, 60, 60};
    CullResult r;
    CK(cull_run(b, 4, v, &r) == 0);
    CK(r.count == 3);
    CK(r.ids[0] == 0 && r.ids[1] == 2 && r.ids[2] == 3);
    CK(cull_count_within(b, 4, v) == 3);
    CullView bad = {0, 0, 0, 10};
    CK(cull_run(b, 4, bad, &r) == CULL_ERR_VIEW);
    free(r.ids);

    /* ---- BVH 构建：节点数与层数 ---- */
    CullError err;
    CullBVH tree;
    CK(cull_bvh_build(b, 4, &tree, &err) == CULL_OK);
    CK(tree.leaf_count == 4);
    CK(tree.node_count == 2 * tree.leaf_count - 1); /* 满二叉树 7 个节点 */
    CK(tree.levels == 3);                            /* ceil(log2(4))+1 */
    /* 根包围盒应包住全部对象 */
    CK(tree.nodes[0].minx == -5.0 && tree.nodes[0].miny == -5.0);
    CK(tree.nodes[0].maxx == 110.0 && tree.nodes[0].maxy == 110.0);

    /* ---- 递归剪枝与暴力结果一致（不走层次时可见集合一致） ---- */
    CullResult br;
    CK(cull_run(b, 4, v, &br) == CULL_OK);
    CullResult hr;
    CullQueryStats st;
    CK(cull_bvh_query(&tree, b, v, &hr, &st, &err) == CULL_OK);
    CK(hr.count == br.count);
    CK(cull_result_diff(&hr, &br) == 0);
    CK(st.visible == 3);
    CK(st.candidates >= st.visible && st.candidates <= 4);
    CK(st.visited_nodes >= 1 && st.visited_nodes <= tree.node_count);
    for (int i = 1; i < hr.count; ++i) CK(hr.ids[i - 1] < hr.ids[i]); /* 升序可复现 */
    free(br.ids);
    free(hr.ids);

    /* 空视口命中：可见 0，但根节点仍被访问 1 次 */
    CullView nowhere = {1000, 1000, 10, 10};
    CK(cull_bvh_query(&tree, b, nowhere, &hr, &st, &err) == CULL_OK);
    CK(hr.count == 0 && st.visited_nodes == 1 && st.candidates == 0);
    free(hr.ids);
    cull_bvh_free(&tree);

    /* ---- 退化包围盒：报错并指出下标与字段 ---- */
    CullBox deg[3] = {{0,0,10,10,0},{5,5,0,8,1},{2,2,4,4,2}};
    CK(cull_bvh_build(deg, 3, &tree, &err) == CULL_ERR_DEGENERATE);
    CK(err.index == 1 && err.field != NULL && err.field[0] == 'w');
    CullBox degh[2] = {{0,0,10,-2,0},{1,1,4,4,1}};
    CK(cull_bvh_build(degh, 2, &tree, &err) == CULL_ERR_DEGENERATE);
    CK(err.index == 0 && err.field != NULL && err.field[0] == 'h');
    CullBox deginf[2] = {{0,0,10,10,0},{1,1,NAN,4,1}};
    CK(cull_bvh_build(deginf, 2, &tree, &err) == CULL_ERR_DEGENERATE);
    CK(err.index == 1 && err.field != NULL && err.field[0] == 'w');
    CK(cull_bvh_build(NULL, 3, &tree, &err) == CULL_ERR_ARG);
    CK(cull_bvh_build(b, 0, &tree, &err) == CULL_ERR_ARG);

    /* ---- 层数为零：查询时报错并指出字段 levels ---- */
    CullBVH zero_tree = {0};
    CK(cull_bvh_query(&zero_tree, b, v, &hr, &st, &err) == CULL_ERR_ZERO_LEVELS);
    CK(err.field != NULL && err.field[0] == 'l');
    CK(cull_bvh_build(b, 4, &tree, &err) == CULL_OK);
    CK(cull_bvh_query(&tree, b, bad, &hr, &st, &err) == CULL_ERR_VIEW);
    cull_bvh_free(&tree);

    /* ---- 随机大数据：层次与暴力可见集合一致，且构建/查询可复现 ---- */
    enum { M = 2000 };
    CullBox *big = (CullBox *)malloc(sizeof(CullBox) * M);
    for (int i = 0; i < M; ++i) {
        big[i].x = rng_double() * 900.0;
        big[i].y = rng_double() * 900.0;
        big[i].w = 5.0 + rng_double() * 45.0;
        big[i].h = 5.0 + rng_double() * 45.0;
        big[i].layer = i % 4;
    }
    CullBVH t1, t2;
    CK(cull_bvh_build(big, M, &t1, &err) == CULL_OK);
    rng_state = 0x12345678UL; /* 重置后重建：结构必须完全一致 */
    CK(cull_bvh_build(big, M, &t2, &err) == CULL_OK);
    CK(t1.node_count == t2.node_count && t1.levels == t2.levels &&
       t1.leaf_count == t2.leaf_count);
    CK(t1.node_count == 2 * M - 1);
    CK(t1.levels == 12); /* 最深叶子深度 ceil(log2(2000))=11，层数 = 深度+1 */
    int same_struct = 1;
    for (int i = 0; i < t1.node_count; ++i) {
        const CullNode *p = &t1.nodes[i], *q = &t2.nodes[i];
        if (p->minx != q->minx || p->miny != q->miny ||
            p->maxx != q->maxx || p->maxy != q->maxy ||
            p->left != q->left || p->right != q->right) same_struct = 0;
    }
    CK(same_struct);

    CullView views[3] = {{100, 100, 200, 200}, {0, 0, 50, 50}, {0, 0, 1000, 1000}};
    for (int k = 0; k < 3; ++k) {
        CK(cull_run(big, M, views[k], &br) == CULL_OK);
        CK(cull_bvh_query(&t1, big, views[k], &hr, &st, &err) == CULL_OK);
        CK(cull_result_diff(&hr, &br) == 0);
        CK(st.visited_nodes <= t1.node_count);
        /* 再次查询：结果与统计完全相同（可复现） */
        CullResult hr2;
        CullQueryStats st2;
        CK(cull_bvh_query(&t1, big, views[k], &hr2, &st2, &err) == CULL_OK);
        CK(hr2.count == hr.count && st2.visited_nodes == st.visited_nodes &&
            st2.candidates == st.candidates);
        CK(cull_result_diff(&hr, &hr2) == 0);
        free(br.ids); free(hr.ids); free(hr2.ids);
    }
    cull_bvh_free(&t1);
    cull_bvh_free(&t2);
    free(big);

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
