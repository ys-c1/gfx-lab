#include "atlas.h"
#include <limits.h>
#include <stdlib.h>

/* Strip treated as unbounded: a free rect of this height is "infinite". */
#define ATLAS_INF (1 << 24)

/* A free rect of the maximal-rectangle table. */
typedef struct { int x, y, w, h; } FreeR;

typedef struct { FreeR *v; int n, cap; } FreeTable;

static int ft_reserve(FreeTable *t, int want) {
    if (t->cap >= want) return 0;
    int cap = t->cap ? t->cap : 8;
    while (cap < want) {
        if (cap > INT_MAX / 2) return -1;
        cap *= 2;
    }
    FreeR *nv = (FreeR*)realloc(t->v, sizeof(FreeR) * (size_t)cap);
    if (!nv) return -1;
    t->v = nv;
    t->cap = cap;
    return 0;
}

static void ft_remove(FreeTable *t, int i) {
    t->v[i] = t->v[t->n - 1];
    --t->n;
}

static int contains(const FreeR *outer, const FreeR *inner) {
    return outer->x <= inner->x && outer->y <= inner->y &&
           outer->x + outer->w >= inner->x + inner->w &&
           outer->y + outer->h >= inner->y + inner->h;
}

/* Insert one candidate free rect, keeping the table maximal: a candidate that
 * is contained in a live rect is dropped (so is an exact duplicate, the
 * earlier entry winning), and any live rect the candidate contains is
 * removed.  This is the incremental form of "prune all contained rects". */
static int ft_add(FreeTable *t, FreeR p) {
    for (int i = 0; i < t->n; ++i)
        if (contains(&t->v[i], &p)) return 0;
    for (int i = 0; i < t->n; ) {
        if (contains(&p, &t->v[i])) ft_remove(t, i);
        else ++i;
    }
    if (ft_reserve(t, t->n + 1) != 0) return -1;
    t->v[t->n++] = p;
    return 0;
}

/* Occupy [x,x+w) x [y,y+h): cut every overlapping free rect into up to four
 * pieces along the used rect's edges, then re-insert them maximally. */
static int ft_occupy(FreeTable *t, int x, int y, int w, int h) {
    const int right = x + w, bottom = y + h;
    for (int i = 0; i < t->n; ) {
        const FreeR f = t->v[i];
        if (x >= f.x + f.w || right <= f.x || y >= f.y + f.h || bottom <= f.y) {
            ++i;
            continue;
        }
        ft_remove(t, i);
        FreeR piece[4];
        int np = 0;
        if (x > f.x)            piece[np++] = (FreeR){ f.x, f.y, x - f.x, f.h };
        if (right < f.x + f.w)  piece[np++] = (FreeR){ right, f.y, f.x + f.w - right, f.h };
        if (y > f.y)            piece[np++] = (FreeR){ f.x, f.y, f.w, y - f.y };
        if (bottom < f.y + f.h) piece[np++] = (FreeR){ f.x, bottom, f.w, f.y + f.h - bottom };
        for (int k = 0; k < np; ++k)
            if (ft_add(t, piece[k]) != 0) return -1;
    }
    return 0;
}

/* Score for a candidate placement: bottom-left first (the strip has no fixed
 * height, so the lowest free space is the one that keeps the strip short),
 * then short-side fit, then long-side fit. */
typedef struct { int y, x, ss, ls; } Score;

static int score_less(const Score *a, const Score *b) {
    if (a->y != b->y) return a->y < b->y;
    if (a->x != b->x) return a->x < b->x;
    if (a->ss != b->ss) return a->ss < b->ss;
    return a->ls < b->ls;
}

/* Best free rect for rw x rh under the score above; 0 when nothing fits. */
static int find_fit(const FreeTable *t, int rw, int rh, int *out, Score *out_score) {
    int best = -1;
    Score best_score = { 0, 0, 0, 0 };
    for (int i = 0; i < t->n; ++i) {
        const FreeR *f = &t->v[i];
        if (f->w < rw || f->h < rh) continue;
        int dw = f->w - rw, dh = f->h - rh;
        Score cur = { f->y, f->x, dw < dh ? dw : dh, dw > dh ? dw : dh };
        if (best < 0 || score_less(&cur, &best_score)) {
            best = i;
            best_score = cur;
        }
    }
    if (best < 0) return 0;
    *out = best;
    *out_score = best_score;
    return 1;
}

/* Pick the orientation for rect i.  With ATLAS_ROT_ANY both are scored and
 * the better placement wins (0 degrees on an exact tie); a pinned hint is
 * used as-is.  Returns 1 with rw/rh/rot/fi set, 0 if no free rect fits, -1 if
 * the hint is illegal. */
