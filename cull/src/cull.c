#include "cull.h"
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int overlaps(CullBox b, CullView v) {
    return !(b.x + b.w < v.x || b.x > v.x + v.w ||
             b.y + b.h < v.y || b.y > v.y + v.h);
}

int cull_run(const CullBox *boxes, int n, CullView view, CullResult *out) {
    if (!boxes || !out || n < 0) return -1;
    if (view.w <= 0 || view.h <= 0) return -2;
    out->ids = (int*)malloc(sizeof(int) * (size_t)(n > 0 ? n : 1));
    if (!out->ids) return -1;
    out->count = 0;
    for (int i = 0; i < n; ++i)
        if (overlaps(boxes[i], view)) out->ids[out->count++] = i;
    return 0;
}

int cull_count_within(const CullBox *boxes, int n, CullView view) {
    if (!boxes || n < 0) return -1;
    int c = 0;
    for (int i = 0; i < n; ++i) if (overlaps(boxes[i], view)) ++c;
    return c;
}

/* ------------------------------------------------------------------ */
/* Spatial hash, two stages.                                          */
/*                                                                    */
/* Grid origin is the minimum corner of the object bounds; cell size  */
/* is either caller supplied or derived from the larger world extent  */
/* so that the target bucket count is roughly the object count.       */
/* Bucket index is gy * gw + gx, so ascending bucket index is exactly */
/* (bucket_y, bucket_x) lexicographic order.  Objects are inserted in */
/* ascending object index, which makes every bucket and the export    */
/* byte-for-byte reproducible.                                        */
/* ------------------------------------------------------------------ */

struct CullHash {
    const CullBox *boxes;
    int n;

    double ox, oy;
    double cw, ch;
    int gw, gh;
    int nb;

    int *cell_start; /* nb + 1 offsets into cell_ids */
    int *cell_ids;   /* entries, ascending object index within each bucket */

    long entries;
    long collisions;
    int occupied;

    CullHashStats stats;

    /* Query scratch, reused across queries (not thread safe). */
    int *mark;
    int gen;
    int *cand;
};

static int view_valid(CullView v) {
    return isfinite(v.x) && isfinite(v.y) && isfinite(v.w) && isfinite(v.h) &&
           v.w > 0.0 && v.h > 0.0;
}

static void set_err(CullHashError *err, int code, int index, long limit,
                    const char *field, const char *msg) {
    if (!err) return;
    err->code = code;
    err->index = index;
    err->limit = limit;
    err->field = field;
    if (msg) {
        snprintf(err->message, sizeof err->message, "%s", msg);
    } else {
        err->message[0] = '\0';
    }
}

/* Inclusive cell range [*lo, *hi] covering the closed interval [lo, hi].
 * *lo > *hi means the interval does not touch the grid. */
static void cell_range(double lo, double hi, double origin, double cell,
                       int dim, int *out_lo, int *out_hi) {
    double f0 = floor((lo - origin) / cell);
    double f1 = floor((hi - origin) / cell);
    if (f1 < 0.0 || f0 > (double)(dim - 1)) {
        *out_lo = 1;
        *out_hi = 0;
        return;
    }
    if (f0 < 0.0) f0 = 0.0;
    if (f1 > (double)(dim - 1)) f1 = (double)(dim - 1);
    *out_lo = (int)f0;
    *out_hi = (int)f1;
}

