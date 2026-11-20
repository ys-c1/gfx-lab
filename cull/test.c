#include "cull.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

/* --- hand-checkable scene ------------------------------------------- */
/* Region 64x64, 1 cell per unit at 64x64.
 * O0 covers the left half  [0,32)x[0,64) at depth 1
 * O1 covers             [32,64)x[ 0,32) at depth 2
 * Expected, for depth-5 candidates:
 *   C0 [ 4,12)x[ 4,12)  inside O0                -> culled
 *   C1 [40,48)x[ 8,16)  inside O1                -> culled
 *   C2 [40,48)x[40,48)  no occluder covers            -> visible
 *   C3 [ 4,12)x[ 4,12)  depth 0.5, in front of O0     -> visible
 *   C4 [30,38)x[56,64)  straddles O0's right edge     -> visible
 *   C5 [24,32)x[32,40)  inside O0                     -> culled
 *   C6 [32,40)x[32,40)  O1 stops at y=32              -> visible
 */
static void hand_scene(CullSprite occ[2], CullSprite cand[7]) {
    occ[0].x = 0;  occ[0].y = 0; occ[0].w = 32; occ[0].h = 64; occ[0].depth = 1.0;
    occ[1].x = 32; occ[1].y = 0; occ[1].w = 32; occ[1].h = 32; occ[1].depth = 2.0;
    double r[7][5] = {
        { 4,  4, 8, 8, 5.0},
        {40,  8, 8, 8, 5.0},
        {40, 40, 8, 8, 5.0},
        { 4,  4, 8, 8, 0.5},
        {30, 56, 8, 8, 5.0},
        {24, 32, 8, 8, 5.0},
        {32, 32, 8, 8, 5.0},
    };
    for (int i = 0; i < 7; ++i) {
        cand[i].x = r[i][0]; cand[i].y = r[i][1];
        cand[i].w = r[i][2]; cand[i].h = r[i][3]; cand[i].depth = r[i][4];
    }
}
static const int hand_expect[7] = {0, 0, 1, 1, 1, 0, 1}; /* 1 == visible */

/* --- deterministic pseudo-random scenes ----------------------------- */
static unsigned long long rng_state;
static unsigned rnd(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (unsigned)(rng_state >> 11);
}
static double rd(double lo, double hi) { return lo + (hi - lo) * ((double)(rnd() % 1000000) / 1000000.0); }

static void random_scene(CullSprite *occ, int nocc, CullSprite *cand, int n,
                         unsigned long long seed, double span) {
    rng_state = seed ? seed : 1;
    for (int i = 0; i < nocc; ++i) {
        occ[i].x = rd(0, span * 0.7);
        occ[i].y = rd(0, span * 0.7);
        occ[i].w = rd(span * 0.08, span * 0.35);
        occ[i].h = rd(span * 0.08, span * 0.5);
        occ[i].depth = rd(0.2, 2.0);
    }
    for (int i = 0; i < n; ++i) {
        cand[i].x = rd(0, span * 0.95);
        cand[i].y = rd(0, span * 0.95);
        cand[i].w = rd(1, span * 0.08);
        cand[i].h = rd(1, span * 0.08);
        cand[i].depth = rd(0.2, 2.0);
    }
}

/* Count culled candidates that the exact reference calls visible:
 * those are false culls (wrongly removed). */
static int false_culls(const unsigned char *exact, const CullOcclusionResult *r, int n) {
    unsigned char kept[4096];
    if (n > 4096) return -1;
    memset(kept, 0, (size_t)n);
    for (int i = 0; i < r->count; ++i) kept[r->ids[i]] = 1;
    int bad = 0;
    for (int i = 0; i < n; ++i) if (!kept[i] && exact[i]) ++bad;
    return bad;
}

