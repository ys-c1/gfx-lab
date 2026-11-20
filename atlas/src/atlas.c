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

/* ---- 多页图集 ---- */

static void book_reset(AtlasBook *book) {
    book->cells = NULL; book->pages = NULL;
    book->page_count = 0; book->n = 0;
    book->page_width = 0; book->page_height = 0;
}

void atlas_book_free(AtlasBook *book) {
    if (!book) return;
    free(book->cells);
    free(book->pages);
    book_reset(book);
}

static int book_check(const AtlasRect *rects, int n, int pw, int ph,
                      AtlasBook *book, int *err_item) {
    if (err_item) *err_item = -1;
    if (!book) return -1;
    book_reset(book);
    if (!rects || pw <= 0 || ph <= 0) return -1;
    if (n <= 0) return -3; /* 页数为零 */
    return 0;
}

/* 阶段一：按页上限切页。page_of[i] 得到页号，*page_count 得页数。
 * 返回 >=0 表示该序号的矩形大于页。 */
static int split_pages(const AtlasRect *r, int n, int pw, int ph,
                       int *page_of, int *page_count) {
    int page = 0, cur_x = 0, cur_y = 0, shelf_h = 0;
    for (int i = 0; i < n; ++i) {
        if (r[i].w <= 0 || r[i].h <= 0 || r[i].w > pw || r[i].h > ph) return i;
        if (cur_x + r[i].w > pw) { cur_y += shelf_h; cur_x = 0; shelf_h = 0; }
        if (cur_y + r[i].h > ph) { ++page; cur_x = 0; cur_y = 0; shelf_h = 0; }
        page_of[i] = page;
        cur_x += r[i].w;
        if (r[i].h > shelf_h) shelf_h = r[i].h;
    }
    *page_count = page + 1;
    return -1;
}

/* 阶段二：逐页回填页内偏移（与 atlas_pack 同一货架规则，保证单页时布局照旧）。 */
static void layout_page(const AtlasRect *r, const int *idx, int m, int pw, int ph,
                        AtlasCell *cells, int page_no, AtlasPageInfo *pi) {
    int cur_x = 0, cur_y = 0, shelf_h = 0, used = 0, shelves = 1;
    for (int k = 0; k < m; ++k) {
        int i = idx[k];
        if (cur_x + r[i].w > pw) { cur_y += shelf_h; cur_x = 0; shelf_h = 0; ++shelves; }
        cells[i].page = page_no;
        cells[i].x = cur_x;
        cells[i].y = cur_y;
        cur_x += r[i].w;
        if (r[i].h > shelf_h) shelf_h = r[i].h;
        used += r[i].w * r[i].h;
    }
    pi->width = pw; pi->height = ph;
    pi->used_px = used;
    pi->shelves = shelves;
    pi->occupancy = (double)used / ((double)pw * (double)ph);
}

int atlas_book_pack(const AtlasRect *rects, int n, int page_w, int page_h,
                    AtlasBook *book, int *err_item) {
    int rc = book_check(rects, n, page_w, page_h, book, err_item);
    if (rc) return rc;

    int *page_of = malloc((size_t)n * sizeof *page_of);
    if (!page_of) return -4;
    int page_count = 0;
    int bad = split_pages(rects, n, page_w, page_h, page_of, &page_count);
    if (bad >= 0) {
        if (err_item) *err_item = bad;
        free(page_of);
        return -2;
    }

    book->cells = calloc((size_t)n, sizeof *book->cells);
    book->pages = calloc((size_t)page_count, sizeof *book->pages);
    int *count = calloc((size_t)page_count, sizeof *count);
    int *fill = calloc((size_t)page_count, sizeof *fill);
    int *idx = malloc((size_t)n * sizeof *idx);
    if (!book->cells || !book->pages || !count || !fill || !idx) {
        free(page_of); free(count); free(fill); free(idx);
        atlas_book_free(book);
        return -4;
    }
    for (int i = 0; i < n; ++i) ++count[page_of[i]];
    int base = 0;
    for (int p = 0; p < page_count; ++p) { int c = count[p]; count[p] = base; base += c; }
    for (int i = 0; i < n; ++i) {           /* 页内保持原始序号升序 */
        int p = page_of[i];
        idx[count[p] + fill[p]++] = i;
    }
    for (int p = 0; p < page_count; ++p)
        layout_page(rects, idx + count[p], fill[p], page_w, page_h,
                    book->cells, p, &book->pages[p]);

    book->page_count = page_count;
    book->n = n;
    book->page_width = page_w;
    book->page_height = page_h;
    free(page_of); free(count); free(fill); free(idx);
    return 0;
}

/* ---- 贪心切页（对照）：高降序首次适配 ---- */

static const AtlasRect *g_sort_rects;
static int cmp_rect_desc(const void *a, const void *b) {
    int ia = *(const int *)a, ib = *(const int *)b;
    const AtlasRect *ra = &g_sort_rects[ia], *rb = &g_sort_rects[ib];
    if (ra->h != rb->h) return rb->h - ra->h;
    if (ra->w != rb->w) return rb->w - ra->w;
    return ia - ib;
}

