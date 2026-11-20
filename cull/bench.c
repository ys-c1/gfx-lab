#define _POSIX_C_SOURCE 199309L
#include "cull.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>

/* 确定性随机（xorshift64），不依赖 rand 实现，便于 Python 侧独立复现。 */
static unsigned long long rng_state;
static unsigned long long xr(void) {
    unsigned long long x = rng_state;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    rng_state = x;
    return x;
}
static double frand(double lo, double hi) {
    return lo + (hi - lo) * (double)(xr() >> 11) / (double)(1ULL << 53);
}

static int has_id(const CullResult *r, int id) {
    for (int i = 0; i < r->count; ++i) if (r->ids[i] == id) return 1;
    return 0;
}

static int cmp_double(const void *a, const void *b) {
    double da = *(const double*)a, db = *(const double*)b;
    return (da > db) - (da < db);
}

static double median_ms(struct timespec *t, int runs) {
    double *m = malloc(sizeof(double) * (size_t)runs);
    for (int i = 0; i < runs; ++i)
        m[i] = (t[i*2+1].tv_sec - t[i*2].tv_sec) * 1e3
             + (t[i*2+1].tv_nsec - t[i*2].tv_nsec) / 1e6;
    qsort(m, (size_t)runs, sizeof(double), cmp_double);
    double med = m[runs / 2];
    free(m);
    return med;
}

static CullView VIEW = {0, 0, 200, 200};
static const double RATIOS[3] = {0.5, 1.0, 1.5};
static const int RUNS = 7;

static void run_n(int n, const char *dir) {
    rng_state = 0x9e3779b97f4a7c15ULL ^ (unsigned)n;
    CullMovingBox *b = malloc(sizeof(CullMovingBox) * (size_t)n);
    CullBox *plain = malloc(sizeof(CullBox) * (size_t)n);
    for (int i = 0; i < n; ++i) {
        b[i].x = frand(-100, 500);
        b[i].y = frand(-100, 500);
        b[i].w = frand(4, 16);
        b[i].h = frand(4, 16);
        b[i].layer = i % 4;
        b[i].speed = frand(3, 50);          /* 像素/帧，保证 >0，不触发零扩展 */
        double ang = frand(0, 6.283185307179586);
        b[i].dx = cos(ang);                 /* 单位方向 */
        b[i].dy = sin(ang);
        plain[i].x = b[i].x; plain[i].y = b[i].y;
        plain[i].w = b[i].w; plain[i].h = b[i].h; plain[i].layer = b[i].layer;
    }

    /* 真值：ratio=1.0 的扫掠结果（与独立 Python 参考实现对照） */
    CullResult truth; CullMotionReport trep; int terr;
    if (cull_motion(b, n, VIEW, 1.0, &truth, &trep, &terr) != CULL_OK) {
        printf("fatal error idx=%d\n", terr); exit(1);
    }

    /* 朴素（不扩展）计时与计数 */
    struct timespec ts[RUNS * 2];
    CullResult naive = {0};
    for (int r = 0; r < RUNS; ++r) {
        clock_gettime(CLOCK_MONOTONIC, &ts[r*2]);
        cull_run(plain, n, VIEW, &naive);
        clock_gettime(CLOCK_MONOTONIC, &ts[r*2+1]);
        if (r + 1 < RUNS) free(naive.ids);
    }
    double t_naive = median_ms(ts, RUNS);

    /* 三档比例计时与计数（ratio=1.0 复用真值结果，仅单独计时一次完整流程） */
    double t_motion[3] = {0};
    CullResult res[3]; CullMotionReport rep[3];
    for (int k = 0; k < 3; ++k) {
        int rc;
        for (int r = 0; r < RUNS; ++r) {
            clock_gettime(CLOCK_MONOTONIC, &ts[r*2]);
            rc = cull_motion(b, n, VIEW, RATIOS[k], &res[k], &rep[k], &terr);
            clock_gettime(CLOCK_MONOTONIC, &ts[r*2+1]);
            if (rc != CULL_OK) { printf("fatal\n"); exit(1); }
            if (r + 1 < RUNS) { free(res[k].ids); cull_report_free(&rep[k]); }
        }
        t_motion[k] = median_ms(ts, RUNS);
    }

    /* 统计：漏剔 = 真值保留但本方案剔除；误剔 = 本方案保留但真值不保留 */
    int naive_miss = 0;
    for (int i = 0; i < truth.count; ++i) if (!has_id(&naive, truth.ids[i])) ++naive_miss;
    int naive_false = naive.count - (truth.count - naive_miss);

    printf("\n===== n=%d 对象，视口(%.0f,%.0f,%.0f,%.0f)，%d 次取中位数 =====\n",
           n, VIEW.x, VIEW.y, VIEW.w, VIEW.h, RUNS);
    printf("真值(扫掠 ratio=1.0)保留 %d 个\n", truth.count);
    printf("%-18s %8s %8s %8s %12s %12s\n",
           "方案", "保留数", "漏剔", "误剔", "总耗时ms", "ns/对象");
    printf("%-18s %8d %8d %8d %12.4f %12.1f\n",
           "朴素(不扩展)", naive.count, naive_miss, naive_false,
           t_naive, t_naive * 1e6 / n);
    for (int k = 0; k < 3; ++k) {
        int miss = 0, falsec = 0;
        for (int i = 0; i < truth.count; ++i)
            if (!has_id(&res[k], truth.ids[i])) ++miss;
        falsec = res[k].count - (truth.count - miss);
        char label[32];
        snprintf(label, sizeof label, "motion ratio=%.1f", RATIOS[k]);
        printf("%-18s %8d %8d %8d %12.4f %12.1f\n",
               label, res[k].count, miss, falsec,
               t_motion[k], t_motion[k] * 1e6 / n);
    }

    /* 落盘：场景 + 各方案保留集合，供 Python -I 独立复核 */
    char path[512];
    snprintf(path, sizeof path, "%s/cull_scene_%d.txt", dir, n);
    FILE *f = fopen(path, "w");
    fprintf(f, "view %.6f %.6f %.6f %.6f\n", VIEW.x, VIEW.y, VIEW.w, VIEW.h);
    fprintf(f, "n %d\n", n);
    for (int i = 0; i < n; ++i)
        fprintf(f, "box %d %.6f %.6f %.6f %.6f %d %.6f %.6f %.6f\n",
                i, b[i].x, b[i].y, b[i].w, b[i].h, b[i].layer,
                b[i].speed, b[i].dx, b[i].dy);
    fclose(f);

    snprintf(path, sizeof path, "%s/cull_result_%d.txt", dir, n);
    f = fopen(path, "w");
    fprintf(f, "naive %d", naive.count);
    for (int i = 0; i < naive.count; ++i) fprintf(f, " %d", naive.ids[i]);
    fprintf(f, "\n");
    for (int k = 0; k < 3; ++k) {
        fprintf(f, "ratio %.2f %d", RATIOS[k], res[k].count);
        for (int i = 0; i < res[k].count; ++i) fprintf(f, " %d", res[k].ids[i]);
        fprintf(f, "\n");
    }
    fclose(f);
    printf("\nwritten %s, %s\n", path, dir);

    for (int k = 0; k < 3; ++k) { free(res[k].ids); cull_report_free(&rep[k]); }
    cull_report_free(&trep);
    free(truth.ids); free(naive.ids);
    free(b); free(plain);
}

int main(int argc, char **argv) {
    const char *dir = (argc > 1) ? argv[1] : "../artifacts";
    run_n(1000, dir);
    run_n(10000, dir);
    return 0;
}
