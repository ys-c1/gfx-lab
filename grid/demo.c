#define _POSIX_C_SOURCE 199309L
#include "grid.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * demo.c - two-pass grid partition driver.
 *
 * Input  (text, one box per line):  x y w h [layer]
 * Usage : ./demo <input> <cols> <rows>
 *
 * Pass 1 inserts the original boxes; pass 2 merges overlapping boxes
 * (union-find, transitive) and re-inserts the merged AABBs.
 * Metrics for both passes and the before/after diff are printed to stdout.
 * Exit code is non-zero when a hard limit is exceeded; the message then
 * names the cap via grid_last_limit().
 */

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static int load_boxes(const char *path, GBox **out, int *n_out) {
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return -1; }
    int cap = 256, n = 0;
    GBox *b = (GBox*)malloc(sizeof(GBox) * (size_t)cap);
    if (!b) { fclose(f); return -1; }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        if (n >= cap) {
            cap *= 2;
            GBox *nb = (GBox*)realloc(b, sizeof(GBox) * (size_t)cap);
            if (!nb) { free(b); fclose(f); return -1; }
            b = nb;
        }
        int layer = 0;
        int got = sscanf(line, "%lf %lf %lf %lf %d",
                         &b[n].x, &b[n].y, &b[n].w, &b[n].h, &layer);
        if (got < 4) continue;
        b[n].layer = layer;
        ++n;
    }
    fclose(f);
    *out = b; *n_out = n;
    return 0;
}

static int run_pass(const char *tag, const GBox *boxes, int n,
                    double gw, double gh, int cols, int rows,
                    int unique,
                    int *dup_out, long long *cand_out, double *sec_out) {
    Grid g;
    double t0 = now_sec();
    int rc = unique
        ? grid_assign_unique(boxes, n, 0.0, 0.0, gw, gh, cols, rows, &g)
        : grid_build(boxes, n, 0.0, 0.0, gw, gh, cols, rows, &g);
    double t1 = now_sec();
    if (rc != 0) {
        fprintf(stderr, "ERROR pass %s: code=%d limit=%s\n",
                tag, rc, grid_last_limit());
        return rc;
    }
    *dup_out = grid_duplicates(&g);
    *cand_out = g.candidates;
    *sec_out = t1 - t0;
    grid_free(&g);
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s <input-boxes.txt> <cols> <rows>\n", argv[0]);
        return 2;
    }
    int cols = atoi(argv[2]), rows = atoi(argv[3]);

    GBox *boxes = NULL;
    int n = 0;
    if (load_boxes(argv[1], &boxes, &n) != 0) return 2;

    /* grid world bounds = bounding square of all input boxes */
    double gw = 1.0, gh = 1.0;
    for (int i = 0; i < n; ++i) {
        double x1 = boxes[i].x + boxes[i].w;
        double y1 = boxes[i].y + boxes[i].h;
        if (x1 > gw) gw = x1;
        if (y1 > gh) gh = y1;
    }

    int dup1; long long cand1; double sec1;
    int rc = run_pass("1(original)", boxes, n, gw, gh, cols, rows, 0,
                      &dup1, &cand1, &sec1);
    if (rc != 0) {
        fprintf(stderr, "hard limit hit: %s (n=%d, cells=%d)\n",
                grid_last_limit(), n, cols * rows);
        free(boxes);
        return 3;
    }

    GridStats st;
    grid_stats_init(&st);
    double m0 = now_sec();
    rc = grid_merge_groups(boxes, n, &st);
    double m1 = now_sec();
    if (rc != 0) {
        fprintf(stderr, "ERROR merge: limit=%s\n", grid_last_limit());
        free(boxes);
        return 3;
    }

    int dup2; long long cand2; double sec2;
    rc = run_pass("2(merged,unique-owner)", st.group_boxes, st.group_count,
                  gw, gh, cols, rows, 1, &dup2, &cand2, &sec2);
    if (rc != 0) {
        fprintf(stderr, "hard limit hit: %s (groups=%d, cells=%d)\n",
                grid_last_limit(), st.group_count, cols * rows);
        grid_stats_free(&st);
        free(boxes);
        return 3;
    }

    /* per-group size distribution for the before/after diff */
    int *gsize = (int*)calloc((size_t)(st.group_count > 0 ? st.group_count : 1),
                              sizeof(int));
    for (int i = 0; i < n; ++i) gsize[st.group_ids[i]]++;
    int multi = 0, max_size = 0;
    for (int g = 0; g < st.group_count; ++g) {
        if (gsize[g] > 1) ++multi;
        if (gsize[g] > max_size) max_size = gsize[g];
    }
    free(gsize);

    printf("input           : %s (n=%d, grid=%dx%d, world=%.1fx%.1f)\n",
           argv[1], n, cols, rows, gw, gh);
    printf("pass1 original  : error=0 time=%.6fs merge_pairs=- dup=%d cand=%lld\n",
           sec1, dup1, cand1);
    printf("merge step      : groups=%d pairs=%d multi_groups=%d max_group=%d time=%.6fs\n",
           st.group_count, st.merge_pairs, multi, max_size, m1 - m0);
    printf("pass2 merged    : error=0 time=%.6fs merge_pairs=- dup=%d cand=%lld\n",
           sec2, dup2, cand2);
    printf("diff before->after: objects %d -> %d (%d merged away), "
           "dup %d -> %d (delta %d), cand %lld -> %lld (delta %lld)\n",
           n, st.group_count, n - st.group_count,
           dup1, dup2, dup2 - dup1,
           cand1, cand2, cand2 - cand1);

    grid_stats_free(&st);
    free(boxes);
    return 0;
}
