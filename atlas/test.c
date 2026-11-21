#include "atlas.h"
#include <stdio.h>
static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

int main(void) {
    AtlasRect r[4] = {{16,16},{32,8},{8,8},{64,4}};
    AtlasPos p[4];
    AtlasStats s;
    CK(atlas_pack(r, 4, 64, p, &s) == 0);
    CK(p[0].x == 0 && p[0].y == 0);
    CK(s.shelves >= 2);
    CK(s.used_px == 16*16 + 32*8 + 8*8 + 64*4);
    AtlasRect bad[1] = {{128, 4}};
    CK(atlas_pack(bad, 1, 64, p, &s) == -2);
    CK(atlas_pack(r, 0, 64, p, &s) == -1);

    /* 关掉排序：必须与旧路径输出完全一致 */
    AtlasPos p2[4];
    AtlasStats s2;
    int moved = -1, err = -1;
    CK(atlas_pack2(r, 4, 64, ATLAS_SORT_OFF, p2, &s2, &moved, &err) == 0);
    CK(moved == 0);
    CK(s2.shelves == s.shelves && s2.used_px == s.used_px && s2.width == s.width);
    for (int i = 0; i < 4; ++i)
        CK(p2[i].x == p[i].x && p2[i].y == p[i].y && p2[i].placed == p[i].placed);

    /* 打开排序：两段式，先按面积从大到小再逐块存放。
     * 用例：高块与矮块交替，原序摆放高块把条带撑高造成浪费。 */
    AtlasRect m[5] = {{30,10},{30,50},{30,10},{30,50},{30,10}};
    AtlasPos mo[5], ms[5];
    AtlasStats so, ss;
    CK(atlas_pack2(m, 5, 100, ATLAS_SORT_OFF, mo, &so, NULL, NULL) == 0);
    CK(atlas_pack2(m, 5, 100, ATLAS_SORT_AREA_DESC, ms, &ss, &moved, &err) == 0);
    CK(moved > 0);                 /* 次序确实被重排 */
    CK(ss.used_px == so.used_px);  /* 像素总数不变 */
    int ho = 0, hs = 0;
    for (int i = 0; i < 5; ++i) {
        if (mo[i].y + m[i].h > ho) ho = mo[i].y + m[i].h;
        if (ms[i].y + m[i].h > hs) hs = ms[i].y + m[i].h;
    }
    CK(hs < ho);                   /* 总高度下降：60 < 100 */
    /* 面积最大的 30x50 应最先存放，落在原点 */
    CK(ms[1].x == 0 && ms[1].y == 0);

    /* 参数为负：报错并指出是哪一项 */
    AtlasRect neg[3] = {{8,8},{-4,16},{8,8}};
    CK(atlas_pack2(neg, 3, 64, ATLAS_SORT_AREA_DESC, p2, &s2, &moved, &err) == -2);
    CK(err == 1);
    AtlasRect neg2[3] = {{8,8},{8,8},{16,-1}};
    CK(atlas_pack2(neg2, 3, 64, ATLAS_SORT_OFF, p2, &s2, &moved, &err) == -2);
    CK(err == 2);

    /* 未知 sort 取值 */
    CK(atlas_pack2(r, 4, 64, 7, p2, &s2, &moved, &err) == -3);

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
