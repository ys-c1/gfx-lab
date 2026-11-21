#define _POSIX_C_SOURCE 199309L
#include "cull.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* 确定性场景，便于逐对象手算与 Python 独立复核：
 *   - 对象 i 的 id = i，盒为网格单元，视口覆盖每个网格单元右上角。
 *   - 对象 i 在候选里的重复次数 k_i = 1 + (i % 4)，全部可见对象都出现。
 *   - 候选顺序为确定性错排（xorshift64），同对象多次出现。
 * 手算真值：
 *   visible = cull_count_within(...)
 *   candidates = sum k_i (可见对象)
 *   unique = visible
 *   merge = candidates - unique
 */
static unsigned long long rs;
static unsigned long long xr(void) {
    unsigned long long x = rs;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    return rs = x;
}

static int cmp_double(const void *a, const void *b) {
    double da = *(const double*)a, db = *(const double*)b;
    return (da > db) - (da < db);
}

static double med_ms(struct timespec *t, int runs) {
    double *m = (double*)malloc(sizeof(double) * (size_t)runs);
    for (int i = 0; i < runs; ++i)
        m[i] = (t[i*2+1].tv_sec - t[i*2].tv_sec) * 1e3
             + (t[i*2+1].tv_nsec - t[i*2].tv_nsec) / 1e6;
    qsort(m, (size_t)runs, sizeof(double), cmp_double);
    double r = m[runs / 2];
    free(m);
    return r;
}

#define RUNS 7
#define GRID 2000
#define CELL 16.0