int cull_hash_build(const CullBox *boxes, int n, const CullHashOpts *opts,
                    CullHash **out, CullHashError *err) {
    if (err) memset(err, 0, sizeof *err);
    if (out) *out = NULL;
    if (!boxes || !out || n < 0) {
        if (n < 0) set_err(err, CULL_ERR_ARG, -1, 0, "n", "n < 0");
        else set_err(err, CULL_ERR_ARG, -1, 0, NULL, "null boxes or out");
        return CULL_ERR_ARG;
    }

    int max_buckets = (opts && opts->max_buckets > 0) ? opts->max_buckets
                                                      : CULL_HASH_MAX_BUCKETS;
    long max_entries = (opts && opts->max_entries > 0) ? opts->max_entries
                                                       : CULL_HASH_MAX_ENTRIES;
    /* cell_start / cell_ids are int indexed, so the entry count must fit. */
    if (max_entries > (long)INT_MAX) max_entries = (long)INT_MAX;

    for (int i = 0; i < n; ++i) {
        const CullBox *b = &boxes[i];
        const char *f = !isfinite(b->x) ? "x"
                      : !isfinite(b->y) ? "y"
                      : !isfinite(b->w) ? "w"
                      : !isfinite(b->h) ? "h" : NULL;
        if (f) {
            char msg[128];
            snprintf(msg, sizeof msg, "box %d field %s is not finite", i, f);
            set_err(err, CULL_ERR_HASH_BOX, i, 0, f, msg);
            return CULL_ERR_HASH_BOX;
        }
        if (b->w < 0.0 || b->h < 0.0) {
            const char *f2 = b->w < 0.0 ? "w" : "h";
            char msg[128];
            snprintf(msg, sizeof msg, "box %d field %s = %.17g is negative",
                     i, f2, f2[0] == 'w' ? b->w : b->h);
            set_err(err, CULL_ERR_HASH_BOX, i, 0, f2, msg);
            return CULL_ERR_HASH_BOX;
        }
    }

    CullHash *h = (CullHash*)calloc(1, sizeof *h);
    if (!h) {
        set_err(err, CULL_ERR_HASH_NOMEM, -1, 0, NULL, "out of memory");
        return CULL_ERR_HASH_NOMEM;
    }
    h->boxes = boxes;
    h->n = n;

    double minx = 0.0, miny = 0.0, maxx = 0.0, maxy = 0.0;
    if (n > 0) {
        minx = maxx = boxes[0].x;
        miny = maxy = boxes[0].y;
        for (int i = 0; i < n; ++i) {
            const CullBox *b = &boxes[i];
            if (b->x < minx) minx = b->x;
            if (b->y < miny) miny = b->y;
            if (b->x + b->w > maxx) maxx = b->x + b->w;
            if (b->y + b->h > maxy) maxy = b->y + b->h;
        }
    }
    double ex = maxx - minx, ey = maxy - miny;
    if (!(ex > 0.0)) ex = 0.0;
    if (!(ey > 0.0)) ey = 0.0;

    double cw, ch;
    int explicit_cell = opts && opts->cell_w > 0.0 && opts->cell_h > 0.0;
    if (explicit_cell) {
        cw = opts->cell_w;
        ch = opts->cell_h;
    } else {
        /* Target about one bucket per object, capped by max_buckets. */
        double target = (double)(n > 0 ? n : 1);
        if (target > (double)max_buckets) target = (double)max_buckets;
        double base = ex > ey ? ex : ey;
        double s = (base > 0.0) ? base / sqrt(target) : 1.0;
        if (!(s > 0.0) || !isfinite(s)) s = 1.0;
        cw = ch = s;
        /* Ceil-division can overshoot the cap; grow the cell deterministically. */
        for (int iter = 0; iter < 64; ++iter) {
            double gx = ex > 0.0 ? ceil(ex / s) : 1.0;
            double gy = ey > 0.0 ? ceil(ey / s) : 1.0;
            double prod = gx * gy;
            if (prod <= (double)max_buckets) break;
            s *= sqrt(prod / (double)max_buckets) * 1.0000001;
            if (!isfinite(s) || s <= 0.0) { s = 1.0; break; }
        }
        cw = ch = s;
    }
    if (!isfinite(cw) || !isfinite(ch) || cw <= 0.0 || ch <= 0.0) {
        free(h);
        set_err(err, CULL_ERR_ARG, -1, 0, "cell",
                "cell size must be finite and positive");
        return CULL_ERR_ARG;
    }

    long long gw = ex > 0.0 ? (long long)ceil(ex / cw) : 1;
    long long gh = ey > 0.0 ? (long long)ceil(ey / ch) : 1;
    if (gw < 1) gw = 1;
    if (gh < 1) gh = 1;
    if (gw > INT_MAX) gw = INT_MAX;
    if (gh > INT_MAX) gh = INT_MAX;
    /* Check the factor cap before multiplying so the product cannot
     * overflow long long when the world extent is extreme. */
    long long nb_ll = (gw > (long long)max_buckets || gh > (long long)max_buckets)
                          ? (long long)max_buckets + 1
                          : gw * gh;
    if (nb_ll > (long long)max_buckets) {
        char msg[160];
        snprintf(msg, sizeof msg,
                 "bucket count exceeds limit %d (grid %lld x %lld)",
                 max_buckets, gw, gh);
        free(h);
        set_err(err, CULL_ERR_HASH_LIMIT, -1, (long)max_buckets, "buckets", msg);
        return CULL_ERR_HASH_LIMIT;
    }

    h->ox = minx;
    h->oy = miny;
    h->cw = cw;
    h->ch = ch;
    h->gw = (int)gw;
    h->gh = (int)gh;
    h->nb = (int)nb_ll;

    /* First pass: count entries, enforcing the insertion cap before alloc. */
    long long total = 0;
    for (int i = 0; i < n; ++i) {
        int x0, x1, y0, y1;
        const CullBox *b = &boxes[i];
        cell_range(b->x, b->x + b->w, h->ox, h->cw, h->gw, &x0, &x1);
        cell_range(b->y, b->y + b->h, h->oy, h->ch, h->gh, &y0, &y1);
        if (x1 < x0 || y1 < y0) continue;
        long long nx = (long long)x1 - x0 + 1;
        long long ny = (long long)y1 - y0 + 1;
        long long add = nx * ny;
        if (add > max_entries - total || add < 0) {
            char msg[160];
            snprintf(msg, sizeof msg,
                     "insertions exceed limit %ld at box %d", max_entries, i);
            free(h);
            set_err(err, CULL_ERR_HASH_LIMIT, i, max_entries, "entries", msg);
            return CULL_ERR_HASH_LIMIT;
        }
        total += add;
    }

    h->cell_start = (int*)calloc((size_t)h->nb + 1, sizeof(int));
    h->cell_ids = total > 0 ? (int*)malloc(sizeof(int) * (size_t)total) : NULL;
    h->mark = (int*)calloc((size_t)(n > 0 ? n : 1), sizeof(int));
    h->cand = (int*)malloc(sizeof(int) * (size_t)(n > 0 ? n : 1));
    if (!h->cell_start || (total > 0 && !h->cell_ids) || !h->mark || !h->cand) {
        cull_hash_free(h);
        set_err(err, CULL_ERR_HASH_NOMEM, -1, 0, NULL, "out of memory");
        return CULL_ERR_HASH_NOMEM;
    }
    h->gen = 0;

    /* Count per bucket. */
    for (int i = 0; i < n; ++i) {
        int x0, x1, y0, y1;
        const CullBox *b = &boxes[i];
        cell_range(b->x, b->x + b->w, h->ox, h->cw, h->gw, &x0, &x1);
        cell_range(b->y, b->y + b->h, h->oy, h->ch, h->gh, &y0, &y1);
        if (x1 < x0 || y1 < y0) continue;
        for (int gy = y0; gy <= y1; ++gy)
            for (int gx = x0; gx <= x1; ++gx)
                ++h->cell_start[gy * h->gw + gx + 1];
    }
    for (int b = 0; b < h->nb; ++b) h->cell_start[b + 1] += h->cell_start[b];
    h->entries = total;

    int *cursor = (int*)malloc(sizeof(int) * (size_t)(h->nb > 0 ? h->nb : 1));
    if (!cursor) {
        cull_hash_free(h);
        set_err(err, CULL_ERR_HASH_NOMEM, -1, 0, NULL, "out of memory");
        return CULL_ERR_HASH_NOMEM;
    }
    for (int b = 0; b < h->nb; ++b) cursor[b] = h->cell_start[b];

    /* Second pass: fill in ascending object index (reproducible order). */
    for (int i = 0; i < n; ++i) {
        int x0, x1, y0, y1;
        const CullBox *b = &boxes[i];
        cell_range(b->x, b->x + b->w, h->ox, h->cw, h->gw, &x0, &x1);
        cell_range(b->y, b->y + b->h, h->oy, h->ch, h->gh, &y0, &y1);
        if (x1 < x0 || y1 < y0) continue;
        for (int gy = y0; gy <= y1; ++gy)
            for (int gx = x0; gx <= x1; ++gx) {
                int c = gy * h->gw + gx;
                h->cell_ids[cursor[c]++] = i;
            }
    }
    free(cursor);

    h->occupied = 0;
    for (int b = 0; b < h->nb; ++b) {
        int cnt = h->cell_start[b + 1] - h->cell_start[b];
        if (cnt > 0) ++h->occupied;
    }
    h->collisions = h->entries - h->occupied;
    h->stats.buckets = h->nb;
    h->stats.occupied = h->occupied;
    h->stats.entries = h->entries;
    h->stats.collisions = h->collisions;
    h->stats.collision_rate = h->entries > 0
        ? 100.0 * (double)h->collisions / (double)h->entries : 0.0;

    *out = h;
    return CULL_OK;
}

