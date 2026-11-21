#include "atlas_vol.h"

#if ATLAS_USE_VOL

#include <stdlib.h>
#include <string.h>

static void book_reset(AvolBook *book) {
    book->cells = NULL;
    book->vols = NULL;
    book->volume_count = 0;
    book->n = 0;
    memset(&book->lim, 0, sizeof(book->lim));
}

void avol_free(AvolBook *book) {
    if (!book) return;
    free(book->cells);
    free(book->vols);
    book_reset(book);
}

static int limits_ok(const AvolLimits *lim) {
    return lim && lim->page_w > 0 && lim->page_h > 0 &&
           (lim->scale == 1 || lim->scale == 2) &&
           lim->bytes_per_px > 0 && lim->cap_bytes > 0;
}

/* 每册可用面积上限：显存上限换算回 1x 点面积。 */
static long cap_points(const AvolLimits *lim) {
    long per_px = (long)lim->scale * lim->scale * lim->bytes_per_px;
    return lim->cap_bytes / per_px;
}

static long vol_bytes(const AvolLimits *lim, long px) {
    return px * (long)lim->scale * (long)lim->scale * lim->bytes_per_px;
}

/* ============================ 阶段一：切册 ============================ */

int avol_split(const AtlasRect *rects, int n, const AvolLimits *lim,
               int *vol_of, int *volume_count, int *err_item) {
    if (err_item) *err_item = -1;
    if (!rects || !limits_ok(lim) || !vol_of || !volume_count || n <= 0)
        return AVOL_ERR_ARG;

    long cap_px = cap_points(lim);
    long *vused = (long*)calloc((size_t)n, sizeof *vused);
    if (!vused) return AVOL_ERR_NOMEM;

    int vol = 0, cur_x = 0, cur_y = 0, shelf = 0;
    for (int i = 0; i < n; ++i) {
        int w = rects[i].w, h = rects[i].h;
        if (w <= 0 || h <= 0 || w > lim->page_w || h > lim->page_h) {
            if (err_item) *err_item = i;
            free(vused);
            return AVOL_ERR_TOO_BIG;
        }
        long rect_px = (long)w * h;
        /* 单矩形必须能被一册装下：页面积与字节上限同时满足。 */
        if (rect_px > cap_px) {
            if (err_item) *err_item = i;
            free(vused);
            return AVOL_ERR_TOO_BIG;
        }
        /* 货架切页：宽度换行、高度到顶换册。 */
        if (cur_x + w > lim->page_w) { cur_y += shelf; cur_x = 0; shelf = 0; }
        if (cur_y + h > lim->page_h) { ++vol; cur_x = cur_y = shelf = 0; }
        /* 显存上限：本册再加这一块会超，则另起一册。 */
        if (vused[vol] + rect_px > cap_px && vused[vol] > 0) {
            ++vol; cur_x = cur_y = shelf = 0;
        }
        vol_of[i] = vol;
        vused[vol] += rect_px;
        cur_x += w;
        if (h > shelf) shelf = h;
    }
    *volume_count = vol + 1;
    free(vused);
    return AVOL_OK;
}

/* ============================ 阶段二：回填 ============================ */

static void layout_volume(const AtlasRect *r, const int *idx, int m,
                          const AvolLimits *lim, AvolCell *cells, int vol,
                          AvolInfo *vi) {
    int cur_x = 0, cur_y = 0, shelf = 0, shelves = 0;
    long used = 0;
    for (int k = 0; k < m; ++k) {
        int i = idx[k];
        if (cur_x + r[i].w > lim->page_w) {
            cur_y += shelf;
            cur_x = 0;
            shelf = 0;
        }
        if (shelf == 0) ++shelves;
        cells[i].volume = vol;
        cells[i].x = cur_x;
        cells[i].y = cur_y;
        cells[i].placed = 1;
        cur_x += r[i].w;
        if (r[i].h > shelf) shelf = r[i].h;
        used += (long)r[i].w * r[i].h;
    }
    vi->page_w = lim->page_w;
    vi->page_h = lim->page_h;
    vi->rects = m;
    vi->used_px = (int)used;
    vi->used_bytes = vol_bytes(lim, used);
    vi->shelves = shelves;
    vi->occupancy = (double)used /
                    ((double)lim->page_w * (double)lim->page_h);
}

