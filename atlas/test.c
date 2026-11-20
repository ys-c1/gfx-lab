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

    /* 不切页时布局照旧：单页 book 与 atlas_pack 完全一致 */
    AtlasBook b;
    int err = -1;
    CK(atlas_book_pack(r, 4, 64, 1024, &b, &err) == 0);
    CK(b.page_count == 1);
    CK(b.page_width == 64 && b.page_height == 1024);
    int same = 1;
    for (int i = 0; i < 4; ++i)
        if (b.cells[i].page != 0 || b.cells[i].x != p[i].x || b.cells[i].y != p[i].y) same = 0;
    CK(same);
    CK(b.pages[0].used_px == s.used_px);
    CK(b.pages[0].occupancy > 0.0 && b.pages[0].occupancy < 1.0);
    atlas_book_free(&b);

    /* 两阶段切页：64x64 页，第三个矩形溢出到第 2 页 */
    AtlasRect r2[3] = {{64,32},{64,32},{64,64}};
    CK(atlas_book_pack(r2, 3, 64, 64, &b, &err) == 0);
    CK(b.page_count == 2);
    CK(b.cells[0].page == 0 && b.cells[0].x == 0 && b.cells[0].y == 0);
    CK(b.cells[1].page == 0 && b.cells[1].x == 0 && b.cells[1].y == 32);
    CK(b.cells[2].page == 1 && b.cells[2].x == 0 && b.cells[2].y == 0);
    CK(b.pages[0].occupancy > 0.99 && b.pages[1].occupancy > 0.99);
    /* 页内偏移不越界 */
    int in_bounds = 1;
    for (int i = 0; i < 3; ++i)
        if (b.cells[i].x + r2[i].w > 64 || b.cells[i].y + r2[i].h > 64) in_bounds = 0;
    CK(in_bounds);

    /* 页表可导出 */
    FILE *f = tmpfile();
    CK(f != NULL);
    CK(atlas_book_export(&b, r2, f) == 0);
    CK(ftell(f) > 0);
    fclose(f);
    atlas_book_free(&b);

    /* 块大于页：报错并指出序号 */
    AtlasRect r3[3] = {{8,8},{128,4},{8,8}};
    CK(atlas_book_pack(r3, 3, 64, 64, &b, &err) == -2 && err == 1);
    AtlasRect r4[2] = {{8,8},{4,128}};
    CK(atlas_book_pack(r4, 2, 64, 64, &b, &err) == -2 && err == 1);
    /* 页数为零：报错 */
    CK(atlas_book_pack(r, 0, 64, 64, &b, &err) == -3);
    CK(atlas_book_pack(r, -2, 64, 64, &b, &err) == -3);
    CK(atlas_book_pack(NULL, 4, 64, 64, &b, &err) == -1);
    CK(atlas_book_pack(r, 4, 0, 64, &b, &err) == -1);

    /* 贪心切页对照 */
    CK(atlas_book_pack_greedy(r2, 3, 64, 64, &b, &err) == 0);
    CK(b.page_count >= 2);
    CK(atlas_book_export(&b, r2, tmpfile()) == 0);
    atlas_book_free(&b);
    CK(atlas_book_pack_greedy(r3, 3, 64, 64, &b, &err) == -2 && err == 1);
    CK(atlas_book_pack_greedy(r, 0, 64, 64, &b, &err) == -3);

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