static int consider(const FreeTable *t, const AtlasRect *r, int hint,
                    int *rw, int *rh, int *rot, int *fi) {
    int hint_rot[2] = { 0, 90 }, trials = 2;
    if (hint == ATLAS_ROT_0)  { hint_rot[0] = 0;  trials = 1; }
    if (hint == ATLAS_ROT_90) { hint_rot[0] = 90; trials = 1; }
    if (hint != ATLAS_ROT_ANY && hint != ATLAS_ROT_0 && hint != ATLAS_ROT_90) return -1;

    int best_fi = -1, best_rot = 0, best_rw = 0, best_rh = 0;
    Score best_score = { 0, 0, 0, 0 };
    for (int k = 0; k < trials; ++k) {
        int o = hint_rot[k];
        int cw = o == 90 ? r->h : r->w, ch = o == 90 ? r->w : r->h;
        int f = -1;
        Score sc;
        if (!find_fit(t, cw, ch, &f, &sc)) continue;
        if (best_fi < 0 || score_less(&sc, &best_score)) {
            best_fi = f;
            best_rot = o;
            best_rw = cw;
            best_rh = ch;
            best_score = sc;
        }
    }
    if (best_fi < 0) return 0;
    *rot = best_rot;
    *rw = best_rw;
    *rh = best_rh;
    *fi = best_fi;
    return 1;
}

/* ---- solver: bottom-left heuristic ------------------------------------- */

static int max_side(const AtlasRect *r) { return r->w > r->h ? r->w : r->h; }

/* Packing order: larger max side first, then larger area, then index. */
static int order_before(int a, int b, const AtlasRect *rects) {
    int ma = max_side(&rects[a]), mb = max_side(&rects[b]);
    if (ma != mb) return ma > mb;
    long long aa = (long long)rects[a].w * rects[a].h;
    long long ab = (long long)rects[b].w * rects[b].h;
    if (aa != ab) return aa > ab;
    return a < b;
}

/* Pack in the given visit order with bottom-left placement.  Free rects are
 * held in a max-rectangle table (no shelf rows) and each rect's orientation is
 * the better of 0/90 degrees.  Returns 0 or an ATLAS_ERR_* code; the offending
 * rect index goes through err_idx. */
static int pack_order(const AtlasRect *rects, const int *order, int n, int width,
                      int free_h, const int *orient, AtlasPos *out, int *used_out,
                      int *rotated_out, int *height_out, int *free_out,
                      int *err_idx) {
    FreeTable tab = {0};
    if (ft_reserve(&tab, 8) != 0) return ATLAS_ERR_ARGS;
    tab.v[tab.n++] = (FreeR){ 0, 0, width, free_h };

    int used = 0, rotated = 0, top = 0, rc = 0;
    for (int k = 0; k < n; ++k) {
        const int i = order[k];
        const AtlasRect *r = &rects[i];
        int hint = orient ? orient[i] : ATLAS_ROT_ANY;
        int rw = 0, rh = 0, rot = 0, fi = -1;
        int got = consider(&tab, r, hint, &rw, &rh, &rot, &fi);
        if (got <= 0) {
            /* A pinned orientation that has no room left is an orientation
             * failure, not a general space failure. */
            int pinned = hint == ATLAS_ROT_0 || hint == ATLAS_ROT_90;
            rc = got < 0 ? ATLAS_ERR_ORIENT
               : pinned  ? ATLAS_ERR_ORIENT
                         : (tab.n == 0 ? ATLAS_ERR_EMPTY : ATLAS_ERR_NO_FIT);
            *err_idx = i;
            goto done;
        }
        const FreeR slot = tab.v[fi];
        out[i].x = slot.x;
        out[i].y = slot.y;
        out[i].placed = 1;
        out[i].rot = rot;
        used += r->w * r->h;
        if (rot == 90) ++rotated;
        if (slot.y + rh > top) top = slot.y + rh;
        if (ft_occupy(&tab, slot.x, slot.y, rw, rh) != 0) {
            rc = ATLAS_ERR_ARGS;
            goto done;
        }
    }
    *used_out = used;
    *rotated_out = rotated;
    *height_out = top;
    *free_out = tab.n;
done:
    free(tab.v);
    return rc;
}