static void run_n(int n, const char *dir) {
    CullObject *objs = (CullObject*)malloc(sizeof(CullObject) * (size_t)n);
    CullBox *boxes = (CullBox*)malloc(sizeof(CullBox) * (size_t)n);
    for (int i = 0; i < n; ++i) {
        objs[i].id = i;
        objs[i].box.x = (double)(i % GRID) * CELL;
        objs[i].box.y = (double)(i / GRID) * CELL;
        objs[i].box.w = CELL * 0.8;
        objs[i].box.h = CELL * 0.8;
        objs[i].box.layer = i % 4;
        boxes[i] = objs[i].box;
    }
    /* 视口覆盖每个单元右上角 1/4：两种盒都会与视口相交 */
    CullView view = {0, 0, (double)GRID * CELL, (double)((n + GRID - 1) / GRID) * CELL};

    CullMap map; int ei;
    if (cull_map_build(&map, objs, n, &ei) != CULL_OK) { printf("map fail idx=%d\n", ei); exit(1); }

    /* 先按视口剔除得到可见对象，再按 k_i 展开候选 */
    CullResult vis;
    if (cull_run(boxes, n, view, &vis) != 0) { printf("cull fail\n"); exit(1); }

    int ncand = 0;
    for (int v = 0; v < vis.count; ++v) ncand += 1 + (vis.ids[v] % 4);
    int *cand = (int*)malloc(sizeof(int) * (size_t)ncand);
    int *order = (int*)malloc(sizeof(int) * (size_t)ncand);
    int w = 0;
    for (int v = 0; v < vis.count; ++v) {
        int id = vis.ids[v], k = 1 + (id % 4);
        for (int j = 0; j < k; ++j) cand[w++] = id;
    }
    /* 确定性错排，保留同对象多次出现 */
    rs = 0x9e3779b97f4a7c15ULL ^ (unsigned long long)n;
    for (int i = 0; i < ncand; ++i) order[i] = i;
    for (int i = ncand - 1; i > 0; --i) {
        int j = (int)(xr() % (unsigned long long)(i + 1));
        int t = order[i]; order[i] = order[j]; order[j] = t;
    }
    int *shuf = (int*)malloc(sizeof(int) * (size_t)ncand);
    for (int i = 0; i < ncand; ++i) shuf[i] = cand[order[i]];
    free(cand); free(order);

    /* 手算真值 */
    long hand_cand = 0, hand_merge = 0;
    for (int v = 0; v < vis.count; ++v) {
        int k = 1 + (vis.ids[v] % 4);
        hand_cand += k;
        hand_merge += k - 1;
    }
    long hand_unique = vis.count;

    /* 计时：合并关/合并开，各取中位数 */
    struct timespec ts[RUNS * 2];
    CullDrawList d = {0};
    double frame_off, frame_on;
    for (int r = 0; r < RUNS; ++r) {
        clock_gettime(CLOCK_MONOTONIC, &ts[r*2]);
        if (cull_draw_list(shuf, ncand, &map, 0, &d, &ei) != CULL_OK) exit(1);
        clock_gettime(CLOCK_MONOTONIC, &ts[r*2+1]);
        cull_draw_list_free(&d);
    }
    frame_off = med_ms(ts, RUNS);

    for (int r = 0; r < RUNS; ++r) {
        clock_gettime(CLOCK_MONOTONIC, &ts[r*2]);
        if (cull_draw_list(shuf, ncand, &map, 1, &d, &ei) != CULL_OK) exit(1);
        clock_gettime(CLOCK_MONOTONIC, &ts[r*2+1]);
        cull_draw_list_free(&d);
    }
    frame_on = med_ms(ts, RUNS);

    /* 单独量合并阶段（阶段 1+2，不含映射校验） */
    double merge_ms;
    int *uniq = (int*)malloc(sizeof(int) * (size_t)ncand);
    int *oids = (int*)malloc(sizeof(int) * (size_t)ncand);
    int *ofirst = (int*)malloc(sizeof(int) * (size_t)ncand);
    for (int r = 0; r < RUNS; ++r) {
        int uc = 0;
        clock_gettime(CLOCK_MONOTONIC, &ts[r*2]);
        if (cull_merge_ids(shuf, ncand, uniq, &uc, &ei) != CULL_OK) exit(1);
        if (cull_order_original(shuf, ncand, uniq, uc, oids, ofirst, &ei) != CULL_OK) exit(1);
        clock_gettime(CLOCK_MONOTONIC, &ts[r*2+1]);
    }
    merge_ms = med_ms(ts, RUNS);

    /* 最终结果 + 逐对象核对 */
    CullDrawList fin;
    if (cull_draw_list(shuf, ncand, &map, 1, &fin, &ei) != CULL_OK) { printf("draw fail\n"); exit(1); }

    long err_calls = (long)fin.count - hand_unique;       /* 绘制调用数误差 */
    long err_merge = hand_merge - (hand_cand - (long)fin.count);

    /* 逐对象：候选出现次数 vs 手算 k_i；合并后恰好出现一次 */
    int *seen = (int*)calloc((size_t)n, sizeof(int));
    int *seen_out = (int*)calloc((size_t)n, sizeof(int));
    int *first_seen = (int*)malloc(sizeof(int) * (size_t)n);
    for (int i = 0; i < n; ++i) first_seen[i] = -1;
    for (int i = 0; i < ncand; ++i) {
        ++seen[shuf[i]];
        if (first_seen[shuf[i]] < 0) first_seen[shuf[i]] = i;
    }
    int bad_per_obj = 0;
    int prev_first = -1, order_ok = 1;
    for (int i = 0; i < fin.count; ++i) {
        int id = fin.ids[i];
        ++seen_out[id];
        if (seen_out[id] != 1) { ++bad_per_obj; continue; }
        int k = 1 + (id % 4);
        if (seen[id] != k) ++bad_per_obj;
        if (fin.first[i] != first_seen[id]) ++bad_per_obj;
        if (fin.first[i] <= prev_first) order_ok = 0;
        prev_first = fin.first[i];
    }
    for (int i = 0; i < n; ++i)
        if (seen[i] > 0 && seen_out[i] != 1) ++bad_per_obj;

    printf("\n===== n=%d 对象，候选 %d，%d 次取中位数 =====\n", n, ncand, RUNS);
    printf("%-30s %14s %14s\n", "指标", "实测", "逐对象手算");
    printf("%-30s %14d %14ld\n", "候选/绘制调用(合并关)", ncand, hand_cand);
    printf("%-30s %14d %14ld\n", "绘制调用(合并开)", fin.count, hand_unique);
    printf("%-30s %14ld %14ld\n", "合并条数(单独)", (long)ncand - (long)fin.count, hand_merge);
    printf("%-30s %14.4f %14s\n", "帧内绘制列表耗时-合并关 ms", frame_off, "-");
    printf("%-30s %14.4f %14s\n", "帧内绘制列表耗时-合并开 ms", frame_on, "-");
    printf("%-30s %14.4f %14s\n", "合并阶段耗时 ms", merge_ms, "-");
    printf("%-30s %14ld %14s\n", "调用数误差", err_calls, "0");
    printf("%-30s %14ld %14s\n", "合并条数误差", err_merge, "0");
    printf("逐对象核对: %d 处不符，输出顺序%s\n", bad_per_obj, order_ok ? "与原顺序一致(首次出现)" : "顺序错误");

    /* 落盘给 Python -I 独立复核 */
    char path[1024];
    snprintf(path, sizeof path, "%s/cull_cand_%d.txt", dir, n);
    FILE *f = fopen(path, "w");
    fprintf(f, "n %d cand %d visible %d view %.6f %.6f %.6f %.6f\n",
            n, ncand, vis.count, view.x, view.y, view.w, view.h);
    fprintf(f, "drawlist %d", fin.count);
    for (int i = 0; i < fin.count; ++i) fprintf(f, " %d", fin.ids[i]);
    fprintf(f, "\nfirst %d", fin.count);
    for (int i = 0; i < fin.count; ++i) fprintf(f, " %d", fin.first[i]);
    fprintf(f, "\ncandidates %d", ncand);
    for (int i = 0; i < ncand; ++i) fprintf(f, " %d", shuf[i]);
    fprintf(f, "\n");
    fclose(f);
    printf("written %s\n", path);

    free(fin.ids); free(fin.first);
    free(uniq); free(oids); free(ofirst);
    free(seen); free(seen_out); free(first_seen);
    free(shuf); free(vis.ids);
    cull_map_free(&map); free(objs); free(boxes);
}

int main(int argc, char **argv) {
    const char *dir = (argc > 1) ? argv[1] : "../artifacts";
    run_n(10000, dir);
    run_n(100000, dir);
    return 0;
}
