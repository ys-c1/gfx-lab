#define _POSIX_C_SOURCE 199309L
#include "grid.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* 确定性 LCG */
static unsigned long rng_state;
static double rnd(void) {
    rng_state = rng_state * 6364136223846793005UL + 1442695040888963407UL;
    return (double)(rng_state >> 33) / (double)(1UL << 31);
}

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void emit(FILE *f, const char *line) {
    printf("%s\n", line);
    fprintf(f, "%s\n", line);
}

static void make_scene(GdBox *b, int n, double world, double avg_size) {
    for (int i = 0; i < n; ++i) {
        b[i].w = avg_size * (0.5 + rnd());
        b[i].h = avg_size * (0.5 + rnd());
        b[i].x = rnd() * (world - b[i].w);
        b[i].y = rnd() * (world - b[i].h);
    }
}

static void bench_scale(FILE *f, int n, double world, double avg_size) {
    char line[256];
    GdBox *b = (GdBox *)malloc((size_t)n * sizeof(GdBox));
    rng_state = 0x9e3779b97f4a7c15UL ^ (unsigned long)n;
    make_scene(b, n, world, avg_size);

    /* ---- 第一段：按密度选格宽建网格 ---- */
    GdConfig cfg = {0};
    cfg.minx = 0; cfg.miny = 0; cfg.maxx = world; cfg.maxy = world;
    double t0 = now_sec();
    int rc = grid_pick_cell(&cfg, b, n);
    double t_pick = now_sec() - t0;
    if (rc != 0) { snprintf(line, sizeof line, "pick error %d (%s)", rc, grid_error_field(rc)); emit(f, line); return; }

    GdGrid *g = NULL;
    t0 = now_sec();
    rc = grid_build(&g, b, n, cfg);
    double t_build = now_sec() - t0;
    if (rc != 0) { emit(f, "build failed"); return; }

    GdPairList gp = {0};
    GdStats st = {0};
    t0 = now_sec();
    grid_pairs(g, &gp, &st);
    double t_grid = now_sec() - t0;
    long long table_bytes = grid_table_bytes(g);

    /* ---- 对照 1：暴力遍历（每帧逐个比包围盒） ---- */
    GdPairList bp = {0};
    long long brute_cand = 0;
    t0 = now_sec();
    grid_brute_pairs(b, n, &bp, &brute_cand);
    double t_brute = now_sec() - t0;

    /* ---- 对照 2：层次包围盒 ---- */
    GdPairList vp = {0};
    long long bvh_tests = 0;
    t0 = now_sec();
    grid_bvh_pairs(b, n, &vp, &bvh_tests);
    double t_bvh = now_sec() - t0;

    int eq_b = grid_pairs_equal(&gp, &bp);
    int eq_v = grid_pairs_equal(&vp, &bp);
    long long pairs = (long long)bp.count;
    long long bvh_bytes = grid_bvh_bytes(n);

    snprintf(line, sizeof line, "==== n=%d  world=%.0f  avg_box=%.1f ====", n, world, avg_size);
    emit(f, line);
    snprintf(line, sizeof line, "grid: cell_w=%.3f dims=%dx%d pick=%.3fms build=%.3fms",
             cfg.cell_w, st.dimx, st.dimy, t_pick * 1e3, t_build * 1e3);
    emit(f, line);
    snprintf(line, sizeof line, "  grid : cells_visited=%-10lld candidates=%-12lld pairs=%-10lld err=%-10lld time=%.3fms",
             st.cell_hits, st.candidates, pairs, st.candidates - pairs, t_grid * 1e3);
    emit(f, line);
    snprintf(line, sizeof line, "  brute: cells_visited=%-10s candidates=%-12lld pairs=%-10lld err=%-10lld time=%.3fms",
             "-", brute_cand, pairs, brute_cand - pairs, t_brute * 1e3);
    emit(f, line);
    snprintf(line, sizeof line, "  bvh  : cells_visited=%-10s candidates=%-12lld pairs=%-10lld err=%-10lld time=%.3fms",
             "-", bvh_tests, pairs, bvh_tests - pairs, t_bvh * 1e3);
    emit(f, line);
    snprintf(line, sizeof line, "  match: grid==brute %d, bvh==brute %d (mismatch %d); grid table %lld B (%.2f B/obj), bvh %lld B (%.2f B/obj)",
             eq_b, eq_v, (eq_b && eq_v) ? 0 : 1,
             table_bytes, (double)table_bytes / n,
             bvh_bytes, (double)bvh_bytes / n);
    emit(f, line);

    /* ---- 第二段：增量增删，只更新受影响格 ---- */
    enum { MOVES = 500 };
    t0 = now_sec();
    int done = 0;
    while (done < MOVES) {
        int id = (int)(rnd() * (n - 1));
        if (grid_remove(g, id) != 0) continue; /* 只移动存活对象 */
        b[id].x = rnd() * (world - b[id].w);
        b[id].y = rnd() * (world - b[id].h);
        if (grid_insert(g, id, b[id]) != 0) break;
        ++done;
    }
    double t_update = now_sec() - t0;
    GdStats st2 = {0};
    grid_stats(g, &st2);

    GdPairList gp2 = {0}, bp2 = {0};
    grid_pairs(g, &gp2, NULL);
    grid_brute_pairs(b, n, &bp2, NULL);
    int eq2 = grid_pairs_equal(&gp2, &bp2);
    snprintf(line, sizeof line,
             "  incremental: moves=%d rebuilds=%d updates=%d update_time=%.3fms (%.2fus/op), match=%d",
             done, st2.rebuilds, st2.updates, t_update * 1e3,
             t_update * 1e6 / (double)(st2.updates > 0 ? st2.updates : 1), eq2);
    emit(f, line);
    emit(f, "");

    grid_pairs_free(&gp); grid_pairs_free(&bp); grid_pairs_free(&vp);
    grid_pairs_free(&gp2); grid_pairs_free(&bp2);
    grid_free(g);
    free(b);
}

int main(void) {
    FILE *f = fopen("demo.txt", "w");
    if (!f) { perror("demo.txt"); return 1; }
    emit(f, "uniform grid broad-phase benchmark (AABB pair query)");
    emit(f, "candidates = AABB tests performed; err = candidates - actual pairs");
    emit(f, "");
    bench_scale(f, 10000, 1000.0, 6.0);
    bench_scale(f, 100000, 3000.0, 6.0);
    fclose(f);
    return 0;
}
