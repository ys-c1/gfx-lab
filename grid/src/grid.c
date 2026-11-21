#include "grid.h"
#include <stdlib.h>

static const char *g_last_limit = "none";

const char *grid_last_limit(void) { return g_last_limit; }

void grid_stats_init(GridStats *s) {
    if (!s) return;
    s->group_count = 0;
    s->merge_pairs = 0;
    s->duplicates = 0;
    s->candidates = 0;
    s->group_ids = NULL;
    s->group_boxes = NULL;
}

void grid_stats_free(GridStats *s) {
    if (!s) return;
    free(s->group_ids);
    free(s->group_boxes);
    s->group_ids = NULL;
    s->group_boxes = NULL;
}

static int box_overlaps(const GBox *a, const GBox *b) {
    return !(a->x + a->w < b->x || a->x > b->x + b->w ||
             a->y + a->h < b->y || a->y > b->y + b->h);
}

int grid_build(const GBox *boxes, int n,
               double gx, double gy, double gw, double gh,
               int cols, int rows, Grid *out) {
    if (!boxes || !out || n < 0) return -1;
    if (gw <= 0.0 || gh <= 0.0 || cols <= 0 || rows <= 0) {
        g_last_limit = "bad grid geometry";
        return -4;
    }
    if (n > GRID_MAX_OBJECTS) {
        g_last_limit = "GRID_MAX_OBJECTS=" "100000";
        return -2;
    }
    long long cells = (long long)cols * (long long)rows;
    if (cells > GRID_MAX_CELLS) {
        g_last_limit = "GRID_MAX_CELLS=" "1000000";
        return -3;
    }
    for (int i = 0; i < n; ++i)
        if (boxes[i].w <= 0.0 || boxes[i].h <= 0.0) {
            g_last_limit = "invalid box";
            return -5;
        }

    out->cell_head = (int*)malloc(sizeof(int) * (size_t)cells);
    /* entries can outnumber objects: every object may register in
     * several cells, so grow these buffers on demand. */
    int entry_cap = n > 0 ? n : 8;
    out->next = (int*)malloc(sizeof(int) * (size_t)entry_cap);
    out->ids = (int*)malloc(sizeof(int) * (size_t)entry_cap);
    if (!out->cell_head || !out->next || !out->ids) {
        free(out->cell_head); free(out->next); free(out->ids);
        g_last_limit = "allocation failure";
        return -1;
    }
    for (long long c = 0; c < cells; ++c) out->cell_head[c] = -1;

    out->cols = cols; out->rows = rows;
    out->x0 = gx; out->y0 = gy; out->w = gw; out->h = gh;
    out->n = 0;
    out->candidates = 0;

    const double cw = gw / (double)cols;
    const double ch = gh / (double)rows;

    for (int i = 0; i < n; ++i) {
        const GBox *b = &boxes[i];
        /* clip box to grid bounds; skip fully-outside boxes */
        double x0 = b->x,           y0 = b->y;
        double x1 = b->x + b->w,    y1 = b->y + b->h;
        if (x1 < gx || x0 > gx + gw || y1 < gy || y0 > gy + gh) continue;
        if (x0 < gx) x0 = gx;
        if (y0 < gy) y0 = gy;
        if (x1 > gx + gw) x1 = gx + gw;
        if (y1 > gy + gh) y1 = gy + gh;

        int cx0 = (int)((x0 - gx) / cw);
        int cy0 = (int)((y0 - gy) / ch);
        int cx1 = (int)((x1 - gx) / cw);
        int cy1 = (int)((y1 - gy) / ch);
        if (cx0 < 0) cx0 = 0;
        if (cy0 < 0) cy0 = 0;
        if (cx1 > cols - 1) cx1 = cols - 1;
        if (cy1 > rows - 1) cy1 = rows - 1;
        /* x1 exactly on the right border maps to cols; already clamped */

        int span = (cx1 - cx0 + 1) * (cy1 - cy0 + 1);
        if (out->n + span > entry_cap) {
            while (out->n + span > entry_cap) entry_cap *= 2;
            int *nn = (int*)realloc(out->next,
                                    sizeof(int) * (size_t)entry_cap);
            int *ni = (int*)realloc(out->ids,
                                    sizeof(int) * (size_t)entry_cap);
            if (!nn || !ni) {
                free(nn); free(ni);
                free(out->cell_head); free(out->next); free(out->ids);
                g_last_limit = "allocation failure";
                return -1;
            }
            out->next = nn; out->ids = ni;
        }

        for (int cy = cy0; cy <= cy1; ++cy) {
            for (int cx = cx0; cx <= cx1; ++cx) {
                int cell = cy * cols + cx;
                int slot = out->n++;
                out->ids[slot] = i;
                out->next[slot] = out->cell_head[cell];
                out->cell_head[cell] = slot;
                ++out->candidates;
            }
        }
    }
    g_last_limit = "none";
    return 0;
}

