#include "atlas_ref.h"

#if !ATLAS_USE_REFCOUNT
#include <stdio.h>
int main(void) {
    printf("atlas_ref disabled (build with ATLAS_USE_REFCOUNT=1)\n");
    return 0;
}
#else

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

/* ---- independent paper model of the allocator ----------------------------
 * page = 16 points, cell = 8 points -> 2x2 cells per page.
 * First-fit, row-major inside a page (cy outer, cx inner), exactly the order
 * the module scans.  A request whose scaled side is <= 8 points takes one
 * cell; anything up to 16 points takes the whole 2x2 page.
 * Point layout never depends on `scale`; only the byte totals do. */

#define PAPER_CELLS 2
#define PAPER_PAGES 2
#define PAPER_CELL_POINTS 8

typedef struct { int rect, page, cx, cy, cells; } PaperBlock;

static PaperBlock paper[PAPER_PAGES][PAPER_CELLS][PAPER_CELLS];
static int paper_occ[PAPER_PAGES][PAPER_CELLS][PAPER_CELLS];

static void paper_reset(void) {
    memset(paper, 0, sizeof(paper));
    memset(paper_occ, 0, sizeof(paper_occ));
}

static int paper_alloc(int rect, int cells) {
    for (int p = 0; p < PAPER_PAGES; ++p) {
        for (int cy = 0; cy + cells <= PAPER_CELLS; ++cy) {
            for (int cx = 0; cx + cells <= PAPER_CELLS; ++cx) {
                int free_cell = 1;
                for (int dy = 0; dy < cells && free_cell; ++dy)
                    for (int dx = 0; dx < cells; ++dx)
                        if (paper_occ[p][cx + dx][cy + dy]) { free_cell = 0; break; }
                if (!free_cell) continue;
                for (int dy = 0; dy < cells; ++dy)
                    for (int dx = 0; dx < cells; ++dx)
                        paper_occ[p][cx + dx][cy + dy] = 1;
                paper[p][cx][cy] = (PaperBlock){rect, p, cx, cy, cells};
                return p;
            }
        }
    }
    return -1;
}

/* Hand-derived facts for the paper model with 8x8 requests at a given scale:
 *   scale 1 -> side 8 points -> 1 cell  -> cells are (0,0) then (1,0)
 *   scale 2 -> side 16 points -> 2 cells (the whole page) -> rect 0 fills
 *   page 0 and rect 1 lands on page 1 at (0,0).
 * The block's footprint in points is the same either way (128 points for two
 * blocks when they share a page); at scale 2 they occupy two whole pages. */

static int paper_block_points(int cells) {
    return cells * cells * PAPER_CELL_POINTS * PAPER_CELL_POINTS;
}

/* Look up where the paper model put a rect; -1 if absent. */
static int paper_find(int rect, int *page, int *px, int *py) {
    for (int p = 0; p < PAPER_PAGES; ++p)
        for (int cy = 0; cy < PAPER_CELLS; ++cy)
            for (int cx = 0; cx < PAPER_CELLS; ++cx)
                if (paper_occ[p][cx][cy] && paper[p][cx][cy].rect == rect) {
                    if (page) *page = p;
                    if (px) *px = cx * PAPER_CELL_POINTS;
                    if (py) *py = cy * PAPER_CELL_POINTS;
                    return 0;
                }
    return -1;
}

static int paper_cells_for(int w, int h, int scale) {
    int side = (w > h ? w : h) * scale;
    return side <= PAPER_CELL_POINTS ? 1 : 2;
}

/* ---- endian probe ---- */

static int is_le(void) {
    unsigned int v = 1;
    unsigned char b[4];
    memcpy(b, &v, 4);
    return b[0] == 1;
}

static void fill_pattern(unsigned char *p, size_t n) {
    for (size_t i = 0; i < n; ++i) p[i] = (unsigned char)(i * 7u + 3u);
}

static int all_zero(const unsigned char *p, size_t n) {
    for (size_t i = 0; i < n; ++i) if (p[i]) return 0;
    return 1;
}

static unsigned long djb2(const char *s) {
    unsigned long h = 5381;
    while (*s) h = h * 33u + (unsigned char)*s++;
    return h;
}

static int read_file(const char *path, char *buf, size_t cap) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = '\0';
    return (int)n;
}

/* ---- scenario per scale ---- */

typedef struct {
    int scale;
    int page_px;
    int page_bytes;
    int budget_points, used_points, free_points;
    int budget_bytes, used_bytes, free_bytes;
    int requested_points, padding_points;
} ScaleFacts;

