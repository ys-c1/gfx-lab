#include "cull.h"
#include <stdio.h>
#include <stdlib.h>

static void show(const char *tag, const CullDrawList *d) {
    printf("%s: 绘制调用 %d 条\n", tag, d->count);
    printf("  ids:");
    for (int i = 0; i < d->count; ++i) printf(" %d", d->ids[i]);
    printf("\n  first:");
    for (int i = 0; i < d->count; ++i) printf(" %d", d->first[i]);
    printf("\n");
}

int main(void) {
    enum { N = 16 };
    CullObject objs[N];
    for (int i = 0; i < N; ++i) {
        objs[i].id = 100 + i;
        objs[i].box.x = (i % 4) * 30; objs[i].box.y = (i / 4) * 40;
        objs[i].box.w = 25; objs[i].box.h = 35; objs[i].box.layer = i % 3;
    }
    CullMap map; int ei;
    if (cull_map_build(&map, objs, N, &ei) != CULL_OK) return 1;

    /* 候选里同一对象出现多次：绘制调用会白白翻倍。 */
    int cand[12] = {103, 104, 103, 100, 104, 109, 100, 115, 103, 109, 100, 104};
    printf("候选 %d 条，唯一对象 %d 个\n", 12, 6);

    CullDrawList off, on;
    if (cull_draw_list(cand, 12, &map, 0, &off, &ei) != CULL_OK) return 1;
    if (cull_draw_list(cand, 12, &map, 1, &on, &ei) != CULL_OK) return 1;
    show("合并关（原样，画面照旧）", &off);
    show("合并开（按对象号合并，再按原顺序输出）", &on);
    printf("合并条数（单独计）: %d -> %d，省下 %d 条\n",
           off.count, on.count, off.count - on.count);
    cull_draw_list_free(&off);
    cull_draw_list_free(&on);

    /* 错误：对象号重复 / 列表为空 / 映射缺项，均指出序号（1 起）。 */
    CullObject dupobjs[3] = {{5, objs[0].box}, {9, objs[1].box}, {5, objs[2].box}};
    CullMap dm;
    if (cull_map_build(&dm, dupobjs, 3, &ei) == CULL_E_DUP_ID)
        printf("报错：对象号重复，第 %d 个对象\n", ei + 1);
    cull_map_free(&dm);

    int one = 103;
    if (cull_merge_ids(&one, 0, &one, &one, &ei) == CULL_E_EMPTY)
        printf("报错：候选列表为空\n");

    int miss[4] = {103, 777, 100, 103};
    CullDrawList bad;
    if (cull_draw_list(miss, 4, &map, 1, &bad, &ei) == CULL_E_MAP_MISS)
        printf("报错：映射缺项，第 %d 个候选（对象号 %d）\n", ei + 1, miss[ei]);

    cull_map_free(&map);
    return 0;
}