static void fill_stats(const AtlasRect *rects, int n, const AvolLimits *lim,
                       const AvolBook *book, AvolStats *stats) {
    memset(stats, 0, sizeof *stats);
    stats->scale = lim->scale;
    stats->volumes = book->volume_count;
    stats->index_len = n + book->volume_count;
    stats->index_bytes = 16 + 12 * n + 20 * book->volume_count;
    for (int p = 0; p < book->volume_count; ++p) {
        stats->used_px += book->vols[p].used_px;
        stats->used_bytes += book->vols[p].used_bytes;
    }
    stats->capacity_px = (long)book->volume_count * lim->page_w * lim->page_h;
    stats->waste_px = stats->capacity_px - stats->used_px;
    stats->waste_bytes = vol_bytes(lim, stats->waste_px);
    stats->occupancy = stats->capacity_px > 0
                     ? (double)stats->used_px / (double)stats->capacity_px
                     : 0.0;
    for (int i = 0; i < n; ++i)
        stats->samples += (long)rects[i].w * rects[i].h *
                          lim->scale * lim->scale;
    stats->oob_index = -1;
}

int avol_layout(const AtlasRect *rects, int n, const int *vol_of,
                int volume_count, const AvolLimits *lim,
                AvolBook *book, AvolStats *stats, int *err_item) {
    if (err_item) *err_item = -1;
    if (!book) return AVOL_ERR_ARG;
    book_reset(book);
    if (!rects || !vol_of || !limits_ok(lim) || n <= 0 || volume_count <= 0)
        return AVOL_ERR_ARG;

    book->cells = (AvolCell*)calloc((size_t)n, sizeof *book->cells);
    book->vols = (AvolInfo*)calloc((size_t)volume_count, sizeof *book->vols);
    int *count = (int*)calloc((size_t)volume_count, sizeof *count);
    int *fill = (int*)calloc((size_t)volume_count, sizeof *fill);
    int *idx = (int*)malloc((size_t)n * sizeof *idx);
    if (!book->cells || !book->vols || !count || !fill || !idx) {
        free(count); free(fill); free(idx);
        avol_free(book);
        return AVOL_ERR_NOMEM;
    }

    for (int i = 0; i < n; ++i) {
        if (vol_of[i] < 0 || vol_of[i] >= volume_count) {
            if (err_item) *err_item = i;
            free(count); free(fill); free(idx);
            avol_free(book);
            return AVOL_ERR_OOB;
        }
        ++count[vol_of[i]];
    }
    int base = 0;
    for (int p = 0; p < volume_count; ++p) {
        int c = count[p];
        count[p] = base;
        base += c;
    }
    for (int i = 0; i < n; ++i) { /* 册内保持原始序号升序 */
        int p = vol_of[i];
        idx[count[p] + fill[p]++] = i;
    }
    for (int p = 0; p < volume_count; ++p)
        layout_volume(rects, idx + count[p], fill[p], lim, book->cells, p,
                      &book->vols[p]);

    book->volume_count = volume_count;
    book->n = n;
    book->lim = *lim;

    /* 回填完立即复核：页内不越界，否则报矩形序号。 */
    for (int i = 0; i < n; ++i) {
        const AvolCell *c = &book->cells[i];
        if (c->x + rects[i].w > lim->page_w ||
            c->y + rects[i].h > lim->page_h) {
            if (err_item) *err_item = i;
            free(count); free(fill); free(idx);
            avol_free(book);
            return AVOL_ERR_OOB;
        }
    }
    if (stats) fill_stats(rects, n, lim, book, stats);

    free(count);
    free(fill);
    free(idx);
    return AVOL_OK;
}

/* ============================ 老路径 ============================ */

