#include "grid.h"
#include <stdio.h>
#include <stdlib.h>
static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

/* 确定性 LCG，保证测试可复现 */
static unsigned long rng_state = 0x12345678UL;
static double rnd(void) {
    rng_state = rng_state * 6364136223846793005UL + 1442695040888963407UL;
    return (double)(rng_state >> 33) / (double)(1UL << 31);
}

static void random_scene(GdBox *b, int n, double world, double size) {
    for (int i = 0; i < n; ++i) {
        b[i].w = size * (0.5 + rnd());
        b[i].h = size * (0.5 + rnd());
        b[i].x = rnd() * (world - b[i].w);
        b[i].y = rnd() * (world - b[i].h);
    }
}

int main(void) {
    /* 1. 固定小场景：手算重叠对 */
    GdBox fixed[5] = {
        {0, 0, 10, 10},      /* 0 与 1 重叠 */
        {5, 5, 10, 10},      /* 1 与 0、2、3 重叠 */
        {12, 12, 4, 4},      /* 2 与 1 重叠；与 3 的 x 区间 [12,16]/[2,8] 不交 */
        {2, 14, 6, 6},       /* 3 与 1 重叠；与 0 的 y 区间 [14,20]/[0,10] 不交 */
        {100, 100, 3, 3},    /* 4 孤立 */
    };
    GdConfig cfg = {0};
    cfg.minx = 0; cfg.miny = 0; cfg.maxx = 110; cfg.maxy = 110;
    CK(grid_pick_cell(&cfg, fixed, 5) == 0);
    CK(cfg.cell_w > 0.0 && cfg.density > 0.0);

    GdGrid *g = NULL;
    CK(grid_build(&g, fixed, 5, cfg) == 0);
    GdPairList gp = {0};
    GdStats st = {0};
    CK(grid_pairs(g, &gp, &st) == 0);

    GdPairList bp = {0};
    long long bcand = 0;
    CK(grid_brute_pairs(fixed, 5, &bp, &bcand) == 0);
    CK(bcand == 5 * 4 / 2);
    CK(grid_pairs_equal(&gp, &bp) == 1);
    /* 手算（边相接按重叠计）：(0,1)、(1,2)、(1,3)，共 3 对；
       0-3 的 y 区间不相交，2-3 的 x 区间不相交，4 号孤立 */
    CK(bp.count == 3);
    static const GdPair expect[3] = {{0,1},{1,2},{1,3}};
    for (int k = 0; k < 3; ++k) {
        int found = 0;
        for (int q = 0; q < bp.count; ++q)
            if (bp.pairs[q].a == expect[k].a && bp.pairs[q].b == expect[k].b) found = 1;
        CK(found);
    }

    GdPairList vp = {0};
    long long vtests = 0;
    CK(grid_bvh_pairs(fixed, 5, &vp, &vtests) == 0);
    CK(grid_pairs_equal(&vp, &bp) == 1);
    CK(vtests > 0);
    grid_pairs_free(&gp); grid_pairs_free(&bp); grid_pairs_free(&vp);
    grid_free(g);

    /* 1b. 边/角相接按重叠计（与 cull 模块的 overlap 语义一致） */
    GdBox touch[2] = {{0, 0, 10, 10}, {10, 0, 10, 10}};
    GdConfig cfg_t = {0};
    cfg_t.minx = 0; cfg_t.miny = 0; cfg_t.maxx = 20; cfg_t.maxy = 10; cfg_t.cell_w = 5;
    GdGrid *gt = NULL;
    CK(grid_build(&gt, touch, 2, cfg_t) == 0);
    GdPairList tp = {0};
    CK(grid_pairs(gt, &tp, NULL) == 0);
    CK(tp.count == 1 && tp.pairs[0].a == 0 && tp.pairs[0].b == 1);
    grid_pairs_free(&tp);
    grid_free(gt);

    /* 2. 错误参数：格宽为零、密度为负，要报错并指出字段 */
    GdConfig bad_w = cfg; bad_w.cell_w = 0.0;
    CK(grid_build(&g, fixed, 5, bad_w) == -2);
    CK(grid_error_field(-2) != NULL);
    GdConfig bad_d = cfg; bad_d.density = -1.0;
    CK(grid_build(&g, fixed, 5, bad_d) == -3);
    CK(grid_error_field(-3) != NULL);
    CK(grid_pick_cell(&bad_d, fixed, 5) == -3);
    GdConfig bad_range = cfg; bad_range.maxx = bad_range.minx;
    CK(grid_build(&g, fixed, 5, bad_range) == -4);
    CK(grid_error_field(-4) != NULL);

    /* 3. 随机场景：网格结果必须与暴力完全一致（不用网格结果一样） */
    for (int trial = 0; trial < 60; ++trial) {
        int n = 1 + (int)(rnd() * 200);
        GdBox *b = (GdBox *)malloc((size_t)n * sizeof(GdBox));
        double world = 100.0, size = 4.0 + rnd() * 12.0;
        random_scene(b, n, world, size);
        GdConfig c = {0};
        c.minx = 0; c.miny = 0; c.maxx = world; c.maxy = world;
        CK(grid_pick_cell(&c, b, n) == 0);
        GdGrid *gg = NULL;
        CK(grid_build(&gg, b, n, c) == 0);
        GdPairList a = {0}, ref = {0}, bvh = {0};
        CK(grid_pairs(gg, &a, NULL) == 0);
        CK(grid_brute_pairs(b, n, &ref, NULL) == 0);
        CK(grid_bvh_pairs(b, n, &bvh, NULL) == 0);
        CK(grid_pairs_equal(&a, &ref) == 1);
        CK(grid_pairs_equal(&bvh, &ref) == 1);
        /* 连续第二次查询：戳记残留不得导致漏对 */
        GdPairList a2 = {0};
        CK(grid_pairs(gg, &a2, NULL) == 0);
        CK(grid_pairs_equal(&a2, &ref) == 1);
        grid_pairs_free(&a); grid_pairs_free(&a2); grid_pairs_free(&ref); grid_pairs_free(&bvh);
        grid_free(gg);
        free(b);
    }

    /* 4. 增量增删：只更新受影响格，结果仍与暴力一致，并统计次数 */
    {
        enum { N = 300 };
        GdBox *b = (GdBox *)malloc(N * sizeof(GdBox));
        random_scene(b, N, 200.0, 8.0);
        GdConfig c = {0};
        c.minx = 0; c.miny = 0; c.maxx = 200.0; c.maxy = 200.0;
        CK(grid_pick_cell(&c, b, N) == 0);
        GdGrid *gg = NULL;
        CK(grid_build(&gg, b, N, c) == 0);
        GdStats before = {0};
        CK(grid_stats(gg, &before) == 0);
        CK(before.rebuilds == 1 && before.updates == 0);

        /* 移动前先查一次，制造跨查询的戳记残留（回归：重复查询漏对） */
        GdPairList warm = {0};
        CK(grid_pairs(gg, &warm, NULL) == 0);
        grid_pairs_free(&warm);

        /* 删掉 20 个，再插回 20 个到新位置 */
        for (int k = 0; k < 20; ++k) CK(grid_remove(gg, k * 7 + 1) == 0);
        CK(grid_remove(gg, 1) == -5);      /* 已删除 */
        CK(grid_remove(gg, N + 10) == -5); /* 越界 */
        for (int k = 0; k < 20; ++k) {
            int id = k * 7 + 1;
            b[id].x = rnd() * 190.0;
            b[id].y = rnd() * 190.0;
            CK(grid_insert(gg, id, b[id]) == 0);
        }
        CK(grid_insert(gg, 0, b[0]) == -5); /* 已存在 */
        GdStats after = {0};
        CK(grid_stats(gg, &after) == 0);
        CK(after.updates == 40 && after.rebuilds == 1);

        GdPairList a = {0}, ref = {0};
        CK(grid_pairs(gg, &a, NULL) == 0);
        CK(grid_brute_pairs(b, N, &ref, NULL) == 0);
        CK(grid_pairs_equal(&a, &ref) == 1);
        CK(grid_table_bytes(gg) > 0);
        grid_pairs_free(&a); grid_pairs_free(&ref);
        grid_free(gg);
        free(b);
    }

    /* 5. 边界：空场景与单对象 */
    {
        GdConfig c = {0};
        c.minx = 0; c.miny = 0; c.maxx = 10; c.maxy = 10; c.cell_w = 2;
        GdGrid *gg = NULL;
        CK(grid_build(&gg, NULL, 0, c) == 0);
        GdPairList a = {0};
        CK(grid_pairs(gg, &a, NULL) == 0);
        CK(a.count == 0);
        GdBox one = {1, 1, 1, 1};
        CK(grid_insert(gg, 0, one) == 0);
        grid_pairs_free(&a);
        CK(grid_pairs(gg, &a, NULL) == 0);
        CK(a.count == 0);
        grid_pairs_free(&a);
        grid_free(gg);
    }

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
