#define _POSIX_C_SOURCE 199309L
#include "reindex.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* 每张图片的尺寸；像素数按 w*h 累计，与 atlas 的 used_px 口径一致。 */
typedef struct { int w, h; } Img;

static unsigned int lcg_state;
static unsigned int rnd(void) {
    lcg_state = lcg_state * 1103515245u + 12345u;
    return lcg_state;
}

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* 简单指纹，用于对照两条路径的输出文件内容。 */
static unsigned long long fingerprint(const int *a, int m) {
    unsigned long long h = 1469598103934665603ULL;
    for (int i = 0; i < m; ++i) {
        h ^= (unsigned)a[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static void write_refs(const char *path, const int *refs, int m,
                       const Img *imgs, const int *map, int resolved) {
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); return; }
    fprintf(f, "# ref,slot,pixels\n");
    for (int i = 0; i < m; ++i) {
        int slot = resolved ? map[refs[i]] : refs[i];
        fprintf(f, "%d,%d,%d\n", i, refs[i], imgs[slot].w * imgs[slot].h);
    }
    fclose(f);
}

static void run_case(int n, int refs_m, int iters, int inject_conflict) {
    printf("================ N=%d ================\n", n);
    Img *imgs = malloc(sizeof(Img) * (size_t)n);
    int *perm = malloc(sizeof(int) * (size_t)n);
    int *refs = malloc(sizeof(int) * (size_t)refs_m);
    int *old_path = malloc(sizeof(int) * (size_t)refs_m);
    int *new_path = malloc(sizeof(int) * (size_t)refs_m);

    /* 同分布尺寸，保证 128 与 1024 可比。 */
    lcg_state = 0xC0FFEEu;
    long long pixels = 0;
    for (int i = 0; i < n; ++i) {
        imgs[i].w = 8 + (int)(rnd() % 56);
        imgs[i].h = 8 + (int)(rnd() % 56);
        pixels += (long long)imgs[i].w * imgs[i].h;
    }

    /* 一轮索引轮换 + Fisher-Yates，得到旧槽位 -> 新槽位的置换。 */
    for (int i = 0; i < n; ++i) perm[i] = i;
    for (int i = n - 1; i > 0; --i) {
        int j = (int)(rnd() % (unsigned)(i + 1));
        int tmp = perm[i]; perm[i] = perm[j]; perm[j] = tmp;
    }
    int build_conflicts = 0;
    if (inject_conflict && n >= 4) {
        perm[2] = perm[1];        /* 制造一次新槽位重复认领 */
        build_conflicts = 1;
    }

    /* 引用按旧槽位生成。 */
    for (int i = 0; i < refs_m; ++i) refs[i] = (int)(rnd() % (unsigned)n);
    memcpy(old_path, refs, sizeof(int) * (size_t)refs_m);
    memcpy(new_path, refs, sizeof(int) * (size_t)refs_m);

    /* 旧路径：直接沿用旧索引 -> 取错图。统计取错的引用数。 */
    int mismatches = 0;
    for (int i = 0; i < refs_m; ++i)
        if (old_path[i] != perm[old_path[i]]) ++mismatches;

    /* 新路径：阶段一建表 + 阶段二批量改写，计时取平均。 */
    double t0 = now_sec();
    int rewritten = 0, rewrite_conflicts = 0;
    for (int k = 0; k < iters; ++k) {
        memcpy(new_path, refs, sizeof(int) * (size_t)refs_m);
        RxTable table;
        RxErr err = {0, 0, 0};
        int rc = rx_apply(perm, n, new_path, refs_m, 1, &table,
                          &rewritten, &rewrite_conflicts, &err);
        if (rc != RX_OK) {
            printf("rx_apply failed: %s param=%s item=%d\n",
                   rx_strerror(rc), err.param ? err.param : "?", err.item);
            exit(1);
        }
        if (k == 0) {
            char csv[64];
            snprintf(csv, sizeof(csv), "mapping_%d.csv", n);
            rx_export(&table, csv);
            printf("mapping table exported -> %s (length=%d, build_conflicts=%d)\n",
                   csv, table.n, table.conflicts);
        }
        rx_free(&table);
    }
    double elapsed = now_sec() - t0;

    /* 改写后所有引用都解析到正确的新槽位（冲突项除外）。 */
    int still_wrong = 0;
    for (int i = 0; i < refs_m; ++i) {
        int old = refs[i];
        if (perm[old] >= 0 && new_path[i] != perm[old]) ++still_wrong;
    }

    /* 关闭开关必须回到当前输出：逐字节等于旧路径。 */
    int *disabled = malloc(sizeof(int) * (size_t)refs_m);
    memcpy(disabled, refs, sizeof(int) * (size_t)refs_m);
    RxErr derr = {0, 0, 0};
    int drc = rx_apply(perm, n, disabled, refs_m, 0, NULL, NULL, NULL, &derr);
    int toggle_ok = (drc == RX_OK) &&
                    (memcmp(disabled, refs, sizeof(int) * (size_t)refs_m) == 0);
    free(disabled);

    /* 与旧路径输出做对照：写两份文件 + 指纹。 */
    RxTable dbg;
    RxErr e2 = {0, 0, 0};
    rx_build(perm, n, &dbg, &e2);
    char p_old[64], p_new[64];
    snprintf(p_old, sizeof(p_old), "refs_old_%d.txt", n);
    snprintf(p_new, sizeof(p_new), "refs_new_%d.txt", n);
    write_refs(p_old, old_path, refs_m, imgs, perm, 0);
    write_refs(p_new, new_path, refs_m, imgs, perm, 1);

    printf("images=%d  total pixels=%lld  refs=%d\n", n, pixels, refs_m);
    printf("timing: %d iters in %.6f s  (avg %.3f us/iter)\n",
           iters, elapsed, elapsed / iters * 1e6);
    printf("rewritten=%d  rewrite_conflicts=%d  build_conflicts=%d  mapping length=%d\n",
           rewritten, rewrite_conflicts, build_conflicts, dbg.n);
    printf("old path wrong refs=%d ; new path still wrong=%d ; toggle-off byte-identical=%s\n",
           mismatches, still_wrong, toggle_ok ? "yes" : "NO");
    printf("output compare: %s fp=%016llX  vs  %s fp=%016llX\n",
           p_old, fingerprint(old_path, refs_m),
           p_new, fingerprint(new_path, refs_m));
    rx_free(&dbg);
    free(imgs); free(perm); free(refs); free(old_path); free(new_path);
}

int main(void) {
    printf(">>> negative-parameter guard\n");
    int perm[4] = {1, 2, 3, 0};
    int refs[4] = {0, 1, 2, 3};
    RxErr err = {0, 0, 0};
    int rc = rx_apply(perm, 4, refs, -8, 1, NULL, NULL, NULL, &err);
    printf("  m=-8 -> rc=%d (%s), offending param='%s' item=%d\n",
           rc, rx_strerror(rc), err.param, err.item);

    int neg_perm[4] = {1, -2, 3, 0};
    RxTable t;
    err.code = 0;
    rc = rx_build(neg_perm, 4, &t, &err);
    printf("  perm[1]=-2 -> rc=%d (%s), offending param='%s' item=%d\n",
           rc, rx_strerror(rc), err.param, err.item);

    int neg_refs[3] = {0, -5, 2};
    err.code = 0;
    rc = rx_apply(perm, 4, neg_refs, 3, 1, NULL, NULL, NULL, &err);
    printf("  refs[1]=-5 -> rc=%d (%s), offending param='%s' item=%d\n",
           rc, rx_strerror(rc), err.param, err.item);

    const int ITERS = 500;
    /* 128 与 1024 各跑一次；1024 档注入一次建表冲突，演示冲突统计与导出。 */
    run_case(128, 4096, ITERS, 0);
    run_case(1024, 32768, ITERS, 1);

    printf("\nsummary: both cases exported mapping_*.csv and refs_old/new_*.txt for comparison\n");
    return 0;
}