static int pack_legacy(const AtlasRect *rects, int n, const AvolLimits *lim,
                       AvolBook *book, AvolStats *stats, int *err_item) {
    if (n <= 0) return AVOL_ERR_ARG;
    for (int i = 0; i < n; ++i)
        if (rects[i].w <= 0 || rects[i].h <= 0 || rects[i].w > lim->page_w) {
            if (err_item) *err_item = i;
            return AVOL_ERR_TOO_BIG;
        }

    AtlasPos *pos = (AtlasPos*)calloc((size_t)n, sizeof *pos);
    if (!pos) return AVOL_ERR_NOMEM;
    AtlasStats old;
    int rc = atlas_pack(rects, n, lim->page_w, pos, &old);
    if (rc != 0) {
        free(pos);
        return rc == -1 ? AVOL_ERR_ARG : AVOL_ERR_TOO_BIG;
    }

    book->cells = (AvolCell*)calloc((size_t)n, sizeof *book->cells);
    book->vols = (AvolInfo*)calloc(1, sizeof *book->vols);
    if (!book->cells || !book->vols) {
        free(pos);
        avol_free(book);
        return AVOL_ERR_NOMEM;
    }
    for (int i = 0; i < n; ++i) {
        book->cells[i].volume = 0;
        book->cells[i].x = pos[i].x;
        book->cells[i].y = pos[i].y;
        book->cells[i].placed = pos[i].placed;
    }
    book->volume_count = 1;
    book->n = n;
    book->lim = *lim;

    AvolInfo *v = &book->vols[0];
    v->page_w = lim->page_w;
    v->page_h = lim->page_h;
    v->rects = n;
    v->used_px = old.used_px;
    v->used_bytes = vol_bytes(lim, old.used_px);
    v->shelves = old.shelves;
    v->occupancy = (double)old.used_px /
                   ((double)lim->page_w * (double)lim->page_h);
    free(pos);

    if (stats) fill_stats(rects, n, lim, book, stats);
    return AVOL_OK;
}

int avol_pack(const AtlasRect *rects, int n, const AvolLimits *lim,
              AvolBook *book, AvolStats *stats, int *err_item, int enable_new) {
    if (err_item) *err_item = -1;
    if (!book) return AVOL_ERR_ARG;
    book_reset(book);
    if (!rects || !limits_ok(lim)) return AVOL_ERR_ARG;

    if (!enable_new) return pack_legacy(rects, n, lim, book, stats, err_item);

    int *vol_of = (int*)calloc((size_t)n, sizeof *vol_of);
    if (!vol_of) return AVOL_ERR_NOMEM;
    int volume_count = 0;
    int rc = avol_split(rects, n, lim, vol_of, &volume_count, err_item);
    if (rc == AVOL_OK)
        rc = avol_layout(rects, n, vol_of, volume_count, lim, book, stats,
                         err_item);
    free(vol_of);
    return rc;
}

/* ============================ 查询与校验 ============================ */

int avol_lookup(const AvolBook *book, int rect_index, AvolCell *out,
                int *bad_index) {
    if (bad_index) *bad_index = -1;
    if (!book || !out || !book->cells) return AVOL_ERR_ARG;
    if (rect_index < 0 || rect_index >= book->n) {
        if (bad_index) *bad_index = rect_index;
        return AVOL_ERR_OOB;
    }
    *out = book->cells[rect_index];
    return AVOL_OK;
}

int avol_texel(const AvolBook *book, const AtlasRect *rects, int rect_index,
               int tx, int ty, int *volume, long *byte_offset,
               int *bad_index) {
    if (bad_index) *bad_index = -1;
    if (!book || !rects || !book->cells || !volume || !byte_offset)
        return AVOL_ERR_ARG;
    if (rect_index < 0 || rect_index >= book->n) {
        if (bad_index) *bad_index = rect_index;
        return AVOL_ERR_OOB;
    }
    const AvolCell *c = &book->cells[rect_index];
    int w = rects[rect_index].w, h = rects[rect_index].h;
    int s = book->lim.scale, bpp = book->lim.bytes_per_px;
    if (c->volume < 0 || c->volume >= book->volume_count ||
        c->x < 0 || c->y < 0 ||
        tx < 0 || ty < 0 || tx >= w * s || ty >= h * s) {
        if (bad_index) *bad_index = rect_index;
        return AVOL_ERR_OOB;
    }
    long page_px_w = (long)book->vols[c->volume].page_w * s;
    long page_px_h = (long)book->vols[c->volume].page_h * s;
    long px = (long)c->x * s + tx;
    long py = (long)c->y * s + ty;
    if (px + 1 > page_px_w || py + 1 > page_px_h) {
        if (bad_index) *bad_index = rect_index;
        return AVOL_ERR_OOB;
    }
    *volume = c->volume;
    *byte_offset = (py * page_px_w + px) * bpp;
    return AVOL_OK;
}

