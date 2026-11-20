#include "atlas.h"
#include <stdlib.h>
#include <string.h>

/* ---- legacy shelf packer ------------------------------------------- */

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
    stats->height = cur_y + shelf_h;
    stats->used_px = used;
    stats->saved_px = 0;
    stats->shelves = shelves;
    return 0;
}

/* ---- stage 1: alpha bounding box ----------------------------------- */

static void set_bad(int *bad_index, int i) { if (bad_index) *bad_index = i; }

int atlas_trim(const AtlasTile *tiles, int n, AtlasTrim *out, int *bad_index) {
    set_bad(bad_index, -1);
    if (!tiles || !out || n <= 0) return ATLAS_EINVAL;
    for (int i = 0; i < n; ++i) {
        int w = tiles[i].w, h = tiles[i].h, m = tiles[i].margin;
        if (w <= 0 || h <= 0) { set_bad(bad_index, i); return ATLAS_EDIM; }
        if (m < 0) { set_bad(bad_index, i); return ATLAS_EMARGIN; }
        int x0 = 0, y0 = 0, x1 = w - 1, y1 = h - 1;
        if (tiles[i].rgba) {
            const unsigned char *px = tiles[i].rgba;
            x0 = w; y0 = h; x1 = -1; y1 = -1;
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    if (px[((size_t)y * (size_t)w + (size_t)x) * 4u + 3u] == 0) continue;
                    if (x < x0) x0 = x;
                    if (x > x1) x1 = x;
                    if (y < y0) y0 = y;
                    if (y > y1) y1 = y;
                }
            }
            if (x1 < 0) { set_bad(bad_index, i); return ATLAS_ETRANSPARENT; }
        }
        long long lx = (long long)x0 - m, ly = (long long)y0 - m;
        long long hx = (long long)x1 + m, hy = (long long)y1 + m;
        if (lx < 0 || ly < 0 || hx >= w || hy >= h) {
            set_bad(bad_index, i);
            return ATLAS_ETRIM_BOUNDS;
        }
        out[i].tile = i;
        out[i].src_x = (int)lx;
        out[i].src_y = (int)ly;
        out[i].w = (int)(hx - lx + 1);
        out[i].h = (int)(hy - ly + 1);
    }
    return ATLAS_OK;
}

/* ---- stage 2: pack -------------------------------------------------- */

typedef struct { int y, h, x; } Shelf;

static int taller_wider(const AtlasTrim *t, int a, int b) {
    if (t[a].h != t[b].h) return t[b].h - t[a].h;   /* taller first */
    if (t[a].w != t[b].w) return t[b].w - t[a].w;   /* wider first */
    return a - b;                                   /* stable */
}

