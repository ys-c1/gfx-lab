#include "atlas.h"

#include <stdlib.h>

typedef struct {
    int id;       /* original index in rects[]                 */
    int w, h;     /* effective dims after the rotation decision */
    int rotated;  /* 1 if the rect was turned 90 degrees        */
} AtlasItem;

/* Height-decreasing, then width-decreasing, then original index. */
static int item_less(const AtlasItem *a, const AtlasItem *b,
                     const AtlasRect *rects) {
    int ha = rects[a->id].h, hb = rects[b->id].h;
    if (ha != hb) return ha > hb;
    int wa = rects[a->id].w, wb = rects[b->id].w;
    if (wa != wb) return wa > wb;
    return a->id < b->id;
}

static void sift(AtlasItem *a, int lo, int n, const AtlasRect *rects) {
    AtlasItem v = a[lo];
    for (;;) {
        int child = 2 * lo + 1;
        if (child >= n) break;
        if (child + 1 < n && item_less(&a[child], &a[child + 1], rects))
            ++child;
        if (!item_less(&v, &a[child], rects)) break;
        a[lo] = a[child];
        lo = child;
    }
    a[lo] = v;
}

/* Self-contained heapsort so the module keeps no libc ordering dependency. */
static void sort_items(AtlasItem *a, int n, const AtlasRect *rects) {
    for (int i = n / 2 - 1; i >= 0; --i) sift(a, i, n, rects);
    for (int i = n - 1; i > 0; --i) {
        AtlasItem t = a[0];
        a[0] = a[i];
        a[i] = t;
        sift(a, 0, i, rects);
    }
}

int atlas_pack_ex(const AtlasRect *rects, int n, int width, unsigned flags,
                  const int *order, AtlasPos *out, AtlasStats *stats,
                  int *err_index) {
    if (err_index) *err_index = -1;
    if (!rects || !out || !stats || n <= 0 || width <= 0) return -1;
    flags &= (ATLAS_SORT_HEIGHT | ATLAS_ALLOW_ROTATE);

    AtlasItem *items = (AtlasItem *)malloc((size_t)n * sizeof(AtlasItem));
    if (!items) return -4;

    /* ---- stage 1: build the binning sequence ---- */
    if (order) {
        char *seen = (char *)calloc((size_t)n, 1);
        if (!seen) {
            free(items);
            return -4;
        }
        for (int k = 0; k < n; ++k) {
            int id = order[k];
            if (id < 0 || id >= n || seen[id]) {
                if (err_index) *err_index = k;
                free(seen);
                free(items);
                return -3;
            }
            seen[id] = 1;
            items[k].id = id;
        }
        free(seen);
    } else {
        for (int k = 0; k < n; ++k) items[k].id = k;
    }
    if (flags & ATLAS_SORT_HEIGHT) sort_items(items, n, rects);

    /* Every rect must have positive size and be placeable in the strip. */
    for (int k = 0; k < n; ++k) {
        const AtlasRect *r = &rects[items[k].id];
        if (r->w <= 0 || r->h <= 0) {
            if (err_index) *err_index = items[k].id;
            free(items);
            return -2;
        }
        int fit = (flags & ATLAS_ALLOW_ROTATE)
                      ? (r->w <= width || r->h <= width)
                      : (r->w <= width);
        if (!fit) {
            if (err_index) *err_index = items[k].id;
            free(items);
            return -2;
        }
    }

    /* ---- stage 2: walk the sequence once, decide orientations ---- */
    int cur_x = 0;
    for (int k = 0; k < n; ++k) {
        const AtlasRect *r = &rects[items[k].id];
        items[k].rotated = 0;
        int ew = r->w;
        if ((flags & ATLAS_ALLOW_ROTATE) && cur_x + ew > width) {
            /* does not fit as-is on the current shelf */
            if (r->h <= width) {
                items[k].rotated = 1;
                ew = r->h;
            }
        }
        if (cur_x + ew > width) {          /* open a new shelf */
            cur_x = 0;
            items[k].rotated = 0;          /* fresh shelf: keep natural orientation */
            ew = r->w;
            if ((flags & ATLAS_ALLOW_ROTATE) && ew > width && r->h <= width) {
                items[k].rotated = 1;      /* only possible when it never fits flat */
                ew = r->h;
            }
        }
        items[k].w = ew;
        items[k].h = items[k].rotated ? r->w : r->h;
        cur_x += ew;
    }

    /* ---- stage 3: reopen shelves by height and backfill coordinates ---- */
    cur_x = 0;
    int cur_y = 0, shelf_h = 0, used = 0, shelves = 1;
    for (int k = 0; k < n; ++k) {
        int id = items[k].id;
        const AtlasRect *r = &rects[id];
        if (cur_x + items[k].w > width) {
            cur_y += shelf_h;
            cur_x = 0;
            shelf_h = 0;
            ++shelves;
        }
        out[id].x = cur_x;
        out[id].y = cur_y;
        out[id].placed = 1;
        out[id].rotated = items[k].rotated;
        cur_x += items[k].w;
        if (items[k].h > shelf_h) shelf_h = items[k].h;
        used += r->w * r->h;              /* rotation preserves area */
    }

    stats->width = width;
    stats->used_px = used;
    stats->shelves = shelves;
    stats->height = cur_y + shelf_h;
    free(items);
    return 0;
}

int atlas_pack(const AtlasRect *rects, int n, int width,
               AtlasPos *out, AtlasStats *stats) {
    if (!rects || !out || !stats || n <= 0 || width <= 0) return -1;
    for (int i = 0; i < n; ++i) {
        if (rects[i].w <= 0 || rects[i].h <= 0 || rects[i].w > width) return -2;
    }
    return atlas_pack_ex(rects, n, width, 0, NULL, out, stats, NULL);
}
