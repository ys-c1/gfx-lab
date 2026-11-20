#define _POSIX_C_SOURCE 199309L
#include "atlas.h"
#include <stdio.h>
#include <time.h>

static unsigned rng_state = 12345u;
static unsigned next_rand(void) {
    rng_state = rng_state * 1103515245u + 12345u;
    return (rng_state >> 16) & 0x7fffu;
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static double total_occupancy(const AtlasBook *b) {
    long used = 0;
    for (int p = 0; p < b->page_count; ++p) used += b->pages[p].used_px;
    return (double)used / ((double)b->page_count * b->page_width * b->page_height);
}

static void bench(const AtlasRect *r, int n, int width) {
    AtlasBook book, greedy;
    int err = -1;
    double t0 = now_ms();
    int rc = atlas_book_pack(r, n, width, width, &book, &err);
    double t1 = now_ms();
    if (rc != 0) { printf("width=%d book pack failed rc=%d err_item=%d\n", width, rc, err); return; }
    int gc = atlas_book_pack_greedy(r, n, width, width, &greedy, &err);
    double t2 = now_ms();
    printf("width=%d pages=%d occupancy=%.4f time=%.3fms (two-phase)\n",
           width, book.page_count, total_occupancy(&book), t1 - t0);
    if (gc == 0) {
        printf("width=%d pages=%d occupancy=%.4f time=%.3fms (greedy)\n",
               width, greedy.page_count, total_occupancy(&greedy), t2 - t1);
        atlas_book_free(&greedy);
    }
    atlas_book_free(&book);
}

int main(void) {
    /* 小例子：64x64 页，演示切页与页表导出 */
    AtlasRect small[12];
    for (int i = 0; i < 12; ++i) { small[i].w = 8 + i * 4; small[i].h = 8 + (i % 3) * 8; }
    AtlasBook book;
    int err = -1;
    if (atlas_book_pack(small, 12, 64, 64, &book, &err) != 0) return 1;
    printf("small: pages=%d\n", book.page_count);
    for (int p = 0; p < book.page_count; ++p)
        printf("  page %d: shelves=%d occupancy=%.4f\n",
               p, book.pages[p].shelves, book.pages[p].occupancy);
    printf("load order: pages 0..%d ascending; within a page, rects by index\n",
           book.page_count - 1);
    for (int i = 0; i < 12; ++i)
        printf("  rect %d -> page %d @ (%d,%d)\n", i, book.cells[i].page,
               book.cells[i].x, book.cells[i].y);
    FILE *f = fopen("demo.txt", "w");
    if (!f) return 1;
    if (atlas_book_export(&book, small, f) != 0) { fclose(f); return 1; }
    fclose(f);
    atlas_book_free(&book);

    /* 基准：4000 个矩形，256 与 1024 两档页宽 */
    enum { N = 4000 };
    static AtlasRect r[N];
    for (int i = 0; i < N; ++i) {
        r[i].w = 8 + (int)(next_rand() % 56);
        r[i].h = 8 + (int)(next_rand() % 56);
    }
    bench(r, N, 256);
    bench(r, N, 1024);
    return 0;
}