int avol_validate(const AvolBook *book, const AtlasRect *rects, int *bad_index) {
    if (bad_index) *bad_index = -1;
    if (!book || !rects || !book->cells || !book->vols || book->volume_count <= 0)
        return AVOL_ERR_ARG;

    long *used = (long*)calloc((size_t)book->volume_count, sizeof *used);
    if (!used) return AVOL_ERR_NOMEM;
    int bad = -1;
    for (int i = 0; i < book->n; ++i) {
        const AvolCell *c = &book->cells[i];
        if (!c->placed || c->volume < 0 || c->volume >= book->volume_count ||
            c->x < 0 || c->y < 0 ||
            (long)c->x + rects[i].w > book->lim.page_w ||
            (long)c->y + rects[i].h > book->lim.page_h) {
            bad = i;
            break;
        }
        used[c->volume] += (long)rects[i].w * rects[i].h;
    }
    if (bad < 0) {
        for (int p = 0; p < book->volume_count; ++p) {
            if (used[p] != book->vols[p].used_px ||
                book->vols[p].used_bytes > book->lim.cap_bytes ||
                book->vols[p].used_px >
                    (long)book->vols[p].page_w * book->vols[p].page_h) {
                if (bad_index) *bad_index = -(p + 1); /* 册级问题：负的册号 */
                free(used);
                return AVOL_ERR_OOB;
            }
        }
    }
    free(used);
    if (bad >= 0) {
        if (bad_index) *bad_index = bad;
        return AVOL_ERR_OOB;
    }
    return AVOL_OK;
}

int avol_probe(const AvolBook *book, const AtlasRect *rects,
               long *samples, int *bad_index) {
    if (bad_index) *bad_index = -1;
    if (samples) *samples = 0;
    if (!book || !rects || !book->cells || !samples) return AVOL_ERR_ARG;
    long count = 0;
    for (int i = 0; i < book->n; ++i) {
        int w = rects[i].w, h = rects[i].h;
        if (w <= 0 || h <= 0) {
            if (bad_index) *bad_index = i;
            return AVOL_ERR_OOB;
        }
        for (int ty = 0; ty < h * book->lim.scale; ++ty)
            for (int tx = 0; tx < w * book->lim.scale; ++tx) {
                int vol;
                long off;
                int rc = avol_texel(book, rects, i, tx, ty, &vol, &off,
                                    bad_index);
                if (rc != AVOL_OK) return rc;
                ++count;
            }
    }
    *samples = count;
    return AVOL_OK;
}

int avol_export(const AvolBook *book, const AtlasRect *rects, FILE *out) {
    if (!book || !rects || !out || !book->cells || !book->vols ||
        book->volume_count <= 0 || book->n <= 0)
        return AVOL_ERR_ARG;

    AvolStats st;
    fill_stats(rects, book->n, &book->lim, book, &st);
    fprintf(out, "# atlas volume table\n");
    fprintf(out, "page_w=%d page_h=%d scale=%d cap_bytes=%ld volumes=%d rects=%d\n",
            book->lim.page_w, book->lim.page_h, book->lim.scale,
            book->lim.cap_bytes, book->volume_count, book->n);
    fprintf(out, "index_len=%d index_bytes=%d used_px=%ld used_bytes=%ld "
                 "capacity_px=%ld waste_px=%ld waste_bytes=%ld occupancy=%.4f\n",
            st.index_len, st.index_bytes, st.used_px, st.used_bytes,
            st.capacity_px, st.waste_px, st.waste_bytes, st.occupancy);
    fprintf(out, "# load order: volumes ascending 0..%d; "
                 "within a volume, rects ascending by rect index\n",
            book->volume_count - 1);
    for (int p = 0; p < book->volume_count; ++p) {
        const AvolInfo *v = &book->vols[p];
        fprintf(out, "volume %d: rects=%d used_px=%d used_bytes=%ld shelves=%d "
                     "occupancy=%.4f\n",
                p, v->rects, v->used_px, v->used_bytes, v->shelves,
                v->occupancy);
        for (int i = 0; i < book->n; ++i)
            if (book->cells[i].volume == p)
                fprintf(out, "  rect %d: %dx%d @ (%d,%d)\n",
                        i, rects[i].w, rects[i].h,
                        book->cells[i].x, book->cells[i].y);
    }
    return ferror(out) ? AVOL_ERR_ARG : AVOL_OK;
}

#endif /* ATLAS_USE_VOL */

/* 开关关闭时保持一个非空翻译单元（老路径完全不引用本模块）。 */
#if !ATLAS_USE_VOL
typedef int avol_translation_unit;
#endif
