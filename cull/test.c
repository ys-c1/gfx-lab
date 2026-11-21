#include "cull.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

/* 参考工具（独立实现）：对单层子集按输入顺序做暴力剔除。
   仅用于测试，不复用库里的 overlaps。 */
static int ref_visible(const CullBox *b, int n, int layer, CullView v) {
    int c = 0;
    for (int i = 0; i < n; ++i) {
        if (b[i].layer != layer) continue;
        double l = b[i].x, r = b[i].x + b[i].w;
        double t = b[i].y, bo = b[i].y + b[i].h;
        if (!(r < v.x || l > v.x + v.w || bo < v.y || t > v.y + v.h)) ++c;
    }
    return c;
}

/* 参考工具：整表暴力剔除，返回可见 id 列表（升序）。 */
static int ref_run(const CullBox *b, int n, CullView v, int *ids) {
    int c = 0;
    for (int i = 0; i < n; ++i) {
        double l = b[i].x, r = b[i].x + b[i].w;
        double t = b[i].y, bo = b[i].y + b[i].h;
        if (!(r < v.x || l > v.x + v.w || bo < v.y || t > v.y + v.h)) ids[c++] = i;
    }
    return c;
}

static int cmp_int(const void *a, const void *b) {
    int x = *(const int*)a, y = *(const int*)b;
    return (x > y) - (x < y);
}

/* 每组的可见 id 是否恰好落在声明的边界区间内、且组内升序。 */
static void check_boundaries(const CullLayered *L, CullError *st) {
    int expect = 0;
    for (int k = 0; k < L->group_count; ++k) {
        CK(L->groups[k].first == expect);
        CK(L->groups[k].count >= 0 && L->groups[k].count <= L->groups[k].candidates);
        if (L->groups[k].count > 0)
            for (int m = 1; m < L->groups[k].count; ++m)
                CK(L->ids[L->groups[k].first + m - 1] < L->ids[L->groups[k].first + m]);
        expect += L->groups[k].count;
    }
    CK(expect == L->count);
    CK(st->code == CULL_OK);
}

