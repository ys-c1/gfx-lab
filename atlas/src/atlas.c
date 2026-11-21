#include "atlas.h"
#include <stdlib.h>

int atlas_pack(const AtlasRect *rects, int n, int width, AtlasPos *out, AtlasStats *stats) {
    if (!rects || !out || !stats || n <= 0 || width <= 0) return -1;
    int cur_x = 0, cur_y = 0, shelf_h = 0, used = 0, shelves = 1;
    for (int i = 0; i < n; ++i) {
        if (rects[i].w <= 0 || rects[i].h <= 0 || rects[i].w > width) return -2;
        if (cur_x + rects[i].w > width) {
            cur_y += shelf_h; cur_x = 0; shelf_h = 0; ++shelves;
        }
        out[i].x = cur_x; out[i].y = cur_y; out[i].placed = 1;
        cur_x += rects[i].w;
        if (rects[i].h > shelf_h) shelf_h = rects[i].h;
        used += rects[i].w * rects[i].h;
    }
    stats->width = width;
    stats->used_px = used;
    stats->shelves = shelves;
    return 0;
}

/* 按给定次序逐块货架存放；位置写回原始下标。 */
static void place_order(const AtlasRect *rects, const int *ord, int n, int width,
                        AtlasPos *out, AtlasStats *stats) {
    int cur_x = 0, cur_y = 0, shelf_h = 0, used = 0, shelves = 1;
    for (int k = 0; k < n; ++k) {
        int i = ord[k];
        if (cur_x + rects[i].w > width) {
            cur_y += shelf_h; cur_x = 0; shelf_h = 0; ++shelves;
        }
        out[i].x = cur_x; out[i].y = cur_y; out[i].placed = 1;
        cur_x += rects[i].w;
        if (rects[i].h > shelf_h) shelf_h = rects[i].h;
        used += rects[i].w * rects[i].h;
    }
    stats->width = width;
    stats->used_px = used;
    stats->shelves = shelves;
}

static const AtlasRect *g_rects;
static int cmp_area_desc(const void *a, const void *b) {
    int ia = *(const int *)a, ib = *(const int *)b;
    long aa = (long)g_rects[ia].w * g_rects[ia].h;
    long ab = (long)g_rects[ib].w * g_rects[ib].h;
    if (aa != ab) return aa < ab ? 1 : -1;
    return ia - ib; /* 面积相同按下标升序，保证稳定可复现 */
}

int atlas_pack2(const AtlasRect *rects, int n, int width, int sort,
                AtlasPos *out, AtlasStats *stats, int *moved, int *err_index) {
    if (moved) *moved = 0;
    if (err_index) *err_index = -1;
    if (!rects || !out || !stats || n <= 0 || width <= 0) return -1;
    if (sort != ATLAS_SORT_OFF && sort != ATLAS_SORT_AREA_DESC) return -3;
    for (int i = 0; i < n; ++i) {
        if (rects[i].w <= 0 || rects[i].h <= 0 || rects[i].w > width) {
            if (err_index) *err_index = i;
            return -2;
        }
    }

    int *ord = malloc((size_t)n * sizeof(int));
    if (!ord) return -4;
    for (int i = 0; i < n; ++i) ord[i] = i;

    if (sort == ATLAS_SORT_AREA_DESC) {
        g_rects = rects;
        qsort(ord, (size_t)n, sizeof(int), cmp_area_desc);
        int m = 0;
        for (int k = 0; k < n; ++k) if (ord[k] != k) ++m;
        if (moved) *moved = m;
    }

    place_order(rects, ord, n, width, out, stats);
    free(ord);
    return 0;
}
