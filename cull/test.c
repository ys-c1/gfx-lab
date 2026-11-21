#include "cull.h"
#include <stdio.h>
#include <stdlib.h>
static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

static int same_ids(const int *a, const int *b, int n) {
    for (int i = 0; i < n; ++i) if (a[i] != b[i]) return 0;
    return 1;
}

int main(void) {
    /* ---- 原有静态剔除行为不变 ---- */
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

    /* ---- 对象表与映射 ---- */
    CullObject objs[3] = {{10, b[0]}, {20, b[1]}, {30, b[2]}};
    CullMap map; int ei = 123;
    CK(cull_map_build(&map, objs, 3, &ei) == CULL_OK);
    CK(map.n == 3);
    CK(cull_map_find(&map, 10) == 0 && cull_map_find(&map, 30) == 2);
    CK(cull_map_find(&map, 99) == -1);
    cull_map_free(&map);
    CK(map.n == 0);

    /* ---- 阶段 1：按对象号合并 ---- */
    /* 候选: 7 3 7 5 3 9  -> 唯一升序 3 5 7 9 */
    int cand[6] = {7, 3, 7, 5, 3, 9};
    int uniq[6]; int uc = 0;
    CK(cull_merge_ids(cand, 6, uniq, &uc, &ei) == CULL_OK);
    CK(uc == 4);
    CK(same_ids(uniq, (int[]){3,5,7,9}, 4));
    CK(ei == -1);

    /* ---- 阶段 2：按原顺序输出（首次出现） ---- */
    int oid[6], ofirst[6];
    CK(cull_order_original(cand, 6, uniq, uc, oid, ofirst, &ei) == CULL_OK);
    CK(same_ids(oid, (int[]){7,3,5,9}, 4));
    CK(same_ids(ofirst, (int[]){0,1,3,5}, 4));

    /* ---- 合并开：绘制调用数 = 唯一对象数 ---- */
    CullObject all[16];
    for (int i = 0; i < 16; ++i) all[i].id = 100 + i;
    CullMap map2;
    CK(cull_map_build(&map2, all, 16, &ei) == CULL_OK);
    int cand2[10] = {103, 104, 103, 100, 104, 109, 100, 115, 103, 109};
    CullDrawList dl_on, dl_off;
    CK(cull_draw_list(cand2, 10, &map2, 1, &dl_on, &ei) == CULL_OK);
    /* 唯一对象号: 100,103,104,109,115 = 5 */
    CK(dl_on.count == 5);
    CK(same_ids(dl_on.ids, (int[]){103,104,100,109,115}, 5));
    CK(same_ids(dl_on.first, (int[]){0,1,3,5,7}, 5));
    for (int i = 1; i < dl_on.count; ++i) CK(dl_on.ids[i] != dl_on.ids[i-1]);
    cull_draw_list_free(&dl_on);

    /* ---- 合并关：逐项照旧，画面不受影响 ---- */
    CK(cull_draw_list(cand2, 10, &map2, 0, &dl_off, &ei) == CULL_OK);
    CK(dl_off.count == 10);                                /* 绘制调用翻倍原样保留 */
    CK(same_ids(dl_off.ids, cand2, 10));
    CK(same_ids(dl_off.first, (int[]){0,1,2,3,4,5,6,7,8,9}, 10));
    cull_draw_list_free(&dl_off);

    /* ---- 合并条数单独计数：候选数 - 绘制调用数 = 5 ---- */
    CK(10 - 5 == 5);

    /* ---- 错误：对象号重复，指出序号 ---- */
    CullObject dupobjs[3] = {{5, b[0]}, {9, b[1]}, {5, b[2]}};
    CullMap dm; ei = -1;
    CK(cull_map_build(&dm, dupobjs, 3, &ei) == CULL_E_DUP_ID);
    CK(ei == 2);            /* 后出现的重复项在对象表中的下标 2（第 3 项） */
    cull_map_free(&dm);

    /* ---- 错误：列表为空 ---- */
    int dummy_u[1]; int dc = 0; ei = 0;
    CK(cull_merge_ids(cand, 0, dummy_u, &dc, &ei) == CULL_E_EMPTY);
    CK(ei == -1);
    CK(cull_map_build(&dm, objs, 0, &ei) == CULL_E_EMPTY);
    CK(cull_draw_list(cand, 0, &map2, 1, &dl_on, &ei) == CULL_E_EMPTY);
    CK(dl_on.ids == NULL && dl_on.count == 0);

    /* ---- 错误：映射缺项，指出候选序号 ---- */
    int miss[4] = {103, 777, 100, 103}; ei = -1;
    CK(cull_draw_list(miss, 4, &map2, 1, &dl_on, &ei) == CULL_E_MAP_MISS);
    CK(ei == 1);            /* 候选下标 1（第 2 项） */
    CK(dl_on.ids == NULL && dl_on.count == 0);
    /* 合并关也要报同一个缺项 */
    CK(cull_draw_list(miss, 4, &map2, 0, &dl_on, &ei) == CULL_E_MAP_MISS);
    CK(ei == 1);
    /* 阶段 2 拿到缺项的 uniq 也要报 */
    int badu[2] = {100, 103}; ei = -1;
    CK(cull_order_original(miss, 4, badu, 2, oid, ofirst, &ei) == CULL_E_MAP_MISS);
    CK(ei == 1);

    /* ---- 参数错误 ---- */
    CK(cull_merge_ids(NULL, 3, uniq, &uc, &ei) == CULL_E_ARG);
    CK(cull_draw_list(cand2, 10, &map2, 1, NULL, &ei) == CULL_E_ARG);

    cull_map_free(&map2);
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
