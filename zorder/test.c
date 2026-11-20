#include "zorder.h"
#include <stdio.h>
#include <stdlib.h>
static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

static long g_areas[1024];

int main(void) {
    /* 1) Morton 编解码往返 */
    for (unsigned y = 0; y < 16; ++y)
        for (unsigned x = 0; x < 16; ++x) {
            unsigned c = zo_morton_encode(x, y), dx, dy;
            zo_morton_decode(c, &dx, &dy);
            CK(dx == x && dy == y);
        }
    CK(zo_morton_encode(0, 0) == 0u);
    CK(zo_morton_encode(1, 0) == 1u);
    CK(zo_morton_encode(0, 1) == 2u);
    CK(zo_morton_encode(1, 1) == 3u);
    CK(zo_morton_encode(2, 0) == 4u);

    /* 2) 4x4 阶段一重排 + 跳转次数 */
    ZoLayout lay;
    CK(zo_reorder(4, 4, NULL, ZO_MORTON, &lay) == 0);
    int expect4[16] = {0,1,4,5,2,3,6,7,8,9,12,13,10,11,14,15};
    CK(lay.n == 16 && lay.remap == NULL);   /* 阶段一尚未回填 */
    for (int i = 0; i < 16; ++i) CK(lay.order[i] == expect4[i]);
    CK(lay.jumps == 6);                 /* 2^k 方格莫顿序跳转次数 = n/2 - 2 */

    /* 3) 阶段二回填索引，remap 为 order 的逆映射 */
    CK(zo_backfill(&lay) == 0);
    for (int pos = 0; pos < 16; ++pos) CK(lay.remap[lay.order[pos]] == pos);
    zo_free(&lay);

    /* 4) 便捷封装一次完成 */
    CK(zo_layout(8, 8, NULL, ZO_MORTON, &lay) == 0);
    CK(lay.n == 64 && lay.remap != NULL);
    for (int pos = 0; pos < 64; ++pos) CK(lay.remap[lay.order[pos]] == pos);
    CK(lay.jumps == 30);                 /* 8x8 全图 Z 序跳转次数 */
    zo_free(&lay);

    /* 5) ZO_NONE 不打乱顺序：布局不变、跳转为 0 */
    CK(zo_layout(8, 8, NULL, ZO_NONE, &lay) == 0);
    for (int i = 0; i < 64; ++i) CK(lay.order[i] == i && lay.remap[i] == i);
    CK(lay.jumps == 0);
    zo_free(&lay);

    /* 6) 面积排序：从大到小，相同面积保持原序号 */
    for (int i = 0; i < 16; ++i) g_areas[i] = (long)(16 - i);  /* 单调递减 */
    CK(zo_layout(4, 4, g_areas, ZO_AREA, &lay) == 0);
    for (int pos = 0; pos < 16; ++pos) CK(lay.order[pos] == pos);
    zo_free(&lay);
    for (int i = 0; i < 16; ++i) g_areas[i] = 100;             /* 全部相同 -> 原序 */
    CK(zo_layout(4, 4, g_areas, ZO_AREA, &lay) == 0);
    for (int i = 0; i < 16; ++i) CK(lay.order[i] == i);
    zo_free(&lay);
    CK(zo_layout(4, 4, NULL, ZO_AREA, &lay) == -1);            /* 面积排序缺面积表 */

    /* 7) 尺寸非 2 的幂报错（序号 0） */
    CK(zo_layout(6, 4, NULL, ZO_MORTON, &lay) == -2 && lay.err_seq == 0);
    CK(zo_layout(5, 5, NULL, ZO_MORTON, &lay) == -2 && lay.err_seq == 0);
    CK(zo_layout(0, 4, NULL, ZO_MORTON, &lay) == -1);

    /* 8) 外部码位：越界与重复，指出首个出错序号 */
    unsigned codes[16];
    for (int i = 0; i < 16; ++i) codes[i] = (unsigned)i;
    codes[9] = 16;                       /* 越界，序号 9 */
    CK(zo_reorder_codes(4, 4, codes, &lay) == -4 && lay.err_seq == 9);
    for (int i = 0; i < 16; ++i) codes[i] = (unsigned)i;
    codes[5] = 2;                        /* 与序号 2 重复，序号 5 先被发现 */
    CK(zo_reorder_codes(4, 4, codes, &lay) == -3 && lay.err_seq == 5);
    for (int i = 0; i < 16; ++i) codes[i] = (unsigned)i;
    CK(zo_reorder_codes(4, 4, codes, &lay) == 0);  /* 合法码位 -> 顺序布局 */
    for (int i = 0; i < 16; ++i) CK(lay.order[i] == i);
    CK(lay.jumps == 0);
    zo_free(&lay);

    /* 9) 合法码位（手工 2x2 莫顿序）经外部入口得到 Z 序 */
    unsigned c2[4] = {0, 1, 2, 3};
    CK(zo_reorder_codes(2, 2, c2, &lay) == 0 && lay.jumps == 0);
    zo_free(&lay);

    /* 10) 窗口统计：Morton 的跨度比/占用率不高于 NONE（局部性不差于原序），参数非法报错 */
    double jm, sm, om, jn, sn, on;
    CK(zo_layout(16, 16, NULL, ZO_MORTON, &lay) == 0);
    CK(zo_window_stats(&lay, 8, 8, 4, 4, &jm, &sm, &om) == 0);
    zo_free(&lay);
    CK(zo_layout(16, 16, NULL, ZO_NONE, &lay) == 0);
    CK(zo_window_stats(&lay, 8, 8, 4, 4, &jn, &sn, &on) == 0);
    CK(sm <= sn + 1e-12);
    CK(om <= on + 1e-12);
    CK(zo_window_stats(&lay, 0, 8, 4, 4, &jn, &sn, &on) == -1);
    CK(zo_window_stats(&lay, 8, 8, 4, 0, &jn, &sn, &on) == -1);
    CK(zo_window_stats(&lay, 32, 8, 4, 4, &jn, &sn, &on) == -1);
    zo_free(&lay);

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
