#include "atlas_ref.h"

#if !ATLAS_USE_REFCOUNT
#include <stdio.h>
int main(void) {
    printf("atlas_ref demo disabled (build with ATLAS_USE_REFCOUNT=1)\n");
    return 0;
}
#else

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 12 tiles, deliberately not all squares so block padding is non-zero. */
static const int TILE_W[12] = {8, 8, 16, 8, 8, 10, 8, 8, 16, 8, 8, 8};
static const int TILE_H[12] = {8, 8, 8, 16, 8, 6, 8, 8, 8, 16, 8, 8};

static void fill(unsigned char *p, size_t n) {
    for (size_t i = 0; i < n; ++i) p[i] = (unsigned char)(i * 7u + 3u);
}

/* Hand derivation, shared by both scales:
 *   block side (points) = next pow2 >= max(w,h)*scale, floor 8; cells = side/8
 *   used_points   = sum(cells^2 * 64)
 *   requested     = sum(w*h) * scale^2
 *   padding/error = used_points - requested
 *   budget        = max_pages * page_points^2
 *   pages         = distinct pages touched by the blocks
 *   bytes         = points * 3 * scale^2 */
typedef struct {
    int side[12], cells[12];
    int used_points, requested, padding, budget_points, pages;
} Paper;

static Paper paper(int scale, int page_points, int max_pages) {
    Paper p;
    memset(&p, 0, sizeof(p));
    for (int i = 0; i < 12; ++i) {
        int need = (TILE_W[i] > TILE_H[i] ? TILE_W[i] : TILE_H[i]) * scale;
        int side = 8;
        while (side < need) side <<= 1;
        p.side[i] = side;
        p.cells[i] = side / 8;
        p.used_points += p.cells[i] * p.cells[i] * 64;
        p.requested += TILE_W[i] * TILE_H[i] * scale * scale;
    }
    p.padding = p.used_points - p.requested;
    p.budget_points = max_pages * page_points * page_points;
    /* first-fit row-major; count pages actually used */
    int cells_per_page = page_points / 8;
    unsigned char occ[ATLAS_REF_MAX_PAGES][32][32];
    memset(occ, 0, sizeof(occ));
    for (int i = 0; i < 12; ++i) {
        int n = p.cells[i], placed = 0;
        for (int pg = 0; pg < max_pages && !placed; ++pg)
            for (int cy = 0; cy + n <= cells_per_page && !placed; ++cy)
                for (int cx = 0; cx + n <= cells_per_page && !placed; ++cx) {
                    int free_cell = 1;
                    for (int dy = 0; dy < n && free_cell; ++dy)
                        for (int dx = 0; dx < n; ++dx)
                            if (occ[pg][cx + dx][cy + dy]) { free_cell = 0; break; }
                    if (!free_cell) continue;
                    for (int dy = 0; dy < n; ++dy)
                        for (int dx = 0; dx < n; ++dx)
                            occ[pg][cx + dx][cy + dy] = 1;
                    if (pg + 1 > p.pages) p.pages = pg + 1;
                    placed = 1;
                }
    }
    return p;
}

static void report(const char *tag, const AtlasRefStats *s) {
    printf("[%s] scale=%d page=%dpx(%dB) budget=%dpt/%dB used=%dpt/%dB free=%dpt/%dB\n",
           tag, s->scale, s->page_px, s->page_bytes,
           s->budget_points, s->budget_bytes,
           s->used_points, s->used_bytes,
           s->free_points, s->free_bytes);
    printf("[%s] requested=%dpt padding=%dpt stat_error=%dpt pages=%d page_total=%dpt slack=%dpt total=%dB\n",
           tag, s->requested_points, s->padding_points, s->stat_error,
           s->pages, s->page_total_points, s->slack_points, s->total_bytes);
    printf("[%s] index_len=%d cap=%d live=%d idle=%d refs=%d balance=%dpt/%dB\n",
           tag, s->index_len, s->index_cap, s->live_blocks, s->idle_blocks,
           s->live_refs, s->balance_points, s->balance_bytes);
    printf("[%s] samples=%d (hits=%d misses=%d) acquires=%d reacquires=%d refs_total=%d\n",
           tag, s->samples, s->sample_hits, s->sample_misses,
           s->acquired, s->reacquired, s->refs_total);
    printf("[%s] released=%d released_to_zero=%d oob=%d uploads=%d\n",
           tag, s->released, s->release_zeroed, s->oob_checks, s->uploads);
    printf("[%s] resets=%d marked_idle=%d leaked=%d index_cleared=%d\n",
           tag, s->resets, s->reset_marked, s->reset_leaked, s->reset_index_cleared);
}

