/* Two-stage trim + pack demo and benchmark.
 *
 * A deterministic set of 64 tiles, each with a 1..4 px transparent border, is
 * packed at atlas widths 128 and 512 in four configurations:
 *
 *   no trim / shelf   : whole tiles, next-fit shelf (the legacy layout)
 *   trim  / shelf     : alpha-trimmed tiles, next-fit shelf
 *   trim  / greedy    : alpha-trimmed tiles, FFDH (tallest first)
 *
 * Reported per configuration: used pixels (sum of placed areas), saved pixels
 * versus the untiled source, occupancy used / (width * height) and mean build
 * time.  The benchmark also reports a lower bound on the optimal strip height
 * so the residual gap is visible, and prints the classical guarantee for
 * first-fit decreasing height (FFDH <= NFDH <= 2*OPT + h_max).
 */
#define _POSIX_C_SOURCE 200809L
#include "atlas.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NTILE 64
#define REPS 2000

typedef struct { int w, h; unsigned char *rgba; } Tile;

static unsigned int rng_state = 0x9e3779b9u;
static unsigned int rnd(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

/* w x h tile whose opaque rectangle is inset by a per-side transparent
 * border. */
static unsigned char *mk_bordered(int w, int h, int bl, int bt, int br, int bb) {
    unsigned char *p = (unsigned char *)calloc((size_t)w * (size_t)h, 4);
    if (!p) return NULL;
    for (int y = bt; y < h - bb; ++y)
        for (int x = bl; x < w - br; ++x)
            p[((size_t)y * (size_t)w + (size_t)x) * 4 + 3] = 255;
    return p;
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

/* Lower bound on the optimal strip height: area bound, tallest tile, and
 * tiles wider than half the strip (two of those can never share a shelf). */
static int height_lb(const AtlasTrim *tr, int n, int width) {
    long long area = 0;
    int maxh = 0, wide = 0;
    for (int i = 0; i < n; ++i) {
        area += (long long)tr[i].w * tr[i].h;
        if (tr[i].h > maxh) maxh = tr[i].h;
        if (tr[i].w * 2 > width) wide += tr[i].h;
    }
    int lb = (int)((area + width - 1) / width);
    if (maxh > lb) lb = maxh;
    if (wide > lb) lb = wide;
    return lb;
}

static void row(const char *label, int width, const AtlasStats *st, int untiled,
                double ms) {
    double occ = st->height > 0 ? (100.0 * st->used_px) / ((double)width * st->height) : 0.0;
    printf("  %-14s used=%6d  saved=%5d  height=%4d  occ=%6.2f%%  shelves=%3d  %7.2f us\n",
           label, st->used_px, untiled - st->used_px, st->height, occ, st->shelves,
           ms * 1000.0);
}

int main(void) {
    Tile tiles[NTILE];
    AtlasTile at[NTILE];
    AtlasTrim trims[NTILE];     /* alpha-trimmed */
    AtlasTrim whole[NTILE];     /* untrimmed control */
    AtlasSlot slots[NTILE];
    AtlasPos legacy[NTILE];
    AtlasRect rects[NTILE];
    AtlasStats st, ls;
    int bad = -1;

    /* Deterministic mix: square icons, wide banners, tall strips. */
    for (int i = 0; i < NTILE; ++i) {
        int kind = i % 3;
        int tw = kind == 0 ? 32 + (int)(rnd() % 33)
               : kind == 1 ? 64 + (int)(rnd() % 65)
                           : 24 + (int)(rnd() % 25);
        int th = kind == 2 ? 64 + (int)(rnd() % 65) : 24 + (int)(rnd() % 41);
        int bl = 1 + (int)(rnd() % 4), bt = 1 + (int)(rnd() % 4);
        int br = 1 + (int)(rnd() % 4), bb = 1 + (int)(rnd() % 4);
        if (bl + br >= tw - 1) br = 0;
        if (bt + bb >= th - 1) bb = 0;
        tiles[i].w = tw; tiles[i].h = th;
        tiles[i].rgba = mk_bordered(tw, th, bl, bt, br, bb);
        if (!tiles[i].rgba) return 1;
        at[i] = (AtlasTile){tw, th, tiles[i].rgba, 0};
        rects[i] = (AtlasRect){tw, th};
        whole[i].tile = i;
        whole[i].src_x = 0; whole[i].src_y = 0;
        whole[i].w = tw; whole[i].h = th;
    }

    long long untiled_ll = 0;
    for (int i = 0; i < NTILE; ++i) untiled_ll += (long long)tiles[i].w * tiles[i].h;
    const int untiled = (int)untiled_ll;

    /* Stage 1, timed on its own. */
    double t0 = now_ms();
    for (int rep = 0; rep < REPS; ++rep) {
        if (atlas_trim(at, NTILE, trims, &bad) != ATLAS_OK) {
            printf("trim failed at tile %d\n", bad);
            return 1;
        }
    }
    double trim_ms = (now_ms() - t0) / REPS;

    long long trimmed_ll = 0;
    for (int i = 0; i < NTILE; ++i) trimmed_ll += (long long)trims[i].w * trims[i].h;
    const int trimmed = (int)trimmed_ll;
    double trim_pct = 100.0 * (double)(untiled - trimmed) / (double)untiled;

    printf("tiles=%d  untiled=%d px  trimmed=%d px  saved=%d px (%.1f%%)  stage1=%.2f us\n\n",
           NTILE, untiled, trimmed, untiled - trimmed, trim_pct, trim_ms * 1000.0);

    int widths[2] = {128, 512};
    for (int k = 0; k < 2; ++k) {
        const int width = widths[k];
        printf("atlas width=%d   lower bound on optimal height=%d\n", width,
               height_lb(trims, NTILE, width));

        /* no trim, shelf control -- must match the legacy atlas_pack layout */
        if (atlas_place(whole, NTILE, width, ATLAS_SHELF, slots, &st, &bad) != ATLAS_OK) {
            printf("  no-trim failed at tile %d\n", bad);
            return 1;
        }
        if (atlas_pack(rects, NTILE, width, legacy, &ls) != 0) return 1;
        for (int i = 0; i < NTILE; ++i)
            if (slots[i].x != legacy[i].x || slots[i].y != legacy[i].y) {
                printf("  LAYOUT MISMATCH at %d\n", i);
                return 1;
            }
        t0 = now_ms();
        for (int rep = 0; rep < REPS; ++rep)
            atlas_place(whole, NTILE, width, ATLAS_SHELF, slots, &st, &bad);
        double t_no = (now_ms() - t0) / REPS;
        row("no trim/shelf", width, &st, untiled, t_no);

        if (atlas_place(trims, NTILE, width, ATLAS_SHELF, slots, &st, &bad) != ATLAS_OK) {
            printf("  trim/shelf failed at tile %d\n", bad);
            return 1;
        }
        t0 = now_ms();
        for (int rep = 0; rep < REPS; ++rep)
            atlas_place(trims, NTILE, width, ATLAS_SHELF, slots, &st, &bad);
        row("trim/shelf", width, &st, untiled, (now_ms() - t0) / REPS);
        int shelf_h = st.height;

        if (atlas_place(trims, NTILE, width, ATLAS_GREEDY, slots, &st, &bad) != ATLAS_OK) {
            printf("  trim/greedy failed at tile %d\n", bad);
            return 1;
        }
        t0 = now_ms();
        for (int rep = 0; rep < REPS; ++rep)
            atlas_place(trims, NTILE, width, ATLAS_GREEDY, slots, &st, &bad);
        row("trim/greedy", width, &st, untiled, (now_ms() - t0) / REPS);
        int greedy_h = st.height;
        int lb = height_lb(trims, NTILE, width);

        /* Error bound.  Since the greedy packing is feasible, OPT <= H, and
         * since LB is a lower bound, OPT >= LB.
         *   (a) simple: H - OPT <= H - LB.
         *   (b) FFDH guarantee  H <= (11/9)OPT + (6/9)h_max
         *       => H - OPT <= (2/11)H + (6/11)h_max.
         * Both are strict upper bounds on how far the greedy height can sit
         * above the optimal one; LB itself is a lower bound on OPT. */
        int hmax = 0;
        for (int i = 0; i < NTILE; ++i) if (trims[i].h > hmax) hmax = trims[i].h;
        double err_simple = (double)(greedy_h - lb);
        double err_ffdh = (2.0 / 11.0) * (double)greedy_h + (6.0 / 11.0) * (double)hmax;
        printf("  height: greedy %d vs shelf %d, OPT in [%d, %d]\n",
               greedy_h, shelf_h, lb, greedy_h);
        printf("  error bound: H-OPT <= %d (H-LB) ; H-OPT <= %.1f (FFDH 11/9 OPT + 6/9 h_max, h_max=%d)\n",
               (int)err_simple, err_ffdh, hmax);
        printf("  observed greedy/LB ratio %.3f (LB may be below OPT)\n\n",
               (double)greedy_h / (double)lb);
    }

    /* End-to-end sanity via the convenience wrapper. */
    if (atlas_build(at, NTILE, 512, 1, ATLAS_GREEDY, slots, &st, &bad) != ATLAS_OK) {
        printf("atlas_build failed at tile %d\n", bad);
        return 1;
    }
    printf("atlas_build(trim+greedy, width=512): used=%d saved=%d height=%d\n",
           st.used_px, st.saved_px, st.height);

    for (int i = 0; i < NTILE; ++i) free(tiles[i].rgba);
    return 0;
}
