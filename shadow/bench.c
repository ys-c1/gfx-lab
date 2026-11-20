/* Benchmark: full re-render (baseline) vs two-phase incremental re-render.
 * For map sizes 128 and 512 the light orbits the scene; per frame we time a
 * full sh_render and an incremental sh_render_incr, count recomputed blocks,
 * and measure the error of the incremental frame against the full one. */
#define _POSIX_C_SOURCE 199309L
#include "shadow.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define FRAMES 128

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void print_table(const ShIncrStats *st) {
    for (int y = 0; y < st->bh; ++y) {
        printf("    ");
        for (int x = 0; x < st->bw; ++x)
            putchar(st->tab[(size_t)y * st->bw + x] ? '#' : '.');
        putchar('\n');
    }
}

static int run_size(int sz, int block) {
    ShadowMap full, incr;
    ShBlocks blk;
    if (sh_init(&full, sz, sz) || sh_init(&incr, sz, sz)) return 1;
    if (sh_blocks_init(&blk, sz, sz, block)) return 1;

    /* occluders scaled to the map size */
    float s = (float)sz;
    ShSeg segs[5] = {
        {0.15f*s, 0.45f*s, 0.45f*s, 0.45f*s},
        {0.55f*s, 0.55f*s, 0.85f*s, 0.55f*s},
        {0.30f*s, 0.70f*s, 0.30f*s, 0.90f*s},
        {0.70f*s, 0.20f*s, 0.70f*s, 0.35f*s},
        {0.40f*s, 0.80f*s, 0.60f*s, 0.80f*s},
    };
    const int nseg = 5;
    float cx = 0.5f * s, cy = 0.5f * s, orb = 0.30f * s, lr = 0.25f * s;

    ShLight prev = {cx + orb, cy, lr, 1.0f};
    if (sh_render(&incr, &prev, segs, nseg)) return 1;   /* frame 0: full seed */

    double t_full = 0.0, t_incr = 0.0;
    long hit_total = 0;
    int hit_max = 0;
    float err_max = 0.0f;
    double err_sum = 0.0;
    ShIncrStats st, st_mid = {0, 0, 0, 0};
    unsigned char *mid_copy = 0;

    for (int f = 1; f < FRAMES; ++f) {
        float a = 2.0f * (float)M_PI * (float)f / (float)FRAMES;
        ShLight cur = {cx + orb * cosf(a), cy + orb * sinf(a), lr, 1.0f};

        double t0 = now_ms();
        if (sh_render(&full, &cur, segs, nseg)) return 1;   /* baseline */
        double t1 = now_ms();
        if (sh_render_incr(&incr, &blk, &prev, &cur, segs, nseg, &st)) return 1;
        double t2 = now_ms();

        t_full += t1 - t0;
        t_incr += t2 - t1;
        hit_total += st.hit;
        if (st.hit > hit_max) hit_max = st.hit;

        if (f == FRAMES / 2) {                              /* keep one block table */
            mid_copy = (unsigned char*)malloc((size_t)st.bw * st.bh);
            for (int i = 0; i < st.bw * st.bh; ++i) mid_copy[i] = st.tab[i];
            st_mid = st;
        }

        for (int i = 0; i < sz * sz; ++i) {                 /* error vs baseline */
            float d = fabsf(incr.buf[i] - full.buf[i]);
            if (d > err_max) err_max = d;
            err_sum += d;
        }
        prev = cur;
    }

    int frames = FRAMES - 1;
    int nblocks = blk.bw * blk.bh;
    double npix = (double)sz * sz;
    printf("size %dx%d, block %d (%dx%d = %d blocks), %d frames\n",
           sz, sz, block, blk.bw, blk.bh, nblocks, frames);
    printf("  recomputed blocks: avg %.1f/frame, max %d, total %ld of %d (%.1f%% of blocks, ~%.1f%% of pixels)\n",
           (double)hit_total / frames, hit_max, hit_total, nblocks,
           100.0 * (double)hit_total / ((double)nblocks * frames),
           100.0 * (double)hit_total * block * block / (npix * frames));
    printf("  time: full %.2f ms total (%.3f ms/frame), incr %.2f ms total (%.3f ms/frame), speedup %.1fx\n",
           t_full, t_full / frames, t_incr, t_incr / frames, t_full / t_incr);
    printf("  error vs full: max %.5f, mean %.6f\n", err_max, err_sum / (npix * frames));
    if (mid_copy) {
        printf("  block table at frame %d (# = recomputed, %d hit):\n", FRAMES / 2, st_mid.hit);
        ShIncrStats show = st_mid; show.tab = mid_copy;
        print_table(&show);
        free(mid_copy);
    }
    printf("\n");

    sh_blocks_free(&blk);
    sh_free(&full); sh_free(&incr);
    return 0;
}

int main(void) {
    if (run_size(128, 8)) return 1;
    if (run_size(512, 16)) return 1;
    return 0;
}