static int check(const char *what, long got, long want) {
    int pass = got == want;
    printf("  %-34s got=%-8ld paper=%-8ld %s\n", what, got, want, pass ? "ok" : "MISMATCH");
    return pass;
}

static int files_equal(const char *pa, const char *pb) {
    FILE *a = fopen(pa, "rb");
    FILE *b = fopen(pb, "rb");
    int same = a && b;
    if (same) {
        int ca, cb;
        do { ca = fgetc(a); cb = fgetc(b); if (ca != cb) same = 0; }
        while (ca != EOF && same);
    }
    if (a) fclose(a);
    if (b) fclose(b);
    return same;
}

static int run(int scale, int page_points, int max_pages) {
    AtlasRefConfig cfg = {page_points, max_pages, 12};
    AtlasError err;
    AtlasRefStats s;
    int pass = 1;
    char pre_path[64], post_path[64], fresh_path[64];

    Paper pp = paper(scale, page_points, max_pages);
    snprintf(pre_path, sizeof(pre_path), "/tmp/atlas_ref_demo_%dx_pre.txt", scale);
    snprintf(post_path, sizeof(post_path), "/tmp/atlas_ref_demo_%dx_post.txt", scale);
    snprintf(fresh_path, sizeof(fresh_path), "/tmp/atlas_ref_demo_%dx_fresh.txt", scale);

    printf("\n=== scale %d (%dx), page %d points, %d pages ===\n",
           scale, scale, page_points, max_pages);

    AtlasRefAtlas *a = atlas_ref_create(&cfg, scale, &err);
    if (!a) { printf("create failed %d\n", (int)err.code); return 0; }

    AtlasRefPos pos[12];
    for (int i = 0; i < 12; ++i) {
        int rc = atlas_ref_acquire(a, i, TILE_W[i], TILE_H[i], &pos[i], &err);
        if (rc != ATLAS_OK) {
            printf("acquire %d failed %d (page=%d x=%d y=%d limit=%d)\n",
                   i, (int)err.code, err.page, err.x, err.y, err.detail);
            pass = 0;
        }
    }
    atlas_ref_acquire(a, 3, TILE_W[3], TILE_H[3], &pos[3], &err); /* refs 2 */
    atlas_ref_acquire(a, 3, TILE_W[3], TILE_H[3], &pos[3], &err); /* refs 3 */
    atlas_ref_release(a, 3, &err);                                /* refs 2 */
    atlas_ref_release(a, 5, &err);                                /* idle */
    atlas_ref_release(a, 7, &err);                                /* idle */

    /* statistical taps: 16 points of a 4x4 grid on page 0 */
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            atlas_ref_probe(a, 0, x * 8 + 4, y * 8 + 4, &err);

    /* four out-of-range taps, each of which must be detected and numbered */
    AtlasError oob[4];
    atlas_ref_probe(a, 0, page_points, 4, &oob[0]);   /* x == page width */
    atlas_ref_probe(a, max_pages, 4, 4, &oob[1]);     /* page past the budget */
    atlas_ref_probe(a, 0, -1, 4, &oob[2]);            /* negative x */
    atlas_ref_probe(a, 0, 4, page_points + 3, &oob[3]); /* y past the bottom */

    for (int i = 0; i < 12; ++i) atlas_ref_sample(a, i, &pos[i], &err);

    size_t bytes = (size_t)3 * page_points * scale * page_points * scale;
    unsigned char *rgb = (unsigned char*)malloc(bytes);
    fill(rgb, bytes);
    atlas_ref_upload(a, 0, rgb, bytes, &err);

    if (atlas_ref_validate(a, &err) != ATLAS_OK) {
        printf("validate failed %d (%s) index=%d page=%d x=%d y=%d\n",
               (int)err.code, err.what ? err.what : "?", err.index, err.page, err.x, err.y);
        pass = 0;
    }
    atlas_ref_export(a, pre_path, &err);
    atlas_ref_stats(a, &s);
    report("pre", &s);

    for (int i = 0; i < 4; ++i) {
        printf("[pre] oob[%d] code=%d rect=%d page=%d x=%d y=%d limit=%d (%s)\n",
               i, (int)oob[i].code, oob[i].index, oob[i].page, oob[i].x, oob[i].y,
               oob[i].detail, oob[i].what ? oob[i].what : "?");
        pass &= oob[i].code == ATLAS_ERR_OOB;
    }

    printf("[pre] against the hand derivation:\n");
    pass &= check("used_points", s.used_points, pp.used_points);
    pass &= check("requested_points", s.requested_points, pp.requested);
    pass &= check("padding_points", s.padding_points, pp.padding);
    pass &= check("stat_error", s.stat_error, pp.padding);
    pass &= check("free_points", s.free_points, pp.budget_points - pp.used_points);
    pass &= check("budget_points", s.budget_points, pp.budget_points);
    pass &= check("pages", s.pages, pp.pages);
    pass &= check("used_bytes", s.used_bytes,
                  (long)pp.used_points * 3 * scale * scale);
    pass &= check("free_bytes", s.free_bytes,
                  (long)(pp.budget_points - pp.used_points) * 3 * scale * scale);
    pass &= check("page_total_points", s.page_total_points,
                  (long)pp.pages * page_points * page_points);
    pass &= check("slack_points", s.slack_points,
                  (long)pp.pages * page_points * page_points - pp.used_points);
    pass &= check("total_bytes", s.total_bytes,
                  (long)pp.pages * page_points * scale * page_points * scale * 3);
    pass &= check("balance_points", s.balance_points, 0);
    pass &= check("balance_bytes", s.balance_bytes, 0);
    pass &= check("index_len", s.index_len, 12);
    pass &= check("live_blocks", s.live_blocks, 10);
    pass &= check("idle_blocks", s.idle_blocks, 2);
    pass &= check("live_refs", s.live_refs, 11);
    pass &= check("oob_checks", s.oob_checks, 4);
    pass &= check("samples", s.samples, 12);

    /* Drop one reference from every block (rect 3 had 2, so it is left with
     * one dangling on purpose: phase 1 must count it as a leak, not free it).
     * Rects 5 and 7 already went to zero above, so they get no release here. */
    for (int i = 0; i < 12; ++i) {
        if (i == 5 || i == 7) continue;
        atlas_ref_release(a, i, &err);
    }

    AtlasRefStats post;
    atlas_ref_reset(a, &post, &err);
    report("post", &post);
    atlas_ref_export(a, post_path, &err);

    size_t got = 0;
    const unsigned char *pg = atlas_ref_page_pixels(a, 0, &got, &err);
    int zero = 1;
    for (size_t i = 0; i < got; ++i) if (pg && pg[i]) { zero = 0; break; }
    printf("[post] page0 bytes=%zu zeroed=%s\n", got, zero ? "yes" : "no");
    free(rgb);
    pass &= zero && got == bytes;

    printf("[post] against the hand derivation:\n");
    pass &= check("index_len", post.index_len, 0);
    pass &= check("used_points", post.used_points, 0);
    pass &= check("free_points", post.free_points, pp.budget_points);
    pass &= check("pages", post.pages, 0);
    pass &= check("live_blocks", post.live_blocks, 0);
    pass &= check("idle_blocks", post.idle_blocks, 0);
    pass &= check("reset_marked", post.reset_marked, 11);
    pass &= check("reset_leaked", post.reset_leaked, 1);
    pass &= check("reset_index_cleared", post.reset_index_cleared, 12);
    pass &= check("resets", post.resets, 1);
    pass &= check("live_refs", post.live_refs, 0);
    pass &= check("balance_points", post.balance_points, 0);

    AtlasRefAtlas *fresh = atlas_ref_create(&cfg, scale, &err);
    atlas_ref_export(fresh, fresh_path, &err);
    int same = files_equal(post_path, fresh_path);
    printf("[post] export matches a fresh atlas byte-for-byte: %s\n", same ? "yes" : "no");
    pass &= same;
    atlas_ref_destroy(fresh);

    printf("[pre]  export -> %s\n", pre_path);
    printf("[post] export -> %s\n", post_path);
    atlas_ref_destroy(a);
    printf("=== scale %d: %s ===\n", scale, pass ? "all paper checks match" : "MISMATCH");
    return pass;
}

int main(void) {
    int ok = run(1, 32, 8);
    ok &= run(2, 32, 8);
    return ok ? 0 : 1;
}

#endif
