#define _POSIX_C_SOURCE 199309L
#include "cull.h"
#include "cull_inc.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* 场景：side*side 的规则网格对象，铺满世界；随机抖动让边界更真实 */
static CullBox *make_world(int side, double spacing, double size, unsigned seed) {
    int n = side * side;
    CullBox *b = malloc(sizeof(CullBox) * (size_t)n);
    srand(seed);
    for (int i = 0; i < n; ++i) {
        double jx = (rand() % 100) / 100.0 * 2.0 - 1.0;
        double jy = (rand() % 100) / 100.0 * 2.0 - 1.0;
        b[i].x = (i % side) * spacing + jx;
        b[i].y = (i / side) * spacing + jy;
        b[i].w = size; b[i].h = size;
        b[i].layer = i % 4;
    }
    return b;
}

/* 生成一条以“轻微拖动”为主的视口序列：每帧只平移 step，偶发一次较大跳动 */
static CullView *make_path(int frames, double step, double vw, double vh,
                           double world) {
    CullView *p = malloc(sizeof(CullView) * (size_t)frames);
    double x = 0, y = 0;
    int dir = 0;
    for (int f = 0; f < frames; ++f) {
        if (f % 25 == 24) { x += step * 8; y += step * 4; }   /* 偶发较大跳动 */
        else {
            switch (dir++ % 4) {
                case 0: x += step; break;
                case 1: y += step; break;
                case 2: x -= step; break;
                default: y -= step; break;
            }
        }
        if (x < 0) x = 0;
        if (x + vw > world) x = world - vw;
        if (y < 0) y = 0;
        if (y + vh > world) y = world - vh;
        p[f] = (CullView){x, y, vw, vh};
    }
    return p;
}

static int eq_sets(const int *a, int na, const int *bb, int nb) {
    if (na != nb) return 0;
    for (int i = 0; i < na; ++i) if (a[i] != bb[i]) return 0;
    return 1;
}

static void run_scale(int side, int frames, double step) {
    int n = side * side;
    double spacing = 10.0, size = 8.0;
    double world = side * spacing;
    double vw = 200.0, vh = 160.0;
    CullBox *boxes = make_world(side, spacing, size, 999);
    CullView *path = make_path(frames, step, vw, vh, world);

    printf("================ N=%d (grid %dx%d), frames=%d, step=%.1f ================\n",
           n, side, side, frames, step);

    /* ---------- 全量重算（对照）：每帧 cull_run 扫全部 N 个 ---------- */
    double t0 = now_sec();
    long full_work = 0; int full_last = 0;
    CullResult *full = malloc(sizeof(CullResult) * (size_t)frames);
    for (int f = 0; f < frames; ++f) {
        cull_run(boxes, n, path[f], &full[f]);
        full_work += n;                 /* 全量每帧检查 N 个候选 */
        full_last = full[f].count;
    }
    double t_full = now_sec() - t0;

    /* ---------- 增量：两段式，只处理进出边界的候选 ---------- */
    CullInc *st = NULL;
    double ti0 = now_sec();
    int rc = cull_inc_open(boxes, n, path[0], &st, NULL);
    double t_open = now_sec() - ti0;
    if (rc != 0) { printf("open failed %d\n", rc); return; }

    /* 首帧（open 产出的初始集合）立即与全量对齐 */
    int errors = 0;
    {
        CullResult snap0;
        cull_inc_snapshot(st, &snap0);
        if (!eq_sets(snap0.ids, snap0.count, full[0].ids, full[0].count)) ++errors;
        free(snap0.ids);
    }

    long sum_cand = 0, sum_upd = 0, sum_in = 0, sum_out = 0, max_upd = 0;
    double t1 = now_sec();
    for (int f = 1; f < frames; ++f) {
        CullIncStats s;
        if (cull_inc_update(st, path[f], &s, NULL) != 0) { printf("update fail\n"); break; }
        sum_cand += s.candidates; sum_upd += s.updated;
        sum_in += s.entered; sum_out += s.exited;
        if (s.updated > max_upd) max_upd = s.updated;
        CullResult snap;
        cull_inc_snapshot(st, &snap);
        if (!eq_sets(snap.ids, snap.count, full[f].ids, full[f].count)) ++errors;
        free(snap.ids);
    }
    double t_inc = now_sec() - t1;

    long avg_cand = sum_cand / (frames - 1);
    long avg_upd  = sum_upd  / (frames - 1);
    double speedup = t_full / t_inc;

    printf("view size %.0fx%.0f  (visible ~%d/frame)\n", vw, vh, full_last);
    printf("[全量重算] 每帧候选=N=%d，累计候选=%ld，总耗时=%.3f ms，均摊=%.3f us/帧\n",
           n, full_work, t_full * 1e3, t_full * 1e6 / frames);
    printf("[增量更新] 建索引耗时=%.3f ms；累计候选=%ld 累计更新=%ld (进=%ld 出=%ld)\n",
           t_open * 1e3, sum_cand, sum_upd, sum_in, sum_out);
    printf("           均摊候选=%ld/帧 均摊更新=%ld/帧 峰值更新=%ld/帧，总耗时=%.3f ms，均摊=%.3f us/帧\n",
           avg_cand, avg_upd, max_upd, t_inc * 1e3, t_inc * 1e6 / (frames - 1));
    printf("候选缩减: 全量 %d -> 增量 %ld/帧 (%.1fx 更少检查)；耗时加速比=%.2fx\n",
           n, avg_cand, (double)n / (avg_cand ? avg_cand : 1), speedup);
    printf("逐项相等校验: 误差 %d 处（%s）\n", errors, errors == 0 ? "增量结果与全量完全一致" : "不一致!");

    for (int f = 0; f < frames; ++f) free(full[f].ids);
    free(full); free(path); free(boxes);
    cull_inc_close(st);
}

int main(void) {
    enum { FRAMES = 200 };
    /* 视口每帧只移动 3 个世界单位（对象间距 10），即“只动一点点” */
    run_scale(100, FRAMES, 3.0);     /* 1 万 */
    printf("\n");
    run_scale(316, FRAMES, 3.0);     /* 316^2 = 99856 ≈ 10 万 */
    printf("\n");
    /* 对照：同样 10 万对象，放大每帧步长，观察更新数随差异带宽度增长 */
    run_scale(316, FRAMES, 12.0);
    return 0;
}