int atlas_book_pack_greedy(const AtlasRect *rects, int n, int page_w, int page_h,
                           AtlasBook *book, int *err_item) {
    int rc = book_check(rects, n, page_w, page_h, book, err_item);
    if (rc) return rc;
    for (int i = 0; i < n; ++i) {
        if (rects[i].w <= 0 || rects[i].h <= 0 ||
            rects[i].w > page_w || rects[i].h > page_h) {
            if (err_item) *err_item = i;
            return -2;
        }
    }

    int *order = malloc((size_t)n * sizeof *order);
    book->cells = calloc((size_t)n, sizeof *book->cells);
    int cap = 8, np = 0;
    int *cx = malloc((size_t)cap * sizeof *cx);
    int *cy = malloc((size_t)cap * sizeof *cy);
    int *sh = malloc((size_t)cap * sizeof *sh);
    int *sc = malloc((size_t)cap * sizeof *sc);
    AtlasPageInfo *pi = malloc((size_t)cap * sizeof *pi);
    if (!order || !book->cells || !cx || !cy || !sh || !sc || !pi) {
        free(order); free(cx); free(cy); free(sh); free(sc); free(pi);
        atlas_book_free(book);
        return -4;
    }
    for (int i = 0; i < n; ++i) order[i] = i;
    g_sort_rects = rects;
    qsort(order, (size_t)n, sizeof *order, cmp_rect_desc);

    for (int k = 0; k < n; ++k) {
        int i = order[k], w = rects[i].w, h = rects[i].h, placed = 0;
        for (int p = 0; p < np && !placed; ++p) {
            if (cx[p] + w <= page_w) {                 /* 当前货架 */
                book->cells[i].page = p;
                book->cells[i].x = cx[p];
                book->cells[i].y = cy[p];
                cx[p] += w;
                if (h > sh[p]) sh[p] = h;
                placed = 1;
            } else if (cy[p] + sh[p] + h <= page_h) {  /* 新开一层货架 */
                cy[p] += sh[p];
                cx[p] = w;
                sh[p] = h;
                ++sc[p];
                book->cells[i].page = p;
                book->cells[i].x = 0;
                book->cells[i].y = cy[p];
                placed = 1;
            }
            if (placed) pi[p].used_px += w * h;
        }
        if (!placed) {                                 /* 开新页 */
            if (np == cap) {
                int new_cap = cap * 2, ok = 1;
                int *p;
                AtlasPageInfo *q;
                p = realloc(cx, (size_t)new_cap * sizeof *cx); if (p) cx = p; else ok = 0;
                p = realloc(cy, (size_t)new_cap * sizeof *cy); if (p) cy = p; else ok = 0;
                p = realloc(sh, (size_t)new_cap * sizeof *sh); if (p) sh = p; else ok = 0;
                p = realloc(sc, (size_t)new_cap * sizeof *sc); if (p) sc = p; else ok = 0;
                q = realloc(pi, (size_t)new_cap * sizeof *pi); if (q) pi = q; else ok = 0;
                if (!ok) {
                    free(order); free(cx); free(cy); free(sh); free(sc); free(pi);
                    atlas_book_free(book);
                    return -4;
                }
                cap = new_cap;
            }
            cx[np] = w; cy[np] = 0; sh[np] = h; sc[np] = 1;
            pi[np].width = page_w; pi[np].height = page_h;
            pi[np].used_px = w * h;
            pi[np].shelves = 1;
            book->cells[i].page = np;
            book->cells[i].x = 0;
            book->cells[i].y = 0;
            ++np;
        }
    }
    for (int p = 0; p < np; ++p) {
        pi[p].shelves = sc[p];
        pi[p].occupancy = (double)pi[p].used_px / ((double)page_w * (double)page_h);
    }
    book->pages = pi;
    book->page_count = np;
    book->n = n;
    book->page_width = page_w;
    book->page_height = page_h;
    free(order); free(cx); free(cy); free(sh); free(sc);
    return 0;
}

int atlas_book_export(const AtlasBook *book, const AtlasRect *rects, FILE *out) {
    if (!book || !rects || !out || !book->cells || !book->pages ||
        book->page_count <= 0 || book->n <= 0) return -1;
    fprintf(out, "# atlas page table\n");
    fprintf(out, "page_width=%d page_height=%d pages=%d rects=%d\n",
            book->page_width, book->page_height, book->page_count, book->n);
    fprintf(out, "# load order: pages ascending 0..%d; within a page, rects ascending by rect index\n",
            book->page_count - 1);
    for (int p = 0; p < book->page_count; ++p) {
        const AtlasPageInfo *pi = &book->pages[p];
        fprintf(out, "page %d: used_px=%d shelves=%d occupancy=%.4f\n",
                p, pi->used_px, pi->shelves, pi->occupancy);
        for (int i = 0; i < book->n; ++i) {
            if (book->cells[i].page == p) {
                fprintf(out, "  rect %d: %dx%d @ (%d,%d)\n",
                        i, rects[i].w, rects[i].h,
                        book->cells[i].x, book->cells[i].y);
            }
        }
    }
    return ferror(out) ? -1 : 0;
}
