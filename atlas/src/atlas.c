#include "atlas.h"

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