static int cmp_int(const void *a, const void *b) {
    int x = *(const int*)a, y = *(const int*)b;
    return (x > y) - (x < y);
}

int cull_hash_query(CullHash *h, CullView view, CullHashResult *out,
                    CullHashError *err) {
    if (err) memset(err, 0, sizeof *err);
    if (!out) {
        set_err(err, CULL_ERR_ARG, -1, 0, NULL, "null out");
        return CULL_ERR_ARG;
    }
    memset(out, 0, sizeof *out);
    if (!h) {
        set_err(err, CULL_ERR_ARG, -1, 0, NULL, "null index");
        return CULL_ERR_ARG;
    }
    if (!view_valid(view)) {
        set_err(err, CULL_ERR_VIEW, -1, 0, "view",
                "view w/h must be finite and > 0");
        return CULL_ERR_VIEW;
    }

    /* Objects that overlap the view necessarily share at least one bucket
     * with it, because both are closed ranges on the same grid. */
    int x0, x1, y0, y1;
    cell_range(view.x, view.x + view.w, h->ox, h->cw, h->gw, &x0, &x1);
    cell_range(view.y, view.y + view.h, h->oy, h->ch, h->gh, &y0, &y1);

    int visited = 0, nc = 0;
    if (!(x1 < x0 || y1 < y0)) {
        if (h->gen == INT_MAX) {
            memset(h->mark, 0, sizeof(int) * (size_t)(h->n > 0 ? h->n : 1));
            h->gen = 0;
        }
        ++h->gen;
        for (int gy = y0; gy <= y1; ++gy)
            for (int gx = x0; gx <= x1; ++gx) {
                int b = gy * h->gw + gx;
                ++visited;
                for (int k = h->cell_start[b]; k < h->cell_start[b + 1]; ++k) {
                    int id = h->cell_ids[k];
                    if (h->mark[id] != h->gen) {
                        h->mark[id] = h->gen;
                        h->cand[nc++] = id;
                    }
                }
            }
    }

    /* Every visible object is in the candidate set, so this bound is enough. */
    int *ids = (int*)malloc(sizeof(int) * (size_t)(nc > 0 ? nc : 1));
    if (!ids) {
        set_err(err, CULL_ERR_HASH_NOMEM, -1, 0, NULL, "out of memory");
        return CULL_ERR_HASH_NOMEM;
    }
    int count = 0;
    for (int k = 0; k < nc; ++k) {
        int id = h->cand[k];
        if (overlaps(h->boxes[id], view)) ids[count++] = id;
    }
    qsort(ids, (size_t)count, sizeof(int), cmp_int);

    out->ids = ids;
    out->count = count;
    out->legacy = 0;
    out->candidates = nc;
    out->visited_buckets = visited;
    out->stats = h->stats;
    return CULL_OK;
}