int grid_duplicates(const Grid *g) {
    if (!g || g->n == 0) return 0;
    /* ids[] stores original object indices; distinct objects are found
     * by marking those indices, so size must cover max(id)+1. */
    int max_id = -1;
    for (int k = 0; k < g->n; ++k)
        if (g->ids[k] > max_id) max_id = g->ids[k];
    char *seen = (char*)calloc((size_t)(max_id + 2), 1);
    if (!seen) return -1;
    int distinct = 0;
    for (int k = 0; k < g->n; ++k) {
        if (!seen[g->ids[k]]) { seen[g->ids[k]] = 1; ++distinct; }
    }
    free(seen);
    return g->n - distinct;
}

int grid_assign_unique(const GBox *boxes, int n,
                       double gx, double gy, double gw, double gh,
                       int cols, int rows, Grid *out) {
    if (!boxes || !out || n < 0) return -1;
    if (gw <= 0.0 || gh <= 0.0 || cols <= 0 || rows <= 0) {
        g_last_limit = "bad grid geometry";
        return -4;
    }
    if (n > GRID_MAX_OBJECTS) {
        g_last_limit = "GRID_MAX_OBJECTS=" "100000";
        return -2;
    }
    long long cells = (long long)cols * (long long)rows;
    if (cells > GRID_MAX_CELLS) {
        g_last_limit = "GRID_MAX_CELLS=" "1000000";
        return -3;
    }
    for (int i = 0; i < n; ++i)
        if (boxes[i].w <= 0.0 || boxes[i].h <= 0.0) {
            g_last_limit = "invalid box";
            return -5;
        }

    out->cell_head = (int*)malloc(sizeof(int) * (size_t)cells);
    /* unique assignment: exactly one entry per box at most */
    out->next = (int*)malloc(sizeof(int) * (size_t)(n > 0 ? n : 1));
    out->ids = (int*)malloc(sizeof(int) * (size_t)(n > 0 ? n : 1));
    if (!out->cell_head || !out->next || !out->ids) {
        free(out->cell_head); free(out->next); free(out->ids);
        g_last_limit = "allocation failure";
        return -1;
    }
    for (long long c = 0; c < cells; ++c) out->cell_head[c] = -1;

    out->cols = cols; out->rows = rows;
    out->x0 = gx; out->y0 = gy; out->w = gw; out->h = gh;
    out->n = 0;
    out->candidates = 0;

    const double cw = gw / (double)cols;
    const double ch = gh / (double)rows;

    for (int i = 0; i < n; ++i) {
        const GBox *b = &boxes[i];
        /* owner cell = cell holding the top-left anchor; boxes whose
         * anchor lies strictly outside the grid are skipped. An anchor
         * on the right/top border is inside and clamps to the last cell. */
        if (b->x < gx || b->y < gy || b->x > gx + gw || b->y > gy + gh)
            continue;
        int cx = (int)((b->x - gx) / cw);
        int cy = (int)((b->y - gy) / ch);
        if (cx < 0) cx = 0;
        if (cy < 0) cy = 0;
        if (cx > cols - 1) cx = cols - 1;
        if (cy > rows - 1) cy = rows - 1;

        int cell = cy * cols + cx;
        int slot = out->n++;
        out->ids[slot] = i;
        out->next[slot] = out->cell_head[cell];
        out->cell_head[cell] = slot;
        ++out->candidates;
    }
    g_last_limit = "none";
    return 0;
}