static void run_scale(int scale) {
    AtlasRefConfig cfg = {16, 2, 16};
    AtlasError err;
    AtlasRefStats s;
    char buf[1024];

    paper_reset();
    AtlasRefAtlas *a = atlas_ref_create(&cfg, scale, &err);
    CK(a != NULL);
    if (!a) return;
    CK(err.code == ATLAS_OK);

    CK(atlas_ref_stats(a, &s) == ATLAS_OK);
    ScaleFacts f = {
        scale,
        16 * scale,
        3 * 16 * scale * 16 * scale,
        2 * 16 * 16,
        0, 2 * 16 * 16,
        2 * 16 * 16 * 3 * scale * scale,
        0, 2 * 16 * 16 * 3 * scale * scale,
        0, 0
    };
    CK(s.scale == scale && s.page_points == 16);
    CK(s.page_px == f.page_px && s.page_bytes == f.page_bytes);
    CK(s.budget_points == f.budget_points);
    CK(s.used_points == 0 && s.free_points == f.free_points);
    CK(s.budget_bytes == f.budget_bytes && s.used_bytes == 0 && s.free_bytes == f.free_bytes);
    CK(s.index_len == 0 && s.index_cap == 16 && s.live_blocks == 0 && s.idle_blocks == 0);
    CK(s.balance_points == 0 && s.balance_bytes == 0);

    /* two acquires of rect 0, one of rect 1 (fits the same page) */
    AtlasRefPos p0, p1;
    CK(atlas_ref_acquire(a, 0, 8, 8, &p0, &err) == ATLAS_OK);
    CK(p0.page == 0 && p0.x == 0 && p0.y == 0 && p0.block_id == 0);
    CK(atlas_ref_acquire(a, 1, 8, 8, &p1, &err) == ATLAS_OK);
    CK(p1.block_id == 1);
    CK(atlas_ref_acquire(a, 0, 8, 8, &p0, &err) == ATLAS_OK);

    /* -------- hand-derived expectations, per scale ------------------------
     * scale 1: 8x8 request -> 8-point side -> one 8-point cell.
     *   rect 0 -> page 0 cell (0,0) at (0,0); rect 1 -> page 0 cell (1,0) at (8,0)
     *   used = 2 cells * 64 points = 128 points (== requested, no padding)
     *   refs  = 2 (rect 0) + 1 (rect 1) = 3, index_len = 2, pages = 1
     * scale 2: 8x8 request -> 16-point side -> whole 2x2 page = 256 points.
     *   rect 0 -> page 0 at (0,0); rect 1 -> page 1 at (0,0)
     *   used = 512 points, free = 0; requested = 2 * 8*8 * 2*2 = 512
     *   refs = 3, index_len = 2, pages = 2, slack = 0
     * Byte totals follow used_points * 3 * scale^2 at 3 bytes per pixel. */
    const int cells = paper_cells_for(8, 8, scale);
    const int block_pts = paper_block_points(cells);
    int exp_page1 = (scale == 1) ? 0 : 1;
    int exp_x1 = (scale == 1) ? 8 : 0;
    int exp_y1 = 0;

    CK(p1.page == exp_page1 && p1.x == exp_x1 && p1.y == exp_y1 && p1.block_id == 1);

    CK(atlas_ref_stats(a, &s) == ATLAS_OK);
    f.used_points = 2 * block_pts;
    f.free_points = f.budget_points - f.used_points;
    f.used_bytes = f.used_points * 3 * scale * scale;
    f.free_bytes = f.free_points * 3 * scale * scale;
    f.requested_points = 2 * 8 * 8 * scale * scale;
    f.padding_points = f.used_points - f.requested_points;
    CK(s.used_points == f.used_points && s.free_points == f.free_points);
    CK(s.used_bytes == f.used_bytes && s.free_bytes == f.free_bytes);
    CK(s.requested_points == f.requested_points);
    CK(s.padding_points == f.padding_points && s.stat_error == f.padding_points);
    CK(s.live_blocks == 2 && s.idle_blocks == 0 && s.index_len == 2);
    CK(s.live_refs == 3);
    CK(s.pages == exp_page1 + 1);
    CK(s.page_total_points == (exp_page1 + 1) * 256);
    CK(s.slack_points == s.page_total_points - f.used_points);
    CK(s.balance_points == 0 && s.balance_bytes == 0);
    CK(s.acquired == 2 && s.reacquired == 1 && s.refs_total == 3);

    /* reference release: one drop makes rect 1 idle, block stays indexed */
    CK(atlas_ref_release(a, 1, &err) == ATLAS_OK);
    CK(atlas_ref_stats(a, &s) == ATLAS_OK);
    CK(s.live_refs == 2 && s.live_blocks == 1 && s.idle_blocks == 1);
    CK(s.index_len == 2);
    CK(s.release_zeroed == 1 && s.released == 1);
    /* a second release on a zero count is stale, not a crash */
    CK(atlas_ref_release(a, 1, &err) == ATLAS_ERR_STALE);
    CK(err.code == ATLAS_ERR_STALE && err.index == 1);

    /* re-acquiring an idle block must clear the phase-1 idle mark again */
    CK(atlas_ref_acquire(a, 1, 8, 8, &p1, &err) == ATLAS_OK);
    CK(atlas_ref_validate(a, &err) == ATLAS_OK);
    CK(atlas_ref_release(a, 1, &err) == ATLAS_OK);
    CK(atlas_ref_validate(a, &err) == ATLAS_OK);

    /* upload real pixels: reset must wipe every one of them */
    unsigned char *rgb = (unsigned char*)malloc((size_t)f.page_bytes);
    CK(rgb != NULL);
    fill_pattern(rgb, (size_t)f.page_bytes);
    CK(atlas_ref_upload(a, 0, rgb, (size_t)f.page_bytes, &err) == ATLAS_OK);
    CK(atlas_ref_upload(a, 0, rgb, 100, &err) == ATLAS_ERR_SIZE);
    free(rgb);

    CK(atlas_ref_validate(a, &err) == ATLAS_OK);

    /* export before reset */
    CK(atlas_ref_export(a, "/tmp/atlas_ref_pre.txt", &err) == ATLAS_OK);
    CK(read_file("/tmp/atlas_ref_pre.txt", buf, sizeof(buf)) > 0);
    {
        char want[160];
        snprintf(want, sizeof(want),
                 "pages=%d budget_points=512 used_points=%d free_points=%d index_len=2 live=1 idle=1",
                 exp_page1 + 1, f.used_points, f.free_points);
        CK(strstr(buf, want) != NULL);
        snprintf(want, sizeof(want),
                 "rect 0 page=0 x=0 y=0 w=8 h=8 block=%d refs=2", block_pts == 64 ? 8 : 16);
        CK(strstr(buf, want) != NULL);
        snprintf(want, sizeof(want),
                 "rect 1 page=%d x=%d y=%d w=8 h=8 block=%d refs=0",
                 exp_page1, exp_x1, exp_y1, block_pts == 64 ? 8 : 16);
        CK(strstr(buf, want) != NULL);
    }

    /* sampling counters: two hits (rect 0 live, rect 1 idle -> miss) */
    {
        AtlasRefPos sp;
        int before_hits = s.sample_hits, before_misses = s.sample_misses;
        CK(atlas_ref_sample(a, 0, &sp, &err) == ATLAS_OK);
        CK(atlas_ref_sample(a, 1, &sp, &err) == ATLAS_OK);  /* idle -> miss */
        CK(atlas_ref_sample(a, 5, &sp, &err) == ATLAS_ERR_STALE);
        CK(atlas_ref_stats(a, &s) == ATLAS_OK);
        CK(s.samples == 3);
        CK(s.sample_hits - before_hits == 1);
        CK(s.sample_misses - before_misses == 2);
        CK(atlas_ref_stats(a, &s) == ATLAS_OK);
    }

    /* two-phase reset: phase 1 marks the one idle block, phase 2 wipes and
     * clears both index entries; rect 0 still holds 2 refs and is a leak */
    AtlasRefStats post;
    CK(atlas_ref_reset(a, &post, &err) == ATLAS_OK);
    CK(post.reset_marked == 1);
    CK(post.reset_leaked == 1);
    CK(post.reset_index_cleared == 2);
    CK(post.resets == 1);
    CK(atlas_ref_stats(a, &s) == ATLAS_OK);
    CK(s.index_len == 0 && s.live_blocks == 0 && s.idle_blocks == 0 && s.live_refs == 0);
    CK(s.used_points == 0 && s.free_points == f.budget_points && s.pages == 0);
    CK(s.balance_points == 0 && s.balance_bytes == 0);
    CK(s.acquired == 2 && s.samples == 3);
    CK(s.sample_hits + s.sample_misses == s.samples);
    CK(s.released == 2 && s.release_zeroed == 2);
    CK(atlas_ref_validate(a, &err) == ATLAS_OK);

    /* all written bytes are gone */
    size_t got = 0;
    const unsigned char *page = atlas_ref_page_pixels(a, 0, &got, &err);
    CK(page != NULL && got == (size_t)f.page_bytes);
    CK(page && all_zero(page, got));

    /* layout after reset equals a fresh atlas, byte for byte */
    CK(atlas_ref_export(a, "/tmp/atlas_ref_post.txt", &err) == ATLAS_OK);
    AtlasRefAtlas *fresh = atlas_ref_create(&cfg, scale, &err);
    CK(fresh != NULL);
    CK(atlas_ref_export(fresh, "/tmp/atlas_ref_fresh.txt", &err) == ATLAS_OK);
    char post_buf[1024], fresh_buf[1024];
    CK(read_file("/tmp/atlas_ref_post.txt", post_buf, sizeof(post_buf)) ==
       read_file("/tmp/atlas_ref_fresh.txt", fresh_buf, sizeof(fresh_buf)));
    CK(strcmp(post_buf, fresh_buf) == 0);
    atlas_ref_destroy(fresh);

    /* identical repack after reset reproduces the same layout text; compare
     * the module against the paper model instead of hard-coded coordinates */
    atlas_ref_destroy(a);
    a = atlas_ref_create(&cfg, scale, &err);
    CK(a != NULL);
    CK(atlas_ref_acquire(a, 0, 8, 8, &p0, &err) == ATLAS_OK);
    CK(atlas_ref_acquire(a, 1, 8, 8, &p1, &err) == ATLAS_OK);
    CK(atlas_ref_acquire(a, 0, 8, 8, &p0, &err) == ATLAS_OK);
    CK(atlas_ref_release(a, 1, &err) == ATLAS_OK);
    paper_reset();
    CK(paper_alloc(0, cells) == exp_page1 * 0);
    CK(paper_alloc(1, cells) == exp_page1);
    int pp, px, py;
    CK(paper_find(0, &pp, &px, &py) == 0);
    CK(pp == 0 && px == 0 && py == 0);
    CK(p0.x == px && p0.y == py && p0.page == pp);
    CK(paper_find(1, &pp, &px, &py) == 0);
    CK(pp == exp_page1 && px == exp_x1 && py == exp_y1);
    CK(p1.x == px && p1.y == py && p1.page == pp);

    CK(atlas_ref_export(a, "/tmp/atlas_ref_repack.txt", &err) == ATLAS_OK);
    CK(read_file("/tmp/atlas_ref_repack.txt", buf, sizeof(buf)) > 0);
    {
        char want[160];
        snprintf(want, sizeof(want),
                 "pages=%d budget_points=512 used_points=%d free_points=%d index_len=2 live=1 idle=1",
                 exp_page1 + 1, f.used_points, f.free_points);
        CK(strstr(buf, want) != NULL);
        snprintf(want, sizeof(want), "rect 1 page=%d x=%d y=%d w=8 h=8", exp_page1, exp_x1, exp_y1);
        CK(strstr(buf, want) != NULL);
    }
    atlas_ref_destroy(a);
}