int main(void) {
    /* ---- existing view culling must not regress ---- */
    {
        CullBox b[4] = {{0,0,10,10,0},{100,100,10,10,1},{50,50,20,20,0},{-5,-5,10,10,2}};
        CullView v = {0, 0, 60, 60};
        CullResult r;
        CK(cull_run(b, 4, v, &r) == 0);
        CK(r.count == 3);
        CK(r.ids[0] == 0 && r.ids[1] == 2 && r.ids[2] == 3);
        CK(cull_count_within(b, 4, v) == 3);
        CullView bad = {0, 0, 0, 10};
        CK(cull_run(b, 4, bad, &r) == -2);
        free(r.ids);
    }

    CullSprite occ[2], cand[7];
    hand_scene(occ, cand);
    CullDepthMap m;

    /* ---- init validation ---- */
    CK(cull_depth_init(NULL, 8, 8, 0, 0, 64, 64) == CULL_ERR_ARG);
    CK(cull_depth_init(&m, 0, 8, 0, 0, 64, 64) == CULL_ERR_ARG);
    CK(cull_depth_init(&m, 8, 8, 64, 0, 0, 64) == CULL_ERR_ARG);
    CK(cull_depth_init(&m, 8, 8, 0, 0, 64, 64) == CULL_OK);
    cull_depth_free(&m);

    /* ---- empty / unrendered depth map is an error while enabled ---- */
    {
        CullOcclusionResult r;
        int idx = 123;
        CK(cull_occlusion(NULL, cand, 7, 1, &r, &idx) == CULL_ERR_EMPTY_DEPTH);
        CK(idx == -1);
        CK(cull_depth_init(&m, 64, 64, 0, 0, 64, 64) == CULL_OK);
        CK(cull_occlusion(&m, cand, 7, 1, &r, &idx) == CULL_ERR_EMPTY_DEPTH); /* never rendered */
        CK(cull_depth_render(&m, NULL, 0, NULL) == CULL_OK);                   /* no occluders */
        CK(m.covered_cells == 0);
        CK(cull_occlusion(&m, cand, 7, 1, &r, &idx) == CULL_ERR_EMPTY_DEPTH); /* rendered but empty */
        /* occluders entirely outside the window leave the map empty too */
        CullSprite far = {-500, -500, 10, 10, 1.0};
        CK(cull_depth_render(&m, &far, 1, NULL) == CULL_OK);
        CK(m.covered_cells == 0);
        CK(cull_occlusion(&m, cand, 7, 1, &r, &idx) == CULL_ERR_EMPTY_DEPTH);
        cull_depth_free(&m);
    }

    /* ---- hand scene at 1 cell per unit ---- */
    {
        CK(cull_depth_init(&m, 64, 64, 0, 0, 64, 64) == CULL_OK);
        CK(cull_depth_render(&m, occ, 2, NULL) == CULL_OK);
        /* O0 paints x 0..31 for all y: 32*64 = 2048 cells.
         * O1 paints x 32..63, y 0..31: 32*32 = 1024 cells. */
        CK(m.covered_cells == 2048 + 1024);

        CullOcclusionResult r;
        CK(cull_occlusion(&m, cand, 7, 1, &r, NULL) == CULL_OK);
        CK(r.count == 4);
        CK(r.culled == 3);
        unsigned char kept[7] = {0};
        for (int i = 0; i < r.count; ++i) kept[r.ids[i]] = 1;
        for (int i = 0; i < 7; ++i) CK(kept[i] == hand_expect[i]);
        CK(r.ids[0] == 2 && r.ids[1] == 3 && r.ids[2] == 4 && r.ids[3] == 6);
        cull_occlusion_free(&r);

        /* exact reference agrees with the hand arithmetic */
        unsigned char exact[7];
        CK(cull_reference_visible(occ, 2, cand, 7, exact, NULL) == CULL_OK);
        for (int i = 0; i < 7; ++i) CK(exact[i] == hand_expect[i]);

        /* occlusion off: every candidate, ascending, culled == 0 */
        CK(cull_occlusion(&m, cand, 7, 0, &r, NULL) == CULL_OK);
        CK(r.count == 7 && r.culled == 0);
        for (int i = 0; i < 7; ++i) CK(r.ids[i] == i);
        cull_occlusion_free(&r);

        /* occlusion off does not need a depth map at all */
        CK(cull_occlusion(NULL, cand, 7, 0, &r, NULL) == CULL_OK);
        CK(r.count == 7);
        cull_occlusion_free(&r);

        /* a point sprite has no blockable footprint: visible either way */
        {
            CullSprite pt = {8, 8, 0, 0, 5.0};
            CK(cull_occlusion(&m, &pt, 1, 1, &r, NULL) == CULL_OK);
            CK(r.count == 1 && r.culled == 0);
            cull_occlusion_free(&r);
            CK(cull_occlusion(NULL, &pt, 1, 0, &r, NULL) == CULL_OK);
            CK(r.count == 1 && r.ids[0] == 0);
            cull_occlusion_free(&r);
            unsigned char ex1[1];
            CK(cull_reference_visible(occ, 2, &pt, 1, ex1, NULL) == CULL_OK);
            CK(ex1[0] == 1);
        }

        /* ... but still validates coordinates */
        {
            CullSprite badc[7];
            memcpy(badc, cand, sizeof cand);
            badc[4].x = NAN;
            int idx = -1;
            CK(cull_occlusion(NULL, badc, 7, 0, &r, &idx) == CULL_ERR_NONFINITE);
            CK(idx == 4);
        }
        cull_depth_free(&m);
    }

    /* ---- failures point at the offending index ---- */
    {
        CullSprite o2[2], c7[7];
        memcpy(o2, occ, sizeof o2);
        memcpy(c7, cand, sizeof c7);

        CullDepthMap dm;
        CK(cull_depth_init(&dm, 16, 16, 0, 0, 64, 64) == CULL_OK);
        CullOcclusionResult r;

        o2[1].depth = INFINITY;
        int idx = -1;
        CK(cull_depth_render(&dm, o2, 2, &idx) == CULL_ERR_NONFINITE);
        CK(idx == 1);
        o2[1].depth = 2.0;
        o2[0].w = -4;
        CK(cull_depth_render(&dm, o2, 2, &idx) == CULL_ERR_RECT);
        CK(idx == 0);
        o2[0].w = 32;

        c7[3].x = NAN;
        idx = -1;
        CK(cull_occlusion(&dm, c7, 7, 1, &r, &idx) == CULL_ERR_NONFINITE);
        CK(idx == 3);
        c7[3].x = 4;
        c7[5].x = -INFINITY;
        idx = -1;
        CK(cull_occlusion(&dm, c7, 7, 1, &r, &idx) == CULL_ERR_NONFINITE);
        CK(idx == 5);
        c7[5].x = 24;
        c7[2].h = -1;
        idx = -1;
        CK(cull_occlusion(&dm, c7, 7, 1, &r, &idx) == CULL_ERR_RECT);
        CK(idx == 2);
        c7[2].h = 8;
        CK(cull_depth_render(&dm, o2, 2, NULL) == CULL_OK);
        idx = -1;
        CK(cull_occlusion(&dm, c7, 7, 1, &r, &idx) == CULL_OK);
        cull_occlusion_free(&r);
        cull_depth_free(&dm);
    }

    /* ---- resolution: coarse maps may under-cull, never over-cull ---- */
    {
        CullSprite o = {0, 0, 18, 64, 1.0};          /* wall covers x < 18 */
        CullSprite c = {14, 8, 4, 8, 5.0};           /* fully behind it */
        CullOcclusionResult r;
        CullDepthMap dm;
        int culled[5];
        int res[5] = {4, 8, 16, 64, 256};
        for (int k = 0; k < 5; ++k) {
            CK(cull_depth_init(&dm, res[k], res[k], 0, 0, 64, 64) == CULL_OK);
            CK(cull_depth_render(&dm, &o, 1, NULL) == CULL_OK);
            CK(cull_occlusion(&dm, &c, 1, 1, &r, NULL) == CULL_OK);
            culled[k] = r.culled;
            /* whatever the resolution says, a cull must be correct */
            if (r.culled) CK(c.x + c.w <= 18.0);
            cull_occlusion_free(&r);
            cull_depth_free(&dm);
        }
        CK(culled[0] == 0);  /* 16-unit cells: wall does not fully cover cell 1 */
        CK(culled[1] == 0);  /*  8-unit cells: same */
        CK(culled[2] == 0);  /*  4-unit cells: same */
        CK(culled[3] == 1);  /*  1-unit cells: exact */
        CK(culled[4] == 1);
    }

    /* ---- randomized cross-check against the exact reference ------- */
    {
        enum { NOCC = 10, NCAND = 600 };
        static CullSprite occ2[NOCC], cand2[NCAND];
        static unsigned char exact[NCAND];
        int res[4] = {8, 64, 256, 1024};
        int sdk[4] = {-1, -1, -1, -1}, sdk_exact[4] = {-1, -1, -1, -1};
        int total_occluded = 0, total_cases = 0;
        for (unsigned long long seed = 1; seed <= 5; ++seed) {
            random_scene(occ2, NOCC, cand2, NCAND, seed * 7919, 256.0);
            CK(cull_reference_visible(occ2, NOCC, cand2, NCAND, exact, NULL) == CULL_OK);
            /* sanity: the reference must also pass unknown-answer scenes */
            int expected_occluded = 0;
            for (int i = 0; i < NCAND; ++i) expected_occluded += !exact[i];
            CK(expected_occluded > 0);
            ++total_cases;
            total_occluded += expected_occluded;
            for (int k = 0; k < 4; ++k) {
                CullDepthMap dm;
                CullOcclusionResult r;
                CK(cull_depth_init(&dm, res[k], res[k], 0, 0, 256, 256) == CULL_OK);
                CK(cull_depth_render(&dm, occ2, NOCC, NULL) == CULL_OK);
                CK(cull_occlusion(&dm, cand2, NCAND, 1, &r, NULL) == CULL_OK);
                /* depth-map culling can only be conservative: everything kept
                 * must be visible in the exact reference, and it never culls
                 * more than the reference does. */
                CK(false_culls(exact, &r, NCAND) == 0);
                CK(r.culled <= expected_occluded);
                if (false_culls(exact, &r, NCAND) != 0) sdk[k] = seed;
                if (r.culled > expected_occluded) sdk_exact[k] = seed;
                cull_occlusion_free(&r);
                cull_depth_free(&dm);
            }
        }
        for (int k = 0; k < 4; ++k) CK(sdk[k] == -1);
        for (int k = 0; k < 4; ++k) CK(sdk_exact[k] == -1);
        (void)total_occluded;
        (void)total_cases;
    }

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
