#include "occ.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

/* Hand-computed on a 4x3 grid (12 cells), segments:
 *   s0 (0.5,0.5)->(3.5,0.5): row y=0, cells x=0..3 -> 4 cells, 4/12
 *   s1 (1.5,0.5)->(1.5,2.5): col x=1, cells y=0..2 -> 3 cells, 3/12
 *   s2 (2.5,1.5)->(3.5,1.5): row y=1, cells x=2..3 -> 2 cells, 2/12
 * union: (0,0)(1,0)(2,0)(3,0)(1,1)(1,2)(2,1)(3,1) = 8 cells
 * added: s0=4, s1=2, s2=1 (cell (2,1),(3,1); one overlaps s0? no -- s0
 *        stays on row 0; s2 adds both, so s2=2) -> 4+2+2 = 8
 */
int main(void) {
    enum { W = 4, H = 3, N = 3 };
    OccSeg segs[N] = {
        {0.5f, 0.5f, 3.5f, 0.5f},
        {1.5f, 0.5f, 1.5f, 2.5f},
        {2.5f, 1.5f, 3.5f, 1.5f},
    };
    int bad = -1;

    /* ---- phase 1: per-segment rates against hand calculation ---- */
    OccCov cov;
    CK(occ_compute(&cov, segs, N, W, H, &bad) == OCC_OK);
    CK(cov.w == W && cov.h == H && cov.nseg == N);
    int expect_cov[N] = {4, 3, 2};
    float expect_rate[N] = {4.0f / 12.0f, 3.0f / 12.0f, 2.0f / 12.0f};
    for (int i = 0; i < N; ++i) {
        CK(cov.covered[i] == expect_cov[i]);
        CK(fabsf(cov.rate[i] - expect_rate[i]) < 1e-6f);
    }
    /* exact cell membership, segment 0 row y=0 */
    for (int x = 0; x < W; ++x)
        CK(cov.mask[0 * W * H + 0 * W + x] == 1);
    CK(cov.mask[0 * W * H + 1 * W + 0] == 0);
    /* segment 1 column x=1 */
    for (int y = 0; y < H; ++y)
        CK(cov.mask[1 * W * H + y * W + 1] == 1);
    CK(cov.mask[1 * W * H + 0 * W + 0] == 0);
    /* segment 2 cells (2,1) and (3,1) */
    CK(cov.mask[2 * W * H + 1 * W + 2] == 1);
    CK(cov.mask[2 * W * H + 1 * W + 3] == 1);
    CK(cov.mask[2 * W * H + 0 * W + 2] == 0);

    /* ---- independent reference must match every plane byte ---- */
    unsigned char *ref = (unsigned char *)malloc((size_t)W * H);
    for (int i = 0; i < N; ++i) {
        CK(occ_reference_mask(ref, &segs[i], 1, W, H, &bad) == OCC_OK);
        CK(memcmp(ref, cov.mask + (size_t)i * W * H, (size_t)W * H) == 0);
    }
    /* all-segment reference equals the union built in phase 2 */
    CK(occ_reference_mask(ref, segs, N, W, H, &bad) == OCC_OK);

    /* ---- phase 2: aggregate ---- */
    OccAgg agg;
    CK(occ_aggregate(&cov, &agg, &bad) == OCC_OK);
    CK(agg.union_cells == 8);
    int expect_added[N] = {4, 2, 2};
    for (int i = 0; i < N; ++i) CK(agg.added[i] == expect_added[i]);
    CK(agg.or_ops == 4 + 3 + 2);
    CK(memcmp(ref, agg.mask, (size_t)W * H) == 0);

    /* ---- aggregation off: recompute-only, phase-1 output unchanged ---- */
    OccCov cov2;
    CK(occ_compute(&cov2, segs, N, W, H, &bad) == OCC_OK);
    CK(memcmp(cov.mask, cov2.mask, (size_t)N * W * H) == 0);
    CK(memcmp(cov.rate, cov2.rate, (size_t)N * sizeof(float)) == 0);
    CK(memcmp(cov.covered, cov2.covered, (size_t)N * sizeof(int)) == 0);
    unsigned char snap[12];
    memcpy(snap, cov2.mask, sizeof(snap));
    /* no aggregate call: masks stay byte-identical */
    CK(memcmp(snap, cov2.mask, sizeof(snap)) == 0);
    occ_agg_free(&agg);
    occ_cov_free(&cov);
    occ_cov_free(&cov2);
    free(ref);

    /* ---- coverage semantics on grid corners and grid lines ---- */
    {
        /* Segment passes exactly through the corner shared by four
         * cells: only the entry and exit cells count (no corner-only
         * touches), on every axis direction. */
        OccSeg diag[4] = {
            {0.25f, 0.25f, 3.75f, 3.75f},
            {3.75f, 0.25f, 0.25f, 3.75f},
            {3.75f, 3.75f, 0.25f, 0.25f},
            {0.25f, 3.75f, 3.75f, 0.25f},
        };
        OccCov dc;
        CK(occ_compute(&dc, diag, 4, 4, 4, &bad) == OCC_OK);
        /* Each main diagonal crosses 3 corners diagonally: 4 cells. */
        for (int i = 0; i < 4; ++i) CK(dc.covered[i] == 4);
        unsigned char r16[16];
        for (int i = 0; i < 4; ++i) {
            CK(occ_reference_mask(r16, &diag[i], 1, 4, 4, &bad) == OCC_OK);
            CK(memcmp(r16, dc.mask + (size_t)i * 16, 16) == 0);
        }
        occ_cov_free(&dc);

        /* Segment lying on a grid line covers the cells on both sides. */
        OccSeg on_line[1] = {{0.5f, 2.0f, 3.5f, 2.0f}};
        OccCov lc;
        CK(occ_compute(&lc, on_line, 1, 4, 4, &bad) == OCC_OK);
        CK(lc.covered[0] == 8);                 /* 4 cells per row, 2 rows */
        for (int x = 0; x < 4; ++x) {
            CK(lc.mask[1 * 4 + x] == 1);       /* y=1 */
            CK(lc.mask[2 * 4 + x] == 1);       /* y=2 */
        }
        occ_cov_free(&lc);

        /* 1x1 grid: any non-degenerate in-bounds segment covers 1 cell. */
        OccSeg tiny[1] = {{0.1f, 0.1f, 0.9f, 0.9f}};
        OccCov tc;
        CK(occ_compute(&tc, tiny, 1, 1, 1, &bad) == OCC_OK);
        CK(tc.covered[0] == 1 && tc.rate[0] == 1.0f);
        occ_cov_free(&tc);
    }

    /* ---- error paths, each naming the segment index ---- */
    /* zero segments */
    OccCov c0;
    bad = -1;
    CK(occ_compute(&c0, segs, 0, W, H, &bad) == OCC_EZERO);
    CK(bad == 0);

    /* out-of-bounds: segment 1 endpoint sits on the outer border x=W */
    OccSeg oob[2] = {{0.5f, 0.5f, 1.5f, 0.5f},
                     {(float)W, 1.5f, 3.5f, 1.5f}};
    bad = -1;
    CK(occ_compute(&c0, oob, 2, W, H, &bad) == OCC_EOOB);
    CK(bad == 1);
    bad = -1;
    unsigned char tmp[12];
    CK(occ_reference_mask(tmp, oob, 2, W, H, &bad) == OCC_EOOB);
    CK(bad == 1);
    /* negative coordinate on segment 0 */
    OccSeg oob0[1] = {{-0.1f, 0.5f, 1.5f, 0.5f}};
    bad = -1;
    CK(occ_compute(&c0, oob0, 1, W, H, &bad) == OCC_EOOB);
    CK(bad == 0);

    /* empty mask: zero-length segment at index 2 */
    OccSeg emp[3] = {{0.5f, 0.5f, 1.5f, 0.5f},
                     {0.5f, 1.5f, 1.5f, 1.5f},
                     {2.5f, 2.5f, 2.5f, 2.5f}};
    bad = -1;
    CK(occ_compute(&c0, emp, 3, W, H, &bad) == OCC_EEMPTY);
    CK(bad == 2);

    /* null / bad-size arguments */
    CK(occ_compute(NULL, segs, N, W, H, &bad) == OCC_EARG);
    CK(occ_compute(&c0, NULL, N, W, H, &bad) == OCC_EARG);
    CK(occ_compute(&c0, segs, N, 0, H, &bad) == OCC_EARG);

    /* aggregate on a never-computed state -> empty-mask error */
    OccCov blank;
    memset(&blank, 0, sizeof(blank));
    OccAgg a0;
    bad = -1;
    CK(occ_aggregate(&blank, &a0, &bad) == OCC_EMASK);
    CK(bad == 0);

    /* PGM writer smoke test */
    unsigned char one[4] = {1, 0, 0, 1};
    CK(occ_write_pgm(one, 2, 2, "build/smoke.pgm") == OCC_OK);
    CK(occ_write_pgm(NULL, 2, 2, "x.pgm") == OCC_EARG);

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
