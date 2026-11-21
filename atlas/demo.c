#define _POSIX_C_SOURCE 199309L /* clock_gettime */
#include "atlas.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define N 4000 /* 每个宽度下入图的矩形数 */

static unsigned long long rs;
static unsigned rng(void) {
    rs = rs * 6364136223846793005ULL + 1442695040888963407ULL;
    return (unsigned)(rs >> 33);
}

static double now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

/* 跑一次“回收开”的场景；evict 非空时把回收索引号序列拷入 evict（调用方free） */
static void run_recycling(int w, const AtlasRect *rects, Atlas *out, int **evict, long *evict_n) {
    atlas_open(out, w, w, N, 1);
    unsigned long long ts = 999; /* 触摸流独立种子，保证两次运行一致 */
    for (int i = 0; i < N; ++i) {
        if (out->n > 0) {
            ts = ts * 2862933555777941757ULL + 3037000493ULL;
            atlas_touch(out, (int)((ts >> 33) % (unsigned)out->n));
        }
        atlas_add(out, rects[i], NULL);
    }
    *evict_n = out->recycle_log_n;
    *evict = malloc((size_t)(*evict_n ? *evict_n : 1) * sizeof **evict);
    for (long i = 0; i < *evict_n; ++i) (*evict)[i] = out->recycle_log[i];
}

int main(void) {
    /* 负参数：报错并指出是哪一项 */
    AtlasRect bad[6] = {{8,8},{8,8},{8,8},{-4,8},{8,8},{8,8}};
    AtlasPos pb[6];
    AtlasStats sb;
    int item = -1;
    int rc = atlas_pack_ex(bad, 6, 64, pb, &sb, &item);
    printf("[err] atlas_pack_ex rc=%d, negative-size item=%d\n", rc, item);

    for (int w = 128; w <= 1024; w *= 8) {
        /* 确定性矩形流（固定种子，可复现） */
        AtlasRect *rects = malloc(N * sizeof *rects);
        rs = 12345;
        for (int i = 0; i < N; ++i) {
            rects[i].w = 4 + (int)(rng() % (unsigned)(w / 8));
            rects[i].h = 4 + (int)(rng() % 28u);
        }

        /* 旧路径 */
        AtlasPos *p = malloc(N * sizeof *p);
        AtlasStats s;
        double t0 = now_us();
        atlas_pack(rects, N, w, p, &s);
        double t1 = now_us();

        /* 新路径，回收关：必须与旧路径输出一致 */
        Atlas off;
        atlas_open(&off, w, 1 << 24, N, 0);
        double t2 = now_us();
        for (int i = 0; i < N; ++i) atlas_add(&off, rects[i], NULL);
        double t3 = now_us();
        int match = off.used_px == s.used_px && off.shelves == s.shelves;
        for (int i = 0; i < N && match; ++i)
            if (off.blocks[i].pos.x != p[i].x || off.blocks[i].pos.y != p[i].y) match = 0;

        /* 新路径，回收开：跑两次验证回收顺序可复现 */
        Atlas on1, on2;
        int *e1, *e2;
        long n1, n2;
        double t4 = now_us();
        run_recycling(w, rects, &on1, &e1, &n1);
        double t5 = now_us();
        run_recycling(w, rects, &on2, &e2, &n2);
        int repro = n1 == n2;
        for (long i = 0; i < n1 && repro; ++i) if (e1[i] != e2[i]) repro = 0;

        printf("[width=%d] atlas %dx%d, %d rects\n", w, w, w, N);
        printf("  old path      : shelves=%d used_px=%d time=%.0fus\n",
               s.shelves, s.used_px, t1 - t0);
        printf("  new (recy off): shelves=%d used_px=%d time=%.0fus  old-path output: %s\n",
               off.shelves, off.used_px, t3 - t2, match ? "MATCH" : "MISMATCH");
        printf("  new (recy on) : live=%d live_px=%d occupancy=%.1f%% time=%.0fus\n",
               on1.n, on1.used_px, atlas_occupancy(&on1) * 100.0, t5 - t4);
        printf("  recycled      : %ld blocks\n  evicted ids   :", n1);
        for (long i = 0; i < n1 && i < 24; ++i) printf(" %d", e1[i]);
        printf(n1 > 24 ? " ... (%ld total)\n" : "\n", n1);
        printf("  rerun         : eviction order %s\n", repro ? "REPRODUCIBLE" : "DIFFERS");

        free(e1); free(e2);
        atlas_close(&off); atlas_close(&on1); atlas_close(&on2);
        free(rects); free(p);
    }
    return 0;
}