int main(void) {
    /* ---- 旧接口不变 ---- */
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

    /* ---- 分层：层号升序、组内输入序，边界明确 ---- */
    CullBox lb[6] = {
        {0,0,10,10, 2},   /* 0: layer2 可见 */
        {20,20,10,10, 0}, /* 1: layer0 可见 */
        {500,500,10,10, 1}, /* 2: layer1 被裁 */
        {30,30,10,10, 0}, /* 3: layer0 可见 */
        {0,0,10,10, 1},   /* 4: layer1 可见 */
        {150,150,10,10, 0}, /* 5: layer0 被裁（完全在视口右侧） */
    };
    CullView lv = {0, 0, 100, 100};
    CullLayered L;
    CullError st;
    CK(cull_run_layered(lb, 6, lv, 1, &L, &st) == CULL_OK);
    CK(L.count == 4);
    CK(L.group_count == 3);
    CK(L.layer_limit == CULL_MAX_LAYERS);
    /* 层号升序: 0,1,2 */
    CK(L.groups[0].layer == 0 && L.groups[1].layer == 1 && L.groups[2].layer == 2);
    /* 各组候选数 = 该层对象数 */
    CK(L.groups[0].candidates == 3 && L.groups[1].candidates == 2 && L.groups[2].candidates == 1);
    /* 各组可见数 */
    CK(L.groups[0].count == 2 && L.groups[1].count == 1 && L.groups[2].count == 1);
    /* 边界: layer0 [0,2), layer1 [2,3), layer2 [3,4) */
    CK(L.groups[0].first == 0 && L.groups[1].first == 2 && L.groups[2].first == 3);
    /* ids: layer0 的 1,3; layer1 的 4; layer2 的 0 */
    CK(L.ids[0] == 1 && L.ids[1] == 3 && L.ids[2] == 4 && L.ids[3] == 0);
    check_boundaries(&L, &st);
    cull_layered_free(&L);
    CK(L.ids == NULL && L.groups == NULL && L.count == 0 && L.group_count == 0);

    /* ---- 组间串扰为 0：逐组独立重算，结果逐项一致 ---- */
    {
        CK(cull_run_layered(lb, 6, lv, 1, &L, &st) == CULL_OK);
        int crosstalk = 0;
        for (int k = 0; k < L.group_count; ++k) {
            int layer = L.groups[k].layer;
            /* 1) 区间内的每个 id 必须属于本层 */
            for (int m = 0; m < L.groups[k].count; ++m) {
                int id = L.ids[L.groups[k].first + m];
                if (lb[id].layer != layer) ++crosstalk;
            }
            /* 2) 与该层单独跑参考工具的数量一致 */
            if (L.groups[k].count != ref_visible(lb, 6, layer, lv)) ++crosstalk;
            /* 3) 与把该层单独喂给旧接口的结果一致 */
            CullBox sub[6]; int sn = 0;
            for (int i = 0; i < 6; ++i) if (lb[i].layer == layer) sub[sn++] = lb[i];
            CullResult rr;
            CK(cull_run(sub, sn, lv, &rr) == 0);
            if (rr.count != L.groups[k].count) ++crosstalk;
            /* 子集结果里的相对下标映射回原下标后与分组输出相等 */
            int expect_ids[6], ei = 0, si = 0;
            for (int i = 0; i < 6; ++i) if (lb[i].layer == layer) {
                for (int q = 0; q < rr.count; ++q) if (rr.ids[q] == si) expect_ids[ei++] = i;
                ++si;
            }
            for (int m = 0; m < ei && m < L.groups[k].count; ++m)
                if (expect_ids[m] != L.ids[L.groups[k].first + m]) ++crosstalk;
            free(rr.ids);
        }
        CK(crosstalk == 0);
        cull_layered_free(&L);
    }

    /* ---- 关闭新分支：输出与 cull_run 逐项一致，且不校验层号/上限 ---- */
    {
        CullResult base;
        CK(cull_run(lb, 6, lv, &base) == 0);
        CK(cull_run_layered(lb, 6, lv, 0, &L, &st) == CULL_OK);
        CK(L.count == base.count);
        CK(memcmp(L.ids, base.ids, sizeof(int) * (size_t)base.count) == 0);
        CK(L.group_count == 0 && L.groups == NULL); /* 关闭时不提供分组 */
        cull_layered_free(&L);
        free(base.ids);

        /* 关闭分支下，负层号照旧可跑 */
        CullBox neg[2] = {{0,0,1,1,0},{-1,0,1,1,-7}};
        CK(cull_run_layered(neg, 2, lv, 0, &L, &st) == CULL_OK);
        CK(L.count == 2 && L.ids[1] == 1);
        cull_layered_free(&L);

        /* 关闭分支下，超上限的层数照旧可跑 */
        CullBox many[CULL_MAX_LAYERS + 1];
        for (int i = 0; i <= CULL_MAX_LAYERS; ++i) {
            many[i].x = 0; many[i].y = 0; many[i].w = 1; many[i].h = 1;
            many[i].layer = i;
        }
        CK(cull_run_layered(many, CULL_MAX_LAYERS + 1, lv, 0, &L, &st) == CULL_OK);
        CK(L.count == CULL_MAX_LAYERS + 1 && L.group_count == 0);
        cull_layered_free(&L);
    }

    /* ---- 层号非法：报错并指出下标与字段 ---- */
    {
        CullBox neg[3] = {{0,0,1,1,0},{0,0,1,1,2},{0,0,1,1,-1}};
        CK(cull_run_layered(neg, 3, lv, 1, &L, &st) == CULL_ERR_LAYER);
        CK(st.code == CULL_ERR_LAYER && st.index == 2 && st.layer == -1);
        CK(st.field && strcmp(st.field, "layer") == 0);
        CK(st.limit == CULL_MAX_LAYERS);
    }

    /* ---- 超限：报错并给出上限值、实际分组数、越界层号 ---- */
    {
        enum { N = CULL_MAX_LAYERS + 2 }; /* 65 个不同层号 */
        CullBox many[N];
        for (int i = 0; i < N; ++i) {
            many[i].x = 0; many[i].y = 0; many[i].w = 1; many[i].h = 1;
            many[i].layer = i; /* 层号 0..65 */
        }
        CK(cull_run_layered(many, N, lv, 1, &L, &st) == CULL_ERR_LAYER_LIMIT);
        CK(st.code == CULL_ERR_LAYER_LIMIT);
        CK(st.limit == CULL_MAX_LAYERS);      /* 上限值 */
        CK(st.found == N);                    /* 实际分组数 */
        CK(st.layer == CULL_MAX_LAYERS);      /* 升序第 65 个层号 */
        CK(st.index == CULL_MAX_LAYERS);      /* 该层首个对象下标 */
        CK(st.field && strcmp(st.field, "layer") == 0);
        CK(L.ids == NULL && L.groups == NULL); /* 失败不留分配 */

        /* 恰好等于上限：合法 */
        CK(cull_run_layered(many, CULL_MAX_LAYERS, lv, 1, &L, &st) == CULL_OK);
        CK(L.group_count == CULL_MAX_LAYERS && L.count == CULL_MAX_LAYERS);
        cull_layered_free(&L);
    }

    /* ---- 层号很大但分组数很少：不应按层号跨度分配内存 ---- */
    {
        CullBox spread[3] = {{0,0,5,5, 0},{0,0,5,5, 1000000000},{-10,-10,5,5, 999999999}};
        CK(cull_run_layered(spread, 3, lv, 1, &L, &st) == CULL_OK);
        CK(L.group_count == 3);
        CK(L.groups[0].layer == 0 && L.groups[1].layer == 999999999 && L.groups[2].layer == 1000000000);
        CK(L.count == 2);
        cull_layered_free(&L);
    }

    /* ---- 段一/段二可单独使用，两段结果与合并接口一致 ---- */
    {
        CullGrouping g;
        CK(cull_group_by_layer(lb, 6, &g, &st) == CULL_OK);
        CK(g.n == 6 && g.group_count == 3);
        CK(g.groups[0].layer == 0 && g.groups[0].first == 0 && g.groups[0].count == 3);
        CK(g.groups[1].layer == 1 && g.groups[1].first == 3 && g.groups[1].count == 2);
        CK(g.groups[2].layer == 2 && g.groups[2].first == 5 && g.groups[2].count == 1);
        /* 段一只分组不剔除：members 覆盖全部对象一次 */
        int seen[6] = {0,0,0,0,0,0};
        for (int i = 0; i < g.n; ++i) { CK(g.members[i] >= 0 && g.members[i] < 6); ++seen[g.members[i]]; }
        for (int i = 0; i < 6; ++i) CK(seen[i] == 1);

        CullLayered L2;
        CK(cull_run_grouped(lb, &g, lv, &L2, &st) == CULL_OK);
        CK(L2.count == 4 && L2.group_count == 3);
        CK(L2.ids[0] == 1 && L2.ids[1] == 3 && L2.ids[2] == 4 && L2.ids[3] == 0);
        cull_layered_free(&L2);
        cull_grouping_free(&g);
        CK(g.members == NULL && g.groups == NULL);
    }

    /* ---- 非法分组结构被段二拒绝（不越界读） ---- */
    {
        CullGrouping g;
        CK(cull_group_by_layer(lb, 6, &g, &st) == CULL_OK);
        g.groups[1].first = 99; /* 破坏边界连续性 */
        CK(cull_run_grouped(lb, &g, lv, &L, &st) == CULL_ERR_ARG);
        g.groups[1].first = 3;
        g.groups[1].layer = 0; /* 破坏层号升序 */
        CK(cull_run_grouped(lb, &g, lv, &L, &st) == CULL_ERR_ARG);
        cull_grouping_free(&g);
    }

    /* ---- 空输入 / 空视口 / 空指针 ---- */
    {
        CK(cull_run_layered(lb, 0, lv, 1, &L, &st) == CULL_OK);
        CK(L.count == 0 && L.group_count == 0);
        cull_layered_free(&L);
        CK(cull_run_layered(lb, 6, bad, 1, &L, &st) == CULL_ERR_VIEW);
        CK(cull_run_layered(lb, 6, bad, 0, &L, &st) == CULL_ERR_VIEW);
        CK(cull_run_layered(NULL, 6, lv, 1, &L, &st) == CULL_ERR_ARG);
        CK(cull_run_layered(lb, -1, lv, 1, &L, &st) == CULL_ERR_ARG);
        CK(cull_run_layered(lb, 6, lv, 1, NULL, &st) == CULL_ERR_ARG);
        CK(cull_run_layered(lb, 6, lv, 1, &L, NULL) == CULL_OK); /* err 可为 NULL */
        cull_layered_free(&L);
    }

    /* ---- 单层 / 全裁 / 全可见 ---- */
    {
        CullBox one[2] = {{0,0,5,5,3},{200,200,5,5,3}};
        CK(cull_run_layered(one, 2, lv, 1, &L, &st) == CULL_OK);
        CK(L.group_count == 1 && L.groups[0].candidates == 2 && L.groups[0].count == 1);
        CK(L.ids[0] == 0 && L.groups[0].first == 0);
        cull_layered_free(&L);

        CullBox none[2] = {{500,500,1,1,0},{600,600,1,1,1}};
        CK(cull_run_layered(none, 2, lv, 1, &L, &st) == CULL_OK);
        CK(L.count == 0 && L.group_count == 2);
        CK(L.groups[0].count == 0 && L.groups[1].count == 0);
        CK(L.groups[0].first == 0 && L.groups[1].first == 0); /* 空区间 */
        check_boundaries(&L, &st);
        cull_layered_free(&L);
    }

    /* ---- 大规模随机对照参考工具（两次规模）---- */
    {
        unsigned long long s = 0x2545f4914f6cdd1dull;
        int sizes[2] = {1000, 20000};
        for (int t = 0; t < 2; ++t) {
            int n = sizes[t];
            CullBox *rb = (CullBox*)malloc(sizeof(CullBox) * (size_t)n);
            int *ref = (int*)malloc(sizeof(int) * (size_t)n);
            int *sorted = (int*)malloc(sizeof(int) * (size_t)n);
            if (!rb || !ref || !sorted) { CK(0); free(rb); free(ref); free(sorted); continue; }
            for (int i = 0; i < n; ++i) {
                s ^= s << 13; s ^= s >> 7; s ^= s << 17;
                rb[i].x = (double)(s % 2000) - 500.0;
                s ^= s << 13; s ^= s >> 7; s ^= s << 17;
                rb[i].y = (double)(s % 2000) - 500.0;
                rb[i].w = 20.0 + (double)(s % 40);
                rb[i].h = 20.0 + (double)(s % 40);
                rb[i].layer = (int)(s % 6);
            }
            int nref = ref_run(rb, n, lv, ref);
            CK(cull_run_layered(rb, n, lv, 1, &L, &st) == CULL_OK);
            CK(L.count == nref);
            CK(L.group_count == 6);
            for (int k = 0; k < L.count; ++k) sorted[k] = L.ids[k];
            qsort(sorted, (size_t)L.count, sizeof(int), cmp_int);
            CK(memcmp(sorted, ref, sizeof(int) * (size_t)nref) == 0);
            /* 每层数量与参考一致 */
            for (int g = 0; g < L.group_count; ++g)
                CK(L.groups[g].count == ref_visible(rb, n, L.groups[g].layer, lv));
            check_boundaries(&L, &st);
            cull_layered_free(&L);
            free(rb); free(ref); free(sorted);
        }
    }

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