void grid_free(Grid *g) {
    if (!g) return;
    free(g->cell_head);
    free(g->next);
    free(g->ids);
    g->cell_head = NULL;
    g->next = NULL;
    g->ids = NULL;
    g->n = 0;
}

/* ---- union-find merge pass ---- */

static int *uf_parent;

static int uf_find(int x) {
    while (uf_parent[x] != x) {
        uf_parent[x] = uf_parent[uf_parent[x]];
        x = uf_parent[x];
    }
    return x;
}

static void uf_union(int a, int b) {
    a = uf_find(a); b = uf_find(b);
    if (a != b) {
        if (a < b) uf_parent[b] = a; else uf_parent[a] = b;
    }
}

int grid_merge_groups(const GBox *boxes, int n, GridStats *st) {
    if (!boxes || !st || n < 0) return -1;
    grid_stats_init(st);
    if (n == 0) { g_last_limit = "none"; return 0; }

    uf_parent = (int*)malloc(sizeof(int) * (size_t)n);
    if (!uf_parent) { g_last_limit = "allocation failure"; return -1; }
    for (int i = 0; i < n; ++i) uf_parent[i] = i;

    int pairs = 0;
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j)
            if (box_overlaps(&boxes[i], &boxes[j])) {
                ++pairs;
                uf_union(i, j);
            }
    st->merge_pairs = pairs;

    /* compact roots to consecutive group ids */
    int *remap = (int*)malloc(sizeof(int) * (size_t)n);
    st->group_ids = (int*)malloc(sizeof(int) * (size_t)n);
    if (!remap || !st->group_ids) {
        free(remap); free(st->group_ids); st->group_ids = NULL;
        free(uf_parent); uf_parent = NULL;
        g_last_limit = "allocation failure";
        return -1;
    }
    for (int i = 0; i < n; ++i) remap[i] = -1;
    int groups = 0;
    for (int i = 0; i < n; ++i) {
        int r = uf_find(i);
        if (remap[r] < 0) remap[r] = groups++;
        st->group_ids[i] = remap[r];
    }
    st->group_count = groups;

    st->group_boxes = (GBox*)calloc((size_t)groups, sizeof(GBox));
    if (!st->group_boxes) {
        free(remap); free(st->group_ids); st->group_ids = NULL;
        free(uf_parent); uf_parent = NULL;
        g_last_limit = "allocation failure";
        return -1;
    }
    for (int g = 0; g < groups; ++g) {
        st->group_boxes[g].x = 1e300;
        st->group_boxes[g].y = 1e300;
        st->group_boxes[g].w = st->group_boxes[g].h = -1.0;
        st->group_boxes[g].layer = 0;
    }
    for (int i = 0; i < n; ++i) {
        int g = st->group_ids[i];
        GBox *dst = &st->group_boxes[g];
        double ix1 = boxes[i].x + boxes[i].w;
        double iy1 = boxes[i].y + boxes[i].h;
        if (boxes[i].x < dst->x) dst->x = boxes[i].x;
        if (boxes[i].y < dst->y) dst->y = boxes[i].y;
        if (dst->w < 0.0 || ix1 > dst->x + dst->w) dst->w = ix1 - dst->x;
        if (dst->h < 0.0 || iy1 > dst->y + dst->h) dst->h = iy1 - dst->y;
    }

    free(remap);
    free(uf_parent);
    uf_parent = NULL;
    g_last_limit = "none";
    return 0;
}
