#define _POSIX_C_SOURCE 199309L
#include "cull.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* deterministic RNG (xorshift64): same scene on every run and machine */
static unsigned long long rng_state = 0x9e3779b97f4a7c15ULL;
static unsigned long long xr(void) {
    unsigned long long x = rng_state;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    rng_state = x;
    return x;
}
static double frand(double lo, double hi) {
    return lo + (hi - lo) * (double)(xr() >> 11) / (double)(1ULL << 53);
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double*)a, y = *(const double*)b;
    return (x > y) - (x < y);
}
static double median(double *v, int n) {
    qsort(v, (size_t)n, sizeof(double), cmp_double);
    return v[n / 2];
}

static int same_ids(const int *a, int na, const int *b, int nb) {
    if (na != nb) return 0;
    return na == 0 || memcmp(a, b, sizeof(int) * (size_t)na) == 0;
}

/* Elements that differ between two ascending id lists. */
static long sym_diff(const int *a, int na, const int *b, int nb) {
    int i = 0, j = 0;
    long d = 0;
    while (i < na && j < nb) {
        if (a[i] == b[j]) { ++i; ++j; }
        else if (a[i] < b[j]) { ++i; ++d; }
        else { ++j; ++d; }
    }
    return d + (na - i) + (nb - j);
}

static CullBox *make_scene(int n) {
    CullBox *b = (CullBox*)malloc(sizeof(CullBox) * (size_t)n);
    if (!b) return NULL;
    rng_state = 0x9e3779b97f4a7c15ULL ^ (unsigned)n;
    for (int i = 0; i < n; ++i) {
        b[i].x = frand(-500.0, 1500.0);
        b[i].y = frand(-500.0, 1500.0);
        b[i].w = frand(4.0, 120.0);
        b[i].h = frand(4.0, 120.0);
        b[i].layer = i % 8;
    }
    /* Move the viewport per run? No: fixed so both scales and the Python
     * reference see the same query. */
    return b;
}

static int write_scene(const char *dir, int n, const CullBox *b, CullView v) {
    char path[512];
    snprintf(path, sizeof path, "%s/cull_hash_scene_%d.txt", dir, n);
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    fprintf(f, "view %.17g %.17g %.17g %.17g\n", v.x, v.y, v.w, v.h);
    fprintf(f, "n %d\n", n);
    for (int i = 0; i < n; ++i)
        fprintf(f, "box %d %.17g %.17g %.17g %.17g %d\n",
                i, b[i].x, b[i].y, b[i].w, b[i].h, b[i].layer);
    fclose(f);
    return 1;
}

static int write_result(const char *dir, int n, const CullResult *brute,
                        const CullHashResult *hash) {
    char path[512];
    snprintf(path, sizeof path, "%s/cull_hash_result_%d.txt", dir, n);
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    fprintf(f, "brute %d", brute->count);
    for (int i = 0; i < brute->count; ++i) fprintf(f, " %d", brute->ids[i]);
    fprintf(f, "\n");
    fprintf(f, "hash %d", hash->count);
    for (int i = 0; i < hash->count; ++i) fprintf(f, " %d", hash->ids[i]);
    fprintf(f, "\n");
    fprintf(f, "stats buckets=%d occupied=%d entries=%ld collisions=%ld "
               "collision_rate=%.2f candidates=%ld visited_buckets=%d\n",
            hash->stats.buckets, hash->stats.occupied, hash->stats.entries,
            hash->stats.collisions, hash->stats.collision_rate,
            hash->candidates, hash->visited_buckets);
    fclose(f);
    return 1;
}

static int same_file(const char *pa, const char *pb, long *len_out) {
    FILE *fa = fopen(pa, "rb"), *fb = fopen(pb, "rb");
    if (!fa || !fb) { if (fa) fclose(fa); if (fb) fclose(fb); return 0; }
    int same = 1;
    long len = 0;
    unsigned char ba[65536], bb[65536];
    for (;;) {
        size_t na = fread(ba, 1, sizeof ba, fa);
        size_t nb = fread(bb, 1, sizeof bb, fb);
        if (na != nb || memcmp(ba, bb, na) != 0) { same = 0; break; }
        len += (long)na;
        if (na == 0) break;
    }
    fclose(fa); fclose(fb);
    if (len_out) *len_out = len;
    return same;
}