int atlas_place(const AtlasTrim *trims, int n, int width, AtlasMode mode,
                AtlasSlot *out, AtlasStats *stats, int *bad_index) {
    set_bad(bad_index, -1);
    if (!trims || !out || !stats || n <= 0 || width <= 0) return ATLAS_EINVAL;
    if (mode != ATLAS_SHELF && mode != ATLAS_GREEDY) return ATLAS_EINVAL;
    /* slots are only ever observed as placed after a successful return */
    for (int i = 0; i < n; ++i) memset(&out[i], 0, sizeof(out[i]));

    for (int i = 0; i < n; ++i) {
        if (trims[i].w <= 0 || trims[i].h <= 0) { set_bad(bad_index, i); return ATLAS_EDIM; }
        if (trims[i].w > width) { set_bad(bad_index, i); return ATLAS_EFIT; }
    }

    int used = 0, shelves = 0, height = 0;
    for (int i = 0; i < n; ++i) {
        out[i].tile = trims[i].tile;
        out[i].w = trims[i].w;
        out[i].h = trims[i].h;
        out[i].src_x = trims[i].src_x;
        out[i].src_y = trims[i].src_y;
        out[i].x = 0; out[i].y = 0; out[i].placed = 0;
        used += trims[i].w * trims[i].h;
    }

    if (mode == ATLAS_SHELF) {
        int cur_x = 0, cur_y = 0, shelf_h = 0;
        shelves = 1;
        for (int i = 0; i < n; ++i) {
            if (cur_x + trims[i].w > width) {
                cur_y += shelf_h; cur_x = 0; shelf_h = 0; ++shelves;
            }
            out[i].x = cur_x; out[i].y = cur_y; out[i].placed = 1;
            cur_x += trims[i].w;
            if (trims[i].h > shelf_h) shelf_h = trims[i].h;
        }
        height = cur_y + shelf_h;
    } else {
        int *order = (int *)malloc(sizeof(int) * (size_t)n);
        Shelf *sh = (Shelf *)malloc(sizeof(Shelf) * (size_t)n);
        if (!order || !sh) { free(order); free(sh); return ATLAS_EINVAL; }
        for (int i = 0; i < n; ++i) order[i] = i;
        /* insertion sort keeps the comparator's total order explicit */
        for (int i = 1; i < n; ++i) {
            int k = order[i], j = i - 1;
            while (j >= 0 && taller_wider(trims, order[j], k) > 0) {
                order[j + 1] = order[j]; --j;
            }
            order[j + 1] = k;
        }
        int nshelf = 0;
        for (int k = 0; k < n; ++k) {
            int i = order[k], s = -1;
            for (int j = 0; j < nshelf; ++j) {
                if (sh[j].x + trims[i].w <= width) { s = j; break; }
            }
            if (s < 0) {
                if (nshelf >= n) { free(order); free(sh); return ATLAS_EINVAL; }
                s = nshelf++;
                sh[s].y = height;
                sh[s].h = trims[i].h;
                sh[s].x = 0;
                height += trims[i].h;
            }
            out[i].x = sh[s].x;
            out[i].y = sh[s].y;
            out[i].placed = 1;
            sh[s].x += trims[i].w;
        }
        shelves = nshelf;
        free(order); free(sh);
    }

    stats->width = width;
    stats->height = height;
    stats->used_px = used;
    stats->saved_px = 0;
    stats->shelves = shelves;
    return ATLAS_OK;
}

/* ---- convenience wrapper ------------------------------------------- */

int atlas_build(const AtlasTile *tiles, int n, int width, int trim, AtlasMode mode,
                AtlasSlot *out, AtlasStats *stats, int *bad_index) {
    set_bad(bad_index, -1);
    if (!tiles || !out || !stats || n <= 0 || width <= 0) return ATLAS_EINVAL;
    if (mode != ATLAS_SHELF && mode != ATLAS_GREEDY) return ATLAS_EINVAL;

    AtlasTrim *tr = (AtlasTrim *)malloc(sizeof(AtlasTrim) * (size_t)n);
    if (!tr) return ATLAS_EINVAL;
    int rc;
    if (trim) {
        rc = atlas_trim(tiles, n, tr, bad_index);
    } else {
        rc = ATLAS_OK;
        for (int i = 0; i < n; ++i) {
            if (tiles[i].w <= 0 || tiles[i].h <= 0) { set_bad(bad_index, i); rc = ATLAS_EDIM; break; }
            tr[i].tile = i;
            tr[i].src_x = 0; tr[i].src_y = 0;
            tr[i].w = tiles[i].w; tr[i].h = tiles[i].h;
        }
    }
    if (rc == ATLAS_OK) rc = atlas_place(tr, n, width, mode, out, stats, bad_index);
    if (rc == ATLAS_OK && trim) {
        long long saved = 0;
        for (int i = 0; i < n; ++i)
            saved += (long long)tiles[i].w * tiles[i].h - (long long)tr[i].w * tr[i].h;
        stats->saved_px = (int)saved;
    } else if (rc == ATLAS_OK) {
        stats->saved_px = 0;
    }
    free(tr);
    return rc;
}
