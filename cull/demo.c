#define _POSIX_C_SOURCE 199309L
#include "cull.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */
static unsigned long long rng_state = 1;
static unsigned rnd(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (unsigned)(rng_state >> 11);
}
static int ri(int lo, int hi) { return lo + (int)(rnd() % (unsigned)(hi - lo + 1)); }

static double now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}

/* ------------------------------------------------------------------ */
/* Hand-checked scene: every verdict can be read off the coordinates.  */
/*                                                                     */
/* Walls are full-height strips, so a candidate is hidden iff one wall */
/* covers its whole x-range and is strictly nearer.  Gaps are wider    */
/* than any candidate, so walls can never hide a sprite collectively.  */
/* ------------------------------------------------------------------ */
typedef struct { double x, w, depth; } Wall;

static int hand_occluded(const Wall *walls, int nw, CullSprite c) {
    for (int i = 0; i < nw; ++i)
        if (walls[i].depth < c.depth &&
            walls[i].x <= c.x && c.x + c.w <= walls[i].x + walls[i].w)
            return 1;
    return 0;
}

int main(int argc, char **argv) {
    int nbench_override = 0, res_override = 0;
    if (argc > 2) { nbench_override = atoi(argv[1]); res_override = atoi(argv[2]); }
    else if (argc > 1) { nbench_override = atoi(argv[1]); }

    /* ---- 1. hand-checked small scene, 128x128 at 1 cell per unit ---- */
    {
        Wall walls[2] = {{16, 32, 1.0}, {64, 32, 2.0}};
        CullSprite cand[12];
        double r[12][5] = {
            {20, 10,  8, 8, 3.0},  /* 0 inside wall0                    CULLED */
            {12, 20,  8, 8, 3.0},  /* 1 crosses wall0's left edge       visible */
            {44, 30,  8, 8, 3.0},  /* 2 crosses wall0's right edge      visible */
            {48, 40,  8, 8, 3.0},  /* 3 in the gap                      visible */
            {70, 50,  8, 8, 3.0},  /* 4 inside wall1                    CULLED */
            {60, 60,  8, 8, 3.0},  /* 5 crosses wall1's left edge       visible */
            {88, 70,  8, 8, 3.0},  /* 6 inside wall1                    CULLED */
            {20, 80,  8, 8, 0.5},  /* 7 in front of wall0               visible */
            {20, 90,  8, 8, 1.0},  /* 8 same depth as wall0 (strict <)  visible */
            {96, 100, 8, 8, 3.0},  /* 9 crosses wall1's right edge      visible */
            {0,   0, 16, 8, 3.0},  /* 10 ends at wall0's left edge     visible */
            {16,  0, 32, 8, 3.0},  /* 11 exactly wall0                  CULLED */
        };
        for (int i = 0; i < 12; ++i) {
            cand[i].x = r[i][0]; cand[i].y = r[i][1];
            cand[i].w = r[i][2]; cand[i].h = r[i][3]; cand[i].depth = r[i][4];
        }
        CullDepthMap dm;
        CullOcclusionResult eng, off;
        CullSprite occ[2] = {{16, 0, 32, 128, 1.0}, {64, 0, 32, 128, 2.0}};
        unsigned char exact[12];

        if (cull_depth_init(&dm, 128, 128, 0, 0, 128, 128) != 0) return 1;
        if (cull_depth_render(&dm, occ, 2, NULL) != 0) return 1;
        if (cull_occlusion(&dm, cand, 12, 1, &eng, NULL) != 0) return 1;
        if (cull_occlusion(&dm, cand, 12, 0, &off, NULL) != 0) return 1;
        if (cull_reference_visible(occ, 2, cand, 12, exact, NULL) != 0) return 1;

        printf("== hand-check ==\n");
        printf("  %-3s %-22s %-6s %-8s %-8s %-8s %s\n",
               "#", "rect x,y,w,h", "depth", "hand", "depthmap", "exact", "agree");
        unsigned char kept[12] = {0};
        for (int i = 0; i < eng.count; ++i) kept[eng.ids[i]] = 1;
        int bad = 0;
        for (int i = 0; i < 12; ++i) {
            int handvis = !hand_occluded(walls, 2, cand[i]);
            int agree = (kept[i] == handvis) && (exact[i] == (unsigned char)handvis);
            if (!agree) ++bad;
            printf("  %-3d (%5.0f,%5.0f,%4.0f,%4.0f) %-6.1f %-8s %-8s %-8s %s\n",
                   i, cand[i].x, cand[i].y, cand[i].w, cand[i].h, cand[i].depth,
                   handvis ? "visible" : "CULLED",
                   kept[i] ? "visible" : "CULLED",
                   exact[i] ? "visible" : "CULLED",
                   agree ? "yes" : "NO");
        }
        printf("  culled %d of 12, disagreements %d\n", eng.culled, bad);
        printf("  occlusion off -> %d of 12 visible (all, ascending: %s), culled %d\n\n",
               off.count,
               (off.count == 12 && off.ids[0] == 0 && off.ids[11] == 11) ? "yes" : "NO",
               off.culled);
        cull_occlusion_free(&eng);
        cull_occlusion_free(&off);
        cull_depth_free(&dm);
        if (bad) return 1;
    }

    /* ---- 2. structured 1024x1024 scenes, per-object hand formula ---- */
    Wall walls[6];
    for (int i = 0; i < 6; ++i) {
        walls[i].x = 50.0 + 170.0 * (double)i;
        walls[i].w = 60.0;
        walls[i].depth = 1.0 + (double)i;
    }
    int counts[2] = {1000, 10000};
    int resolutions[3] = {64, 256, 1024};
    if (nbench_override > 0) { counts[0] = nbench_override; counts[1] = -1; }

    for (int ci = 0; ci < 2; ++ci) {
        int n = counts[ci];
        if (n <= 0) continue;
        CullSprite *cand = (CullSprite*)malloc(sizeof(CullSprite) * (size_t)n);
        CullSprite occ[6];
        unsigned char *exact = (unsigned char*)malloc((size_t)n);
        char *hand = (char*)malloc((size_t)n);
        if (!cand || !exact || !hand) return 1;
        for (int i = 0; i < 6; ++i) {
            occ[i].x = walls[i].x; occ[i].y = 0; occ[i].w = walls[i].w;
            occ[i].h = 1024; occ[i].depth = walls[i].depth;
        }
        rng_state = 0x9e3779b97f4a7c15ULL ^ (unsigned long long)n;
        for (int i = 0; i < n; ++i) {
            cand[i].w = ri(4, 12);
            cand[i].h = ri(4, 12);
            cand[i].x = ri(0, 1024 - (int)cand[i].w);
            cand[i].y = ri(0, 1024 - (int)cand[i].h);
            cand[i].depth = 6.0;
            hand[i] = (char)!hand_occluded(walls, 6, cand[i]); /* 1 == visible */
        }
        if (cull_reference_visible(occ, 6, cand, n, exact, NULL) != 0) return 1;

        int hand_occluded_n = 0;
        for (int i = 0; i < n; ++i) hand_occluded_n += !hand[i];

        printf("== %d candidates, 1024x1024, 6 walls ==\n", n);
        printf("  hand formula: %d occluded, %d visible\n",
               hand_occluded_n, n - hand_occluded_n);

        int nres = res_override > 0 ? 1 : 3;
        for (int k = 0; k < nres; ++k) {
            int res = res_override > 0 ? res_override : resolutions[k];
            CullDepthMap dm;
            CullOcclusionResult r;
            if (cull_depth_init(&dm, res, res, 0, 0, 1024, 1024) != 0) return 1;

            double t0 = now_us();
            if (cull_depth_render(&dm, occ, 6, NULL) != 0) return 1;
            double t1 = now_us();
            if (cull_occlusion(&dm, cand, n, 1, &r, NULL) != 0) return 1;
            double t2 = now_us();

            /* per-object comparison against the hand formula */
            unsigned char *kept = (unsigned char*)calloc((size_t)n, 1);
            if (!kept) return 1;
            for (int i = 0; i < r.count; ++i) kept[r.ids[i]] = 1;
            int false_cull_hand = 0, under_cull_hand = 0;
            int false_cull_exact = 0, under_cull_exact = 0, hand_vs_exact = 0;
            for (int i = 0; i < n; ++i) {
                if (!kept[i] && hand[i]) ++false_cull_hand;  /* wrongly removed  */
                if (kept[i] && !hand[i]) ++under_cull_hand;  /* conservatively kept */
                if (!kept[i] && exact[i]) ++false_cull_exact;
                if (kept[i] && !exact[i]) ++under_cull_exact;
                if ((unsigned char)hand[i] != exact[i]) ++hand_vs_exact;
            }
            printf("  res %4dx%-4d  culled %5d (%.1f%%)  false-cull %d  "
                   "under-cull %d (%.1f%% of exact)  exact/hand mismatch %d  "
                   "stage1 %8.1f us  stage2 %8.1f us  total %8.1f us\n",
                   res, res, r.culled, 100.0 * (double)r.culled / (double)n,
                   false_cull_hand, under_cull_hand,
                   hand_occluded_n ? 100.0 * (double)under_cull_hand / (double)hand_occluded_n : 0.0,
                   hand_vs_exact,
                   t1 - t0, t2 - t1, t2 - t0);
            if (k == nres - 1)
                printf("  per-object check: hand agrees with exact on %d/%d; "
                       "depth-map false culls vs exact %d\n",
                       n - hand_vs_exact, n, false_cull_exact);
            (void)under_cull_exact;

            free(kept);
            cull_occlusion_free(&r);
            cull_depth_free(&dm);
        }

        /* Show a few individual rows so the aggregate numbers can be spot
         * checked by hand. */
        {
            CullDepthMap dm;
            CullOcclusionResult r;
            int res = res_override > 0 ? res_override : 1024;
            if (cull_depth_init(&dm, res, res, 0, 0, 1024, 1024) != 0) return 1;
            if (cull_depth_render(&dm, occ, 6, NULL) != 0) return 1;
            if (cull_occlusion(&dm, cand, n, 1, &r, NULL) != 0) return 1;
            unsigned char *kept = (unsigned char*)calloc((size_t)n, 1);
            if (!kept) return 1;
            for (int i = 0; i < r.count; ++i) kept[r.ids[i]] = 1;
            printf("  spot check at res %dx%d (first 8 candidates):\n", res, res);
            printf("    %-3s %-22s %-8s %-8s %-8s\n",
                   "#", "rect x,y,w,h", "hand", "depthmap", "exact");
            for (int i = 0; i < n && i < 8; ++i)
                printf("    %-3d (%6.1f,%6.1f,%4.1f,%4.1f) %-8s %-8s %-8s\n",
                       i, cand[i].x, cand[i].y, cand[i].w, cand[i].h,
                       hand[i] ? "visible" : "CULLED",
                       kept[i] ? "visible" : "CULLED",
                       exact[i] ? "visible" : "CULLED");
            /* first culled and first kept rows are the interesting ones */
            int fc = -1, fk = -1;
            for (int i = 0; i < n; ++i) { if (!kept[i] && fc < 0) fc = i; if (kept[i] && fk < 0) fk = i; }
            if (fc >= 0)
                printf("    first culled:  #%d (%6.1f,%6.1f,%4.1f,%4.1f)\n",
                       fc, cand[fc].x, cand[fc].y, cand[fc].w, cand[fc].h);
            if (fk >= 0)
                printf("    first kept:    #%d (%6.1f,%6.1f,%4.1f,%4.1f)\n",
                       fk, cand[fk].x, cand[fk].y, cand[fk].w, cand[fk].h);
            free(kept);
            cull_occlusion_free(&r);
            cull_depth_free(&dm);
        }

        printf("\n");
        free(cand); free(exact); free(hand);
    }

    /* ---- 3. fractional-coordinate scene at many resolutions ------- */
    /* Walls and sprites on a 0.25 grid, 1024 units wide, resolutions that
     * are not divisors of anything.  The number of culls rises with
     * resolution and never exceeds the exact count. */
    {
        Wall fw[4];
        for (int i = 0; i < 4; ++i) {
            fw[i].x = 40.5 + 230.25 * (double)i;
            fw[i].w = 57.75;
            fw[i].depth = 1.0 + 0.5 * (double)i;
        }
        enum { NF = 400 };
        CullSprite cand[NF];
        CullSprite occ[4];
        unsigned char exact[NF];
        char hand[NF];
        for (int i = 0; i < 4; ++i) {
            occ[i].x = fw[i].x; occ[i].y = 0; occ[i].w = fw[i].w;
            occ[i].h = 1024; occ[i].depth = fw[i].depth;
        }
        rng_state = 0x2545F4914F6CDD1DULL;
        for (int i = 0; i < NF; ++i) {
            cand[i].w = 2.25 + 0.25 * (double)ri(0, 20);
            cand[i].h = 2.25 + 0.25 * (double)ri(0, 20);
            cand[i].x = 0.25 * (double)ri(0, (int)((1024.0 - cand[i].w) * 4.0));
            cand[i].y = 0.25 * (double)ri(0, (int)((1024.0 - cand[i].h) * 4.0));
            cand[i].depth = 5.0;
            hand[i] = (char)!hand_occluded(fw, 4, cand[i]);
        }
        if (cull_reference_visible(occ, 4, cand, NF, exact, NULL) != 0) return 1;
        int exact_culled = 0, hand_culled = 0, hand_mismatch = 0;
        for (int i = 0; i < NF; ++i) {
            exact_culled += !exact[i];
            hand_culled += !hand[i];
            hand_mismatch += ((unsigned char)hand[i] != exact[i]);
        }
        printf("== fractional scene: %d candidates, 4 walls, quarter-unit grid ==\n", NF);
        printf("  exact: %d culled   hand formula: %d culled   mismatch %d\n",
               exact_culled, hand_culled, hand_mismatch);
        int fres[7] = {16, 32, 64, 128, 256, 512, 1024};
        for (int k = 0; k < 7; ++k) {
            CullDepthMap dm;
            CullOcclusionResult r;
            if (cull_depth_init(&dm, fres[k], fres[k], 0, 0, 1024, 1024) != 0) return 1;
            if (cull_depth_render(&dm, occ, 4, NULL) != 0) return 1;
            int rc = cull_occlusion(&dm, cand, NF, 1, &r, NULL);
            if (rc == CULL_ERR_EMPTY_DEPTH) {
                /* Correct: no wall covers a whole cell at this resolution, so
                 * there is nothing to test against and the API refuses. */
                printf("  res %4dx%-4d  depth map empty (%d cells) -> CULL_ERR_EMPTY_DEPTH\n",
                       fres[k], fres[k], dm.covered_cells);
                cull_depth_free(&dm);
                continue;
            }
            if (rc != 0) return 1;
            unsigned char kept[NF] = {0};
            for (int i = 0; i < r.count; ++i) kept[r.ids[i]] = 1;
            int fc = 0, uc = 0;
            for (int i = 0; i < NF; ++i) {
                if (!kept[i] && exact[i]) ++fc;
                if (kept[i] && !exact[i]) ++uc;
            }
            printf("  res %4dx%-4d  culled %3d  vs exact %3d  missed(under-cull) %3d  "
                   "false-cull %d\n", fres[k], fres[k], r.culled, exact_culled, uc, fc);
            cull_occlusion_free(&r);
            cull_depth_free(&dm);
        }
        printf("\n");
    }
    return 0;
}