#define REPS 9

static int run_scale(int n, const char *dir) {
    CullView view = {0.0, 0.0, 320.0, 240.0};
    CullBox *boxes = make_scene(n);
    if (!boxes) { printf("out of memory\n"); return 1; }

    /* Reference: the original per-object scan. */
    CullResult brute;
    if (cull_run(boxes, n, view, &brute) != 0) { free(boxes); return 1; }

    CullHashOpts opts;
    memset(&opts, 0, sizeof opts);
    CullHashError err;
    CullHash *h = NULL;
    double t_build[REPS], t_query[REPS], t_legacy[REPS], t_total[REPS];
    int built = 0;
    enum { QREPS = 200 };
    for (int r = 0; r < REPS; ++r) {
        double t0 = now_s();
        int rc = cull_hash_build(boxes, n, &opts, &h, &err);
        double t1 = now_s();
        if (rc != CULL_OK) {
            printf("build failed %d: %s\n", rc, err.message);
            free(brute.ids); free(boxes);
            return 1;
        }
        /* Query timing: built once, queried QREPS times (single calls are
         * below timer resolution at the small scale). */
        CullHashResult q;
        double t2 = now_s();
        for (int k = 0; k < QREPS; ++k) {
            if (k > 0) cull_hash_result_free(&q);
            rc = cull_hash_query(h, view, &q, &err);
            if (rc != CULL_OK) break;
        }
        double t3 = now_s();
        if (rc != CULL_OK) {
            printf("query failed %d: %s\n", rc, err.message);
            cull_hash_free(h); free(brute.ids); free(boxes);
            return 1;
        }
        /* correctness is independent of the repetition */
        if (!same_ids(brute.ids, brute.count, q.ids, q.count)) {
            printf("FAIL hash != brute at n=%d (diff %ld)\n", n,
                   sym_diff(brute.ids, brute.count, q.ids, q.count));
            built = -1;
        }
        t_build[r] = t1 - t0;
        t_query[r] = (t3 - t2) / QREPS;
        t_total[r] = t_build[r] + t_query[r];

        CullHashResult legacy;
        CullHashOpts lo = opts;
        lo.legacy = 1;
        double t4 = now_s();
        for (int k = 0; k < QREPS; ++k) {
            if (k > 0) cull_hash_result_free(&legacy);
            rc = cull_run_hashed(boxes, n, view, &lo, &legacy, &err);
            if (rc != CULL_OK) break;
        }
        double t5 = now_s();
        if (rc != CULL_OK || !same_ids(brute.ids, brute.count,
                                       legacy.ids, legacy.count)) {
            printf("FAIL legacy path diverges at n=%d\n", n);
            built = -1;
        }
        t_legacy[r] = (t5 - t4) / QREPS;
        cull_hash_result_free(&legacy);
        cull_hash_result_free(&q);
        if (r + 1 < REPS) { cull_hash_free(h); h = NULL; }
    }
    (void)built;
    if (!h) { free(brute.ids); free(boxes); return 1; }

    /* Final query for reporting and export. */
    CullHashResult q;
    if (cull_hash_query(h, view, &q, &err) != CULL_OK) {
        cull_hash_free(h); free(brute.ids); free(boxes); return 1;
    }
    long diff = sym_diff(brute.ids, brute.count, q.ids, q.count);

    char p_buckets[512], p_buckets2[512], p_result[512];
    snprintf(p_buckets, sizeof p_buckets, "%s/cull_hash_buckets_%d.txt", dir, n);
    snprintf(p_buckets2, sizeof p_buckets2, "%s/cull_hash_buckets_%d_repeat.txt", dir, n);
    snprintf(p_result, sizeof p_result, "%s/cull_hash_result_%d.txt", dir, n);

    if (cull_hash_export_buckets(h, p_buckets) != CULL_OK) {
        printf("bucket export failed\n");
        cull_hash_result_free(&q); cull_hash_free(h); free(brute.ids); free(boxes);
        return 1;
    }
    /* Rebuild from scratch and export again; the tables must be identical. */
    CullHash *h2 = NULL;
    int reproducible = 0;
    long table_bytes = 0;
    if (cull_hash_build(boxes, n, &opts, &h2, &err) == CULL_OK &&
        cull_hash_export_buckets(h2, p_buckets2) == CULL_OK) {
        reproducible = same_file(p_buckets, p_buckets2, &table_bytes);
    }
    cull_hash_free(h2);
    int wrote = write_scene(dir, n, boxes, view) &&
                write_result(dir, n, &brute, &q);

    double base = median(t_legacy, REPS);
    if (!(base > 0.0)) base = 1e-12;
    printf("\n================ n = %d，视口 (%.0f,%.0f,%.0f,%.0f) ================\n",
           n, view.x, view.y, view.w, view.h);
    printf("网格桶数 %d，非空桶 %d，插入数 %ld，冲突数 %ld，冲突率 %.2f%%\n",
           q.stats.buckets, q.stats.occupied, q.stats.entries,
           q.stats.collisions, q.stats.collision_rate);
    printf("可见 %d（参考工具 %d，误差 %ld），候选 %ld，访问桶 %d\n",
           q.count, brute.count, diff, q.candidates, q.visited_buckets);
    printf("候选缩减: %d -> %ld（%.2fx 更少测试）\n",
           n, q.candidates, (double)n / (double)(q.candidates ? q.candidates : 1));
    printf("阶段 | 中位耗时ms | 相对参考工具\n");
    printf("建索引（第一段） | %.4f | %.2fx\n",
           median(t_build, REPS) * 1e3, median(t_build, REPS) / base);
    printf("查询（第二段） | %.4f | %.2fx\n",
           median(t_query, REPS) * 1e3, median(t_query, REPS) / base);
    printf("两段合计 | %.4f | %.2fx\n",
           median(t_total, REPS) * 1e3, median(t_total, REPS) / base);
    printf("参考工具（逐对象） | %.4f | 1.00x\n", base * 1e3);
    {
        /* Build once, query many: how many queries until the two-stage
         * path beats calling the reference every time? */
        double q = median(t_query, REPS), b = median(t_build, REPS);
        double per = base - q;
        double breakeven = per > 1e-12 ? b / per : -1.0;
        printf("摊薄：建索引后每次查询 %.4f ms；单次净收益 %.4f ms；"
               "相对逐对象每次调用，约 %.0f 次查询后回本\n",
               q * 1e3, per * 1e3, breakeven);
        if (breakeven > 0.0) {
            int K = (int)(breakeven + 0.5);
            if (K < 1) K = 1;
            printf("      同一场景每帧查询、每 %d 帧重建索引时，均摊 %.4f ms/帧 "
                   "（参考工具 %.4f ms/帧）\n",
                   K, (b / K + q) * 1e3, base * 1e3);
        }
    }
    printf("桶表两次构建逐字节一致: %s（%ld 字节）\n",
           reproducible ? "是" : "否", table_bytes);
    printf("结果导出: %s\n", wrote ? p_result : "(写入失败)");

    int ok = (diff == 0) && reproducible && wrote &&
             same_ids(brute.ids, brute.count, q.ids, q.count);
    printf("本规模判定: %s\n", ok ? "PASS" : "FAIL");

    cull_hash_result_free(&q);
    cull_hash_free(h);
    free(brute.ids);
    free(boxes);
    return ok ? 0 : 1;
}

int main(int argc, char **argv) {
    const char *dir = (argc > 1) ? argv[1] : "../artifacts";
    int failures = 0;
    failures |= run_scale(1000, dir);
    failures |= run_scale(10000, dir);
    printf("\n总判定: %s\n", failures ? "存在 FAIL" : "全部 PASS");
    return failures ? 1 : 0;
}