int cull_run_hashed(const CullBox *boxes, int n, CullView view,
                    const CullHashOpts *opts, CullHashResult *out,
                    CullHashError *err) {
    if (err) memset(err, 0, sizeof *err);
    if (!out) {
        set_err(err, CULL_ERR_ARG, -1, 0, NULL, "null out");
        return CULL_ERR_ARG;
    }
    memset(out, 0, sizeof *out);

    if (opts && opts->legacy) {
        /* New path disabled: delegate to the original scan, unchanged. */
        CullResult r;
        int rc = cull_run(boxes, n, view, &r);
        if (rc != 0) {
            set_err(err, rc == -2 ? CULL_ERR_VIEW : CULL_ERR_ARG,
                    -1, 0, rc == -2 ? "view" : NULL,
                    rc == -2 ? "view w/h must be > 0" : "invalid argument");
            return rc;
        }
        out->ids = r.ids;
        out->count = r.count;
        out->legacy = 1;
        out->candidates = n;
        out->visited_buckets = 0;
        return CULL_OK;
    }

    CullHash *h = NULL;
    int rc = cull_hash_build(boxes, n, opts, &h, err);
    if (rc != CULL_OK) return rc;

    CullHashResult q;
    rc = cull_hash_query(h, view, &q, err);
    cull_hash_free(h);
    if (rc != CULL_OK) return rc;
    *out = q;
    return CULL_OK;
}

