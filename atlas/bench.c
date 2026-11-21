#define _POSIX_C_SOURCE 199309L
#include "atlas.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#define N 512
#define REPEAT 20000

static unsigned rng_state = 12345;
static unsigned rng(void) {
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state >> 8;
}

/* 混合尺寸：约 1/8 大图块，其余中小图块，顺序由 LCG 打散 */
static void gen_rects(AtlasRect *r, int n) {
    for (int i = 0; i < n; ++i) {
        if (i % 8 == 0) { r[i].w = 56 + (int)(rng() % 65); r[i].h = 40 + (int)(rng() % 57); }
        else            { r[i].w = 8  + (int)(rng() % 33); r[i].h = 8  + (int)(rng() % 33); }
    }
    for (int i = n - 1; i > 0; --i) { /* Fisher-Yates 打乱 */
        int j = (int)(rng() % (unsigned)(i + 1));
        AtlasRect t = r[i]; r[i] = r[j]; r[j] = t;
    }
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static int atlas_height(const AtlasRect *r, const AtlasPos *p, int n) {
    int h = 0;
    for (int i = 0; i < n; ++i) if (p[i].y + r[i].h > h) h = p[i].y + r[i].h;
    return h;
}

typedef struct { double us; int used, shelves, height, moved; double waste; } Row;

static Row run(const AtlasRect *r, int n, int width, int sort, AtlasPos *pos, AtlasStats *st) {
    Row row;
    int moved = 0;
    double t0 = now_s();
    for (int it = 0; it < REPEAT; ++it)
        atlas_pack2(r, n, width, sort, pos, st, &moved, NULL);
    row.us = (now_s() - t0) * 1e6 / REPEAT;
    row.used = st->used_px;
    row.shelves = st->shelves;
    row.height = atlas_height(r, pos, n);
    row.moved = moved;
    row.waste = 100.0 * (1.0 - (double)row.used / ((double)width * row.height));
    return row;
}

static void report(int width, const Row *orig, const Row *sorted) {
    printf("宽度 %d:\n", width);
    printf("  %-14s %10s %10s %8s %8s %8s %8s\n",
           "", "像素数", "耗时us", "浪费率%", "排序次数", "条带数", "高度");
    printf("  %-14s %10d %10.2f %8.2f %8d %8d %8d\n",
           "原序摆放", orig->used, orig->us, orig->waste, orig->moved, orig->shelves, orig->height);
    printf("  %-14s %10d %10.2f %8.2f %8d %8d %8d\n",
           "面积降序两段", sorted->used, sorted->us, sorted->waste, sorted->moved, sorted->shelves, sorted->height);
    printf("  差异(两段-原序): 浪费率 %+.2f pp, 条带数 %+d, 高度 %+d, 耗时 %+.2f us\n",
           sorted->waste - orig->waste, sorted->shelves - orig->shelves,
           sorted->height - orig->height, sorted->us - orig->us);
}

int main(void) {
    static AtlasRect r[N];
    static AtlasPos pos[N], pos_old[N];
    AtlasStats st, st_old;
    gen_rects(r, N);

    for (int w = 0; w < 2; ++w) {
        int width = w == 0 ? 128 : 1024;
        Row orig = run(r, N, width, ATLAS_SORT_OFF, pos, &st);
        Row sorted = run(r, N, width, ATLAS_SORT_AREA_DESC, pos, &st);
        report(width, &orig, &sorted);

        /* 与旧路径对照：sort 关掉时必须逐块一致 */
        atlas_pack(r, N, width, pos_old, &st_old);
        atlas_pack2(r, N, width, ATLAS_SORT_OFF, pos, &st, NULL, NULL);
        int same = memcmp(pos, pos_old, sizeof(AtlasPos) * N) == 0
                && st.used_px == st_old.used_px && st.shelves == st_old.shelves;
        printf("  旧路径对照(sort=off vs atlas_pack): %s\n\n", same ? "一致" : "不一致!");
        if (!same) return 1;
    }
    return 0;
}