int main(void) {
    run_scale(1);
    run_scale(2);

    /* scale-dependent geometry: 2x doubles the pixels and bytes, not the points */
    {
        AtlasRefConfig cfg = {16, 2, 16};
        AtlasError err;
        AtlasRefStats s1, s2;
        AtlasRefAtlas *a1 = atlas_ref_create(&cfg, 1, &err);
        AtlasRefAtlas *a2 = atlas_ref_create(&cfg, 2, &err);
        CK(a1 && a2);
        CK(atlas_ref_stats(a1, &s1) == ATLAS_OK && atlas_ref_stats(a2, &s2) == ATLAS_OK);
        CK(s2.page_px == 2 * s1.page_px);
        CK(s2.page_bytes == 4 * s1.page_bytes);
        CK(s2.budget_points == s1.budget_points);
        CK(s2.budget_bytes == 4 * s1.budget_bytes);
        CK(s2.used_points == s1.used_points && s2.free_points == s1.free_points);
        atlas_ref_destroy(a1);
        atlas_ref_destroy(a2);
    }

    /* out-of-range accesses must be reported with their numbers */
    {
        AtlasRefConfig cfg = {16, 2, 16};
        AtlasError err;
        AtlasRefAtlas *a = atlas_ref_create(&cfg, 1, &err);
        AtlasRefPos pos;
        CK(a != NULL);
        CK(a && atlas_ref_probe(a, 2, 0, 0, &err) == ATLAS_ERR_OOB);
        CK(err.code == ATLAS_ERR_OOB && err.page == 2 && err.detail == 16);
        CK(a && atlas_ref_probe(a, 0, 16, 0, &err) == ATLAS_ERR_OOB);
        CK(err.code == ATLAS_ERR_OOB && err.page == 0 && err.x == 16 && err.y == 0);
        CK(a && atlas_ref_probe(a, 0, -1, 5, &err) == ATLAS_ERR_OOB);
        CK(err.code == ATLAS_ERR_OOB && err.x == -1 && err.y == 5);
        CK(a && atlas_ref_probe(a, -1, 0, 0, &err) == ATLAS_ERR_OOB);
        CK(err.page == -1 && err.x == 0 && err.y == 0);
        CK(a && atlas_ref_acquire(a, 16, 8, 8, &pos, &err) == ATLAS_ERR_BAD_RECT);
        CK(err.index == 16);
        CK(a && atlas_ref_acquire(a, 0, 32, 8, &pos, &err) == ATLAS_ERR_SIZE);
        CK(err.index == 0 && err.detail == 16);
        CK(a && atlas_ref_acquire(a, 0, 8, 8, &pos, &err) == ATLAS_OK);
        CK(pos.page == 0 && pos.x == 0 && pos.y == 0);
        CK(a && atlas_ref_probe(a, 0, 17, 4, &err) == ATLAS_ERR_OOB);
        CK(a && atlas_ref_probe(a, 0, 0, 0, &err) == 0);
        CK(a && atlas_ref_probe(a, 0, 7, 7, &err) == 0);
        CK(a && atlas_ref_probe(a, 0, 8, 8, &err) == -1);
        AtlasRefStats s;
        CK(atlas_ref_stats(a, &s) == ATLAS_OK);
        CK(s.oob_checks == 5);
        CK(atlas_ref_validate(a, &err) == ATLAS_OK);
        atlas_ref_destroy(a);
    }

    /* capacity: a 16-point page holds 2x2 cells, i.e. four 8x8 blocks; after
     * four blocks page 0 is full, the next four spill to page 1, then the
     * budget is exhausted.  Paper derivation: 4 + 4 = 8 blocks = 512 points. */
    {
        AtlasRefConfig cfg = {16, 2, 16};
        AtlasError err;
        AtlasRefPos pos;
        AtlasRefAtlas *a = atlas_ref_create(&cfg, 1, &err);
        CK(a != NULL);
        paper_reset();
        for (int i = 0; i < 8; ++i) {
            CK(a && atlas_ref_acquire(a, i, 8, 8, &pos, &err) == ATLAS_OK);
            CK(paper_alloc(i, paper_cells_for(8, 8, 1)) == i / 4);
            int pp, px, py;
            CK(paper_find(i, &pp, &px, &py) == 0);
            CK(pos.page == pp && pos.x == px && pos.y == py);
        }
        CK(a && atlas_ref_acquire(a, 8, 8, 8, &pos, &err) == ATLAS_ERR_FRAGMENTED);
        CK(err.index == 8 && err.detail == 8);
        AtlasRefStats s;
        CK(atlas_ref_stats(a, &s) == ATLAS_OK);
        CK(s.pages == 2 && s.index_len == 8 && s.used_points == 512 && s.free_points == 0);
        CK(s.balance_points == 0 && s.balance_bytes == 0);
        atlas_ref_destroy(a);
    }

    /* legacy demo text is a frozen golden file, hashed as raw bytes */
    {
        char golden[1024];
        int n = read_file("testdata/demo_legacy.txt", golden, sizeof(golden));
        CK(n > 0);
        CK(n > 0 && djb2(golden) == 0x92f9b672fcfa96c1UL);
        CK(n > 0 && strstr(golden, "shelves=2 used=2688") != NULL);
        if (n <= 0) {
            printf("note: run tests from the atlas/ directory so testdata/ resolves\n");
        }
    }

    /* the point index is little-endian: 0x00000001 -> 01 00 00 00 */
    {
        unsigned int v = 1;
        unsigned char raw[4];
        memcpy(raw, &v, 4);
        CK(is_le() ? (raw[0] == 1 && raw[1] == 0 && raw[2] == 0 && raw[3] == 0)
                   : (raw[0] == 0 && raw[1] == 0 && raw[2] == 0 && raw[3] == 1));
    }
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}

#endif
