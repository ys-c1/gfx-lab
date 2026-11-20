#ifndef ATLAS_H
#define ATLAS_H
#include <stdio.h>

typedef struct { int w, h; } AtlasRect;
typedef struct { int x, y; int placed; } AtlasPos;
typedef struct { int width; int used_px; int shelves; } AtlasStats;
int atlas_pack(const AtlasRect *rects, int n, int width, AtlasPos *out, AtlasStats *stats);

/* ---- 多页图集 ----
 * 两阶段打包：先按页上限 (page_w x page_h) 切页，再逐页回填页内偏移。
 * 单页即可放下时，布局与 atlas_pack 完全一致。
 * 加载顺序：页号升序（0..page_count-1）；页内按矩形原始序号升序。
 */
typedef struct { int page; int x, y; } AtlasCell;      /* 页号 + 页内偏移 */
typedef struct {
    int width, height;   /* 页尺寸 */
    int used_px;         /* 已用像素 */
    int shelves;         /* 货架数 */
    double occupancy;    /* 占用率 used_px / (width*height) */
} AtlasPageInfo;
typedef struct {
    AtlasCell *cells;        /* n 条，按矩形原始序号 */
    AtlasPageInfo *pages;    /* page_count 条 */
    int page_count;
    int n;
    int page_width, page_height;
} AtlasBook;

/* 返回值：0 成功；-1 参数非法；-2 矩形大于页（*err_item 为矩形序号）；
 *        -3 页数为零（n<=0）；-4 内存分配失败。
 * 失败时 book 被清零，atlas_book_free 安全。 */
int atlas_book_pack(const AtlasRect *rects, int n, int page_w, int page_h,
                    AtlasBook *book, int *err_item);
/* 贪心切页（对照用）：按高降序首次适配，返回值约定同上。 */
int atlas_book_pack_greedy(const AtlasRect *rects, int n, int page_w, int page_h,
                           AtlasBook *book, int *err_item);
void atlas_book_free(AtlasBook *book);
/* 导出页表（含每页占用率与每个矩形的页内偏移、加载顺序说明）。 */
int atlas_book_export(const AtlasBook *book, const AtlasRect *rects, FILE *out);
#endif