static int check_feasible(const AtlasJob *job, AtlasError *err) {
    const int n = job->n, width = job->width, height = job->height;
    const int bounded = height > 0;
    /* Single pass so the reported index is the lowest offending one. */
    for (int i = 0; i < n; ++i) {
        const AtlasRect *r = &job->rects[i];
        int hint = job->orient ? job->orient[i] : ATLAS_ROT_ANY;
        if (hint != ATLAS_ROT_ANY && hint != ATLAS_ROT_0 && hint != ATLAS_ROT_90) {
            if (err) { err->code = ATLAS_ERR_ORIENT; err->index = i; }
            return ATLAS_ERR_ORIENT;
        }
        if (r->w <= 0 || r->h <= 0) {
            if (err) { err->code = ATLAS_ERR_RECT; err->index = i; }
            return ATLAS_ERR_RECT;
        }
        int fits = hint == ATLAS_ROT_0  ? (r->w <= width && (!bounded || r->h <= height))
                 : hint == ATLAS_ROT_90 ? (r->h <= width && (!bounded || r->w <= height))
                 : (r->w <= width && (!bounded || r->h <= height)) ||
                   (r->h <= width && (!bounded || r->w <= height));
        if (!fits) {
            if (err) { err->code = ATLAS_ERR_TOO_BIG; err->index = i; }
            return ATLAS_ERR_TOO_BIG;
        }
    }
    return 0;
}

/* Legacy sequential shelf packer.  Row order, x accumulation and shelf height
 * are exactly the original implementation's, so layouts are unchanged. */
static int pack_shelf(const AtlasRect *rects, int n, int width, AtlasPos *out,
                      AtlasStats *stats, AtlasError *err) {
    int cur_x = 0, cur_y = 0, shelf_h = 0, used = 0, shelves = 1;
    for (int i = 0; i < n; ++i) {
        if (rects[i].w <= 0 || rects[i].h <= 0) {
            if (err) { err->code = ATLAS_ERR_RECT; err->index = i; }
            return ATLAS_ERR_RECT;
        }
        if (rects[i].w > width) {
            if (err) { err->code = ATLAS_ERR_TOO_BIG; err->index = i; }
            return ATLAS_ERR_TOO_BIG;
        }
        if (cur_x + rects[i].w > width) {
            cur_y += shelf_h; cur_x = 0; shelf_h = 0; ++shelves;
        }
        out[i].x = cur_x; out[i].y = cur_y; out[i].placed = 1; out[i].rot = 0;
        cur_x += rects[i].w;
        if (rects[i].h > shelf_h) shelf_h = rects[i].h;
        used += rects[i].w * rects[i].h;
    }
    stats->width = width;
    stats->used_px = used;
    stats->shelves = shelves;
    stats->height = cur_y + shelf_h;
    stats->rotated = 0;
    stats->free_rects = 0;
    long long area = (long long)width * stats->height;
    stats->waste = area > 0 ? 1.0 - (double)used / (double)area : 0.0;
    return 0;
}


static int pack_maxrect(const AtlasJob *job, AtlasPos *out, AtlasStats *stats,
                        AtlasError *err) {
    const AtlasRect *rects = job->rects;
    const int n = job->n, width = job->width;
    const int bounded = job->height > 0;

    int rc = check_feasible(job, err);
    if (rc != 0) return rc;

    int *order = (int*)malloc(sizeof(int) * (size_t)n);
    if (!order) return ATLAS_ERR_ARGS;
    for (int i = 0; i < n; ++i) order[i] = i;
    for (int a = 1; a < n; ++a) { /* deterministic insertion sort */
        int key = order[a], b = a - 1;
        while (b >= 0 && order_before(key, order[b], rects)) {
            order[b + 1] = order[b];
            --b;
        }
        order[b + 1] = key;
    }

    int used = 0, rotated = 0, top = 0, free_n = 0, bad = -1;
    rc = pack_order(rects, order, n, width, bounded ? job->height : ATLAS_INF,
                    job->orient, out, &used, &rotated, &top, &free_n, &bad);
    free(order);
    if (rc != 0) {
        if (err) { err->code = rc; err->index = bad; }
        return rc;
    }

    stats->width = width;
    stats->used_px = used;
    stats->rotated = rotated;
    stats->free_rects = free_n;
    stats->height = bounded ? job->height : top;
    stats->shelves = 0;
    for (int i = 0; i < n; ++i) {
        int seen = 0;
        for (int j = 0; j < i; ++j)
            if (out[j].y == out[i].y) { seen = 1; break; }
        if (!seen) ++stats->shelves;
    }
    {
        long long area = (long long)width * stats->height;
        stats->waste = area > 0 ? 1.0 - (double)used / (double)area : 0.0;
    }
    return 0;
}