int cull_hash_get_stats(const CullHash *h, CullHashStats *out) {
    if (!h || !out) return CULL_ERR_ARG;
    *out = h->stats;
    return CULL_OK;
}

int cull_hash_export_buckets(const CullHash *h, const char *path) {
    if (!h || !path) return CULL_ERR_ARG;
    FILE *f = fopen(path, "w");
    if (!f) return CULL_ERR_HASH_IO;

    fprintf(f, "# cull spatial hash bucket table\n");
    fprintf(f, "grid %d %d\n", h->gw, h->gh);
    fprintf(f, "origin %.17g %.17g\n", h->ox, h->oy);
    fprintf(f, "cell %.17g %.17g\n", h->cw, h->ch);
    fprintf(f, "n %d\n", h->n);
    fprintf(f, "buckets %d\n", h->stats.buckets);
    fprintf(f, "occupied %d\n", h->stats.occupied);
    fprintf(f, "entries %ld\n", h->stats.entries);
    fprintf(f, "collisions %ld\n", h->stats.collisions);
    fprintf(f, "collision_rate %.2f\n", h->stats.collision_rate);
    for (int b = 0; b < h->nb; ++b) {
        int lo = h->cell_start[b], hi = h->cell_start[b + 1];
        if (hi <= lo) continue;
        fprintf(f, "bucket %d %d %d", b % h->gw, b / h->gw, hi - lo);
        for (int k = lo; k < hi; ++k) fprintf(f, " %d", h->cell_ids[k]);
        fprintf(f, "\n");
    }
    int bad = ferror(f);
    if (fclose(f) != 0) bad = 1;
    return bad ? CULL_ERR_HASH_IO : CULL_OK;
}

void cull_hash_free(CullHash *h) {
    if (!h) return;
    free(h->cell_start);
    free(h->cell_ids);
    free(h->mark);
    free(h->cand);
    free(h);
}

void cull_hash_result_free(CullHashResult *r) {
    if (!r) return;
    free(r->ids);
    memset(r, 0, sizeof *r);
}
