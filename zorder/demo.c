#define _POSIX_C_SOURCE 199309L
#include "zorder.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* 确定性伪随机，保证三种策略面对同一份面积数据 */
static unsigned rng_state = 20261121u;
static unsigned rng_next(void) {
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17; rng_state ^= rng_state << 5;
    return rng_state;
}

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

typedef struct { const char *name; int strategy; } Strategy;

static void run_case(int w, int h, int reps, FILE *out) {
    int n = w * h;
    long *areas = (long*)malloc(sizeof(long) * (size_t)n);
    for (int i = 0; i < n; ++i) areas[i] = 64 + (long)(rng_next() % 4096);

    Strategy st[3] = {{"morton", ZO_MORTON}, {"area", ZO_AREA}, {"none", ZO_NONE}};
    printf("\n== grid %dx%d, tiles=%d (reps=%d) ==\n", w, h, n, reps);
    printf("%-7s %10s %12s %12s %12s %12s\n", "layout", "jumps_all", "jumps_win", "span_ratio", "occupancy", "us/run");
    if (out) fprintf(out, "grid %dx%d tiles=%d\n", w, h, n);
    if (out) fprintf(out, "%-7s %10s %12s %12s %12s %12s\n", "layout", "jumps_all", "jumps_win", "span_ratio", "occupancy", "us/run");

    for (int s = 0; s < 3; ++s) {
        ZoLayout lay;
        double t0 = now_sec();
        for (int r = 0; r < reps; ++r) {
            if (zo_layout(w, h, areas, st[s].strategy, &lay) != 0) { printf("layout error\n"); exit(1); }
            zo_free(&lay);
        }
        double dt = now_sec() - t0;
        if (zo_layout(w, h, areas, st[s].strategy, &lay) != 0) { printf("layout error\n"); exit(1); }

        double jw, span, occ;
        if (zo_window_stats(&lay, 8, 8, 4, 4, &jw, &span, &occ) != 0) { printf("stats error\n"); exit(1); }

        double us_per_run = dt * 1e6 / (double)reps;
        printf("%-7s %10d %12.2f %12.4f %12.4f %12.2f\n", st[s].name, lay.jumps, jw, span, occ, us_per_run);
        if (out) fprintf(out, "%-7s %10d %12.2f %12.4f %12.4f %12.2f\n",
                         st[s].name, lay.jumps, jw, span, occ, us_per_run);

        /* 抽样展示前 12 个存放位置对应的原图块序号 */
        printf("  order[0..11]:");
        if (out) { fprintf(out, "order[0..11]:"); }
        for (int i = 0; i < 12 && i < lay.n; ++i) {
            printf(" %d", lay.order[i]);
            if (out) fprintf(out, " %d", lay.order[i]);
        }
        printf("\n");
        if (out) fputc('\n', out);
        zo_free(&lay);
    }
    free(areas);
}

int main(void) {
    FILE *out = fopen("demo.txt", "w");
    if (!out) { printf("cannot open demo.txt\n"); return 1; }
    printf("zorder tile reorder benchmark (window 8x8, stride 4, cache line = 4 tiles)\n");
    fprintf(out, "zorder tile reorder benchmark (window 8x8, stride 4, cache line = 4 tiles)\n");
    run_case(16, 16, 2000, out);   /* 256 图块 */
    run_case(32, 32, 500, out);    /* 1024 图块 */
    fclose(out);
    printf("\nwritten demo.txt\n");
    return 0;
}