int atlas_pack_job(const AtlasJob *job, AtlasPos *out, AtlasStats *stats, AtlasError *err) {
    if (err) { err->code = 0; err->index = -1; }
    if (!job || !job->rects || !out || !stats || job->n <= 0 || job->width <= 0)
        return ATLAS_ERR_ARGS;
    if (job->mode == ATLAS_MAXRECT) {
        if (job->height < 0) return ATLAS_ERR_ARGS;
        return pack_maxrect(job, out, stats, err);
    }
    /* Shelf mode is layout-frozen: a bounded height or orientation hints would
     * change the layout, so they are rejected rather than silently ignored. */
    if (job->mode != ATLAS_SHELF || job->height != 0 || job->orient)
        return ATLAS_ERR_ARGS;
    return pack_shelf(job->rects, job->n, job->width, out, stats, err);
}

int atlas_pack(const AtlasRect *rects, int n, int width, AtlasPos *out, AtlasStats *stats) {
    if (!rects || !out || !stats || n <= 0 || width <= 0) return ATLAS_ERR_ARGS;
    int rc = pack_shelf(rects, n, width, out, stats, NULL);
    /* Legacy contract: -2 covered both invalid rects and rects wider than the
     * strip, so keep that spelling for the old entry point. */
    return rc == ATLAS_ERR_TOO_BIG ? ATLAS_ERR_RECT : rc;
}

int atlas_pack_maxrect(const AtlasRect *rects, int n, int width, AtlasPos *out,
                       AtlasStats *stats, AtlasError *err) {
    AtlasJob job = { rects, n, width, 0, ATLAS_MAXRECT, NULL };
    return atlas_pack_job(&job, out, stats, err);
}

double atlas_waste_lower_bound(const AtlasRect *rects, int n, int width) {
    if (!rects || n <= 0 || width <= 0) return -1.0;
    long long area = 0;
    int longest_short_side = 0;
    for (int i = 0; i < n; ++i) {
        if (rects[i].w <= 0 || rects[i].h <= 0) return -1.0;
        int s = rects[i].w < rects[i].h ? rects[i].w : rects[i].h;
        if (s > longest_short_side) longest_short_side = s;
        area += (long long)rects[i].w * rects[i].h;
    }
    long long h = area / width + (area % width != 0);
    if (longest_short_side > h) h = longest_short_side;
    double waste = 1.0 - (double)area / ((double)width * (double)h);
    if (waste < 0.0) waste = 0.0;
    return waste;
}

double atlas_waste_upper_bound(const AtlasRect *rects, int n, int width) {
    if (!rects || n <= 0 || width <= 0) return -1.0;
    long long area = 0;
    int tallest = 0;
    for (int i = 0; i < n; ++i) {
        const AtlasRect *r = &rects[i];
        if (r->w <= 0 || r->h <= 0) return -1.0;
        int long_side = r->w > r->h ? r->w : r->h;
        int short_side = r->w > r->h ? r->h : r->w;
        /* Best orientation: put the long side along the strip when it fits,
         * otherwise the rect cannot be packed at all. */
        int hgt;
        if (long_side <= width) hgt = short_side;
        else if (short_side <= width) hgt = long_side;
        else return -1.0;
        if (hgt > tallest) tallest = hgt;
        area += (long long)r->w * r->h;
    }
    /* NFDH guarantee (Coffman/Garey/Johnson/Tarjan): the achieved strip
     * height never exceeds 2*area/width + tallest. */
    long long h = (2 * area + width - 1) / width + tallest;
    double waste = 1.0 - (double)area / ((double)width * (double)h);
    if (waste < 0.0) waste = 0.0;
    if (waste > 1.0) waste = 1.0;
    return waste;
}

static unsigned int mix32(unsigned int x) {
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

void atlas_make_rects(AtlasRect *rects, int n, int min_side, int max_side,
                      unsigned int seed) {
    if (!rects || n <= 0) return;
    if (min_side < 1) min_side = 1;
    if (max_side < min_side) max_side = min_side;
    const int span = max_side - min_side + 1;
    for (int i = 0; i < n; ++i) {
        unsigned int h1 = mix32(seed + (unsigned int)i * 2u);
        unsigned int h2 = mix32(seed ^ (0x9e3779b9U + (unsigned int)i * 2u + 1u));
        int hi = min_side + (int)(h1 % (unsigned int)span);
        int lo = min_side + (int)(h2 % (unsigned int)span);
        if (lo > hi) { int t = lo; lo = hi; hi = t; }
        /* Rotate the wide one to be width half the time, so the set is
         * orientation-neutral. */
        if (h2 & 1u) rects[i] = (AtlasRect){ lo, hi };
        else        rects[i] = (AtlasRect){ hi, lo };
    }
}
