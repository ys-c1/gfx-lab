/* bench.c -- two-stage adaptive culling benchmark and report generator.
 *
 * Stages (see src/cull.h):
 *   1. cull_apply_tolerance: tol_i = tol_base + hypot(vx,vy) * dt
 *   2. cull_run_adaptive:    each box grown by tol_i, viewport grown by margin
 *
 * Outputs (in OUTDIR, default artifacts/cull):
 *   hand.csv                      hand-computed 8-object reference cases
 *   details_<N>_m<margin>.csv     per-object truth and decisions
 *   candidates_<N>_legacy.txt     legacy candidate ids (one per line)
 *   candidates_<N>_m<margin>.txt  adaptive candidate ids (one per line)
 *   summary.json                  aggregate metrics and timings
 *   report.md                     Chinese benchmark report
 */
#define _POSIX_C_SOURCE 200809L
#include "cull.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <time.h>

#ifndef PI
#define PI 3.14159265358979323846
#endif

#define DT (1.0 / 60.0)
#define DT_HAND 0.1 /* hand cases are designed for dt = 0.1 s */
#define VIEW_W 1920.0
#define VIEW_H 1080.0
#define SEED 0xC0FFEEULL
#define SPEED_MAX 1200.0

static uint64_t rng_state = SEED;
static double rnd(void) {
    rng_state = rng_state * 6364136223846793005ULL + 1442695040888963407ULL;
    return (double)((rng_state >> 33) & 0x7FFFFFFFu) / (double)0x7FFFFFFFu;
}

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static double median(double *s, int k) {
    qsort(s, (size_t)k, sizeof(double), cmp_double);
    return s[k / 2];
}

static void gen_boxes(CullBox *a, int n) {
    for (int i = 0; i < n; ++i) {
        a[i].x = -200.0 + rnd() * 2320.0;           /* [-200, 2120) */
        a[i].y = -200.0 + rnd() * 1480.0;           /* [-200, 1280) */
        a[i].w = 4.0 + rnd() * 60.0;                /* [4, 64) */
        a[i].h = 4.0 + rnd() * 60.0;
        a[i].layer = i % 4;
        double speed = (rnd() < 0.1) ? 0.0 : rnd() * SPEED_MAX;
        double ang = rnd() * 2.0 * PI;
        a[i].vx = speed * cos(ang);
        a[i].vy = speed * sin(ang);
        a[i].tol = 0.0;
    }
}

static int overlap_rect(double x0, double y0, double x1, double y1,
                        CullView v) {
    return !(x1 < v.x || x0 > v.x + v.w || y1 < v.y || y0 > v.y + v.h);
}

static int static_inside(const CullBox *b, CullView v) {
    return overlap_rect(b->x, b->y, b->x + b->w, b->y + b->h, v);
}

/* Exact swept AABB: union of the box at t and t+dt. This is the
 * geometric ground truth for "visible at some point during the frame". */
static int swept_inside(const CullBox *b, double dt, CullView v) {
    double x0 = b->x, x1 = b->x + b->w;
    double y0 = b->y, y1 = b->y + b->h;
    double nx0 = x0 + b->vx * dt, nx1 = x1 + b->vx * dt;
    double ny0 = y0 + b->vy * dt, ny1 = y1 + b->vy * dt;
    return overlap_rect(fmin(x0, nx0), fmin(y0, ny0),
                        fmax(x1, nx1), fmax(y1, ny1), v);
}

static CullBox mkbox(double x, double y, double w, double h,
                     double vx, double vy, double tol) {
    CullBox b = {x, y, w, h, 0, vx, vy, tol};
    return b;
}

static void mkdir_p(const char *dir) {
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s", dir);
    size_t len = strlen(tmp);
    for (size_t i = 1; i < len; ++i)
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            mkdir(tmp, 0755);
            tmp[i] = '/';
        }
    mkdir(dir, 0755);
}

typedef struct {
    int n;
    double margin;
    int legacy_count, adapt_count;
    int static_count, swept_count;
    int legacy_false, adapt_false, extra_conservative;
    double tol_min, tol_max, tol_sum;
    double t_legacy, t_stage1, t_stage2, t_total;
} Metrics;

static int contains(const CullResult *r, int id) {
    for (int i = 0; i < r->count; ++i)
        if (r->ids[i] == id) return 1;
    return 0;
}

int main(int argc, char **argv) {
    const char *outdir = (argc > 1) ? argv[1] : "artifacts/cull";
    mkdir_p(outdir);

    CullView view = {0.0, 0.0, VIEW_W, VIEW_H};
    const int Ns[2] = {1000, 10000};
    const double margins[2] = {0.0, 8.0};

    char path[512];
    FILE *fj, *fr;

    /* ---- hand-computed reference, same 8 boxes as test.c ----------- */
    CullView hand_view = {0.0, 0.0, 60.0, 60.0};
    enum { HN = 8 };
    CullBox hb[HN] = {
        mkbox(  0,  0, 10, 10,    0,    0, 0),
        mkbox(100,100, 10, 10,    0,    0, 0),
        mkbox( 50, 50, 20, 20,    0,    0, 0),
        mkbox( -5, -5, 10, 10,    0,    0, 0),
        mkbox( 65, 10, 10, 10, -100,    0, 0),
        mkbox( 65, 30, 10, 10,  -10,    0, 0),
        mkbox( 10, 65, 10, 10,    0, -100, 0),
        mkbox( 65, 65, 10, 10,  -30,  -40, 0),
    };
    double htols[HN];
    CullParams hp = {DT_HAND, 0.0};
    if (cull_apply_tolerance(hb, HN, hp, htols, 0) != 0) return 2;
    CullResult hlegacy, hadapt0, hadapt8;
    if (cull_run(hb, HN, hand_view, &hlegacy) != 0) return 2;
    if (cull_run_adaptive(hb, HN, hand_view, hp, htols, &hadapt0, 0) != 0) return 2;
    CullParams hp8 = {DT_HAND, 8.0};
    if (cull_run_adaptive(hb, HN, hand_view, hp8, htols, &hadapt8, 0) != 0) return 2;
    /* guard the hand-computed expectations */
    int expect0[] = {0, 2, 3};
    int expectA[] = {0, 2, 3, 4, 6, 7};
    if (hlegacy.count != 3) return 3;
    for (int i = 0; i < 3; ++i) if (hlegacy.ids[i] != expect0[i]) return 3;
    if (hadapt0.count != 6) return 3;
    for (int i = 0; i < 6; ++i) if (hadapt0.ids[i] != expectA[i]) return 3;

    snprintf(path, sizeof path, "%s/hand.csv", outdir);
    FILE *fh = fopen(path, "w");
    if (!fh) return 4;
    fprintf(fh, "id,x,y,w,h,vx,vy,speed,tol,static_inside,swept_inside,"
                "legacy_kept,adapt_m0_kept,adapt_m8_kept\n");
    for (int i = 0; i < HN; ++i) {
        double speed = hypot(hb[i].vx, hb[i].vy);
        fprintf(fh, "%d,%g,%g,%g,%g,%g,%g,%g,%g,%d,%d,%d,%d,%d\n",
                i, hb[i].x, hb[i].y, hb[i].w, hb[i].h, hb[i].vx, hb[i].vy,
                speed, htols[i], static_inside(&hb[i], hand_view),
                swept_inside(&hb[i], DT_HAND, hand_view), contains(&hlegacy, i),
                contains(&hadapt0, i), contains(&hadapt8, i));
    }
    fclose(fh);
    free(hlegacy.ids);
    free(hadapt0.ids);
    free(hadapt8.ids);

    /* ---- main benchmark --------------------------------------------- */
    Metrics M[2][2];
    snprintf(path, sizeof path, "%s/summary.json", outdir);
    fj = fopen(path, "w");
    if (!fj) return 4;
    fprintf(fj, "{\n  \"dt\": %.10g, \"view\": {\"w\": %g, \"h\": %g}, "
                "\"seed\": %llu, \"speed_max\": %g, \"runs\": [\n",
            DT, VIEW_W, VIEW_H, (unsigned long long)SEED, SPEED_MAX);

    int first_run = 1;
    for (int ni = 0; ni < 2; ++ni) {
        int n = Ns[ni];
        int reps = (n <= 1000) ? 201 : 51;
        CullBox *boxes = (CullBox *)malloc(sizeof(CullBox) * (size_t)n);
        double *tols = (double *)malloc(sizeof(double) * (size_t)n);
        gen_boxes(boxes, n);

        CullResult legacy;
        double *samples = (double *)malloc(sizeof(double) * (size_t)reps);

        /* legacy timing + result */
        for (int k = 0; k < reps; ++k) {
            double t0 = now_sec();
            if (cull_run(boxes, n, view, &legacy) != 0) return 2;
            samples[k] = now_sec() - t0;
            if (k < reps - 1) free(legacy.ids);
        }
        double t_legacy = median(samples, reps);

        unsigned char *st = (unsigned char *)malloc((size_t)n);
        unsigned char *sw = (unsigned char *)malloc((size_t)n);
        int static_count = 0, swept_count = 0;
        for (int i = 0; i < n; ++i) {
            st[i] = (unsigned char)static_inside(&boxes[i], view);
            sw[i] = (unsigned char)swept_inside(&boxes[i], DT, view);
            static_count += st[i];
            swept_count += sw[i];
        }
        snprintf(path, sizeof path, "%s/candidates_%d_legacy.txt", outdir, n);
        if (cull_export(path, &legacy) != 0) return 4;

        for (int mi = 0; mi < 2; ++mi) {
            double margin = margins[mi];
            CullParams params = {DT, margin};

            for (int k = 0; k < reps; ++k) {
                double t0 = now_sec();
                if (cull_apply_tolerance(boxes, n, params, tols, 0) != 0)
                    return 2;
                samples[k] = now_sec() - t0;
            }
            double t_s1 = median(samples, reps);

            CullResult adapt;
            for (int k = 0; k < reps; ++k) {
                double t0 = now_sec();
                if (cull_run_adaptive(boxes, n, view, params, tols,
                                      &adapt, 0) != 0)
                    return 2;
                samples[k] = now_sec() - t0;
                if (k < reps - 1) free(adapt.ids);
            }
            double t_s2 = median(samples, reps);

            for (int k = 0; k < reps; ++k) {
                double t0 = now_sec();
                if (cull_apply_tolerance(boxes, n, params, tols, 0) != 0 ||
                    cull_run_adaptive(boxes, n, view, params, tols,
                                      &adapt, 0) != 0)
                    return 2;
                samples[k] = now_sec() - t0;
                if (k < reps - 1) free(adapt.ids);
            }
            double t_total = median(samples, reps);

            int legacy_false = 0, adapt_false = 0;
            double tol_min = INFINITY, tol_max = 0.0, tol_sum = 0.0;
            for (int i = 0; i < n; ++i) {
                if (tols[i] < tol_min) tol_min = tols[i];
                if (tols[i] > tol_max) tol_max = tols[i];
                tol_sum += tols[i];
                if (sw[i] && !st[i] && !contains(&legacy, i))
                    ++legacy_false;
                if (sw[i] && !contains(&adapt, i))
                    ++adapt_false;
            }
            int extra = adapt.count - (swept_count - adapt_false);

            M[ni][mi] = (Metrics){n, margin, legacy.count, adapt.count,
                                  static_count, swept_count,
                                  legacy_false, adapt_false, extra,
                                  tol_min, tol_max, tol_sum,
                                  t_legacy, t_s1, t_s2, t_total};

            snprintf(path, sizeof path, "%s/details_%d_m%g.csv",
                     outdir, n, margin);
            FILE *fc = fopen(path, "w");
            if (!fc) return 4;
            fprintf(fc, "id,x,y,w,h,vx,vy,speed,tol,margin,static_inside,"
                        "swept_inside,legacy_kept,adapt_kept,"
                        "legacy_false_cull,adapt_false_cull\n");
            for (int i = 0; i < n; ++i) {
                double speed = hypot(boxes[i].vx, boxes[i].vy);
                int lk = contains(&legacy, i), ak = contains(&adapt, i);
                fprintf(fc, "%d,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.12f,%g,"
                            "%d,%d,%d,%d,%d,%d\n",
                        i, boxes[i].x, boxes[i].y, boxes[i].w, boxes[i].h,
                        boxes[i].vx, boxes[i].vy, speed, tols[i], margin,
                        st[i], sw[i], lk, ak,
                        (sw[i] && !lk) ? 1 : 0, (sw[i] && !ak) ? 1 : 0);
            }
            fclose(fc);

            snprintf(path, sizeof path, "%s/candidates_%d_m%g.txt",
                     outdir, n, margin);
            if (cull_export(path, &adapt) != 0) return 4;
            free(adapt.ids);

            if (!first_run) fprintf(fj, ",\n");
            first_run = 0;
            fprintf(fj,
                "    {\"n\": %d, \"margin\": %g, \"legacy_candidates\": %d, "
                "\"adaptive_candidates\": %d, \"static_inside\": %d, "
                "\"swept_inside\": %d, \"legacy_false_culls\": %d, "
                "\"adaptive_false_culls\": %d, \"conservative_extra\": %d, "
                "\"tol_min\": %.6f, \"tol_avg\": %.6f, \"tol_max\": %.6f, "
                "\"t_legacy_s\": %.9f, \"t_stage1_s\": %.9f, "
                "\"t_stage2_s\": %.9f, \"t_total_s\": %.9f}",
                n, margin, legacy.count, M[ni][mi].adapt_count,
                static_count, swept_count, legacy_false, adapt_false, extra,
                tol_min, tol_sum / n, tol_max,
                t_legacy, t_s1, t_s2, t_total);
        }
        free(legacy.ids);
        free(st);
        free(sw);
        free(samples);
        free(tols);
        free(boxes);
    }
    fprintf(fj, "\n  ]\n}\n");
    fclose(fj);

    /* ---- report ------------------------------------------------------ */
    snprintf(path, sizeof path, "%s/report.md", outdir);
    fr = fopen(path, "w");
    if (!fr) return 4;
    struct utsname un;
    uname(&un);
    char stamp[64];
    time_t now = time(0);
    strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", localtime(&now));

    fprintf(fr, "# 视口剔除：自适应容差两段式改造报告\n\n");
    fprintf(fr, "- 生成时间：%s\n", stamp);
    fprintf(fr, "- 运行环境：%s %s（%s）\n", un.sysname, un.release, un.machine);
    fprintf(fr, "- 数据：确定性 LCG 随机（seed=%llu），位置 x∈[-200,2120)、"
                "y∈[-200,1280)，盒宽高 [4,64) px，10%% 对象静止，"
                "其余速度大小 [0,%g] px/s、方向均匀\n",
            (unsigned long long)SEED, SPEED_MAX);
    fprintf(fr, "- 视口：%g×%g，帧间隔 dt=1/60≈%.6f s\n", VIEW_W, VIEW_H, DT);
    fprintf(fr, "- 计时：CLOCK_MONOTONIC，1k 重复 %d 次、1w 重复 %d 次，取中位数\n\n",
            201, 51);

    fprintf(fr, "## 1. 方法（两段式）\n\n");
    fprintf(fr, "**阶段一：按对象速度算自适应容差**\n\n");
    fprintf(fr, "```\n"
                "speed_i = hypot(vx_i, vy_i)          # 像素/秒\n"
                "tol_i   = tol_base_i + speed_i * dt  # 像素，逐对象\n"
                "```\n\n");
    fprintf(fr, "**阶段二：按外扩后的视口筛选**。对象盒四边各外扩 `tol_i` 像素，"
                "视口四边各外扩 `margin` 像素，再做 AABB 相交测试；"
                "对象实际外扩量为 `tol_i + margin`。当全部 `tol_i=0` 且 `margin=0` "
                "时退化为原 `cull_run`，集合逐元素一致（单元测试含 200 随机盒对照）。\n\n");
    fprintf(fr, "**输入校验**：`tol<0`、`dt<0`、`margin<0` 或速度分量非有限 "
                "(NaN/±Inf) 返回 -3，`CullError.field` 指名字段 "
                "（`tol`/`vx/vy`/`dt`/`margin`/`tolerance`），`index` 指出对象下标"
                "（全局参数为 -1）。\n\n");

    fprintf(fr, "## 2. 逐对象手算对照（8 例，视口 [0,60]²，dt=0.1）\n\n");
    fprintf(fr, "容差手算：tol = hypot(vx,vy)·0.1；自适应保留条件为外扩盒与 "
                "[0,60]² 相交。m8 列指视口四边再外扩 8 px。\n\n");
    fprintf(fr, "| id | 盒(x,y,w,h) | (vx,vy) | speed | tol 手算 | 静态在内 | "
                "保留 m0 | 保留 m8 | 说明 |\n");
    fprintf(fr, "|----|------------|---------|-------|----------|----------|"
                "---------|---------|------|\n");
    const char *notes[HN] = {
        "静态在内，两法都保留",
        "距离 40px，远超 tol=0，剔除",
        "静态相交（右边/上边），保留",
        "静态相交（角），保留",
        "speed=100 → tol=10；外扩后左边 65-10=55≤60，保留；下一帧 x=55 确实入屏",
        "speed=10 → tol=1，m0 下 64>60 仍有 4px 间隙被剔；m8 视口边扩到 68，保留",
        "speed=100 → tol=10；外扩后上边 65-10=55≤60，保留；下一帧 y=55 入屏",
        "speed=hypot(30,40)=50 → tol=5；x、y 方向各外扩 5，65-5=60 恰好相交，保留",
    };
    double ht2[HN];
    cull_apply_tolerance(hb, HN, hp, ht2, 0);
    CullResult hl2, ha2, ha8;
    cull_run(hb, HN, hand_view, &hl2);
    cull_run_adaptive(hb, HN, hand_view, hp, ht2, &ha2, 0);
    cull_run_adaptive(hb, HN, hand_view, hp8, ht2, &ha8, 0);
    for (int i = 0; i < HN; ++i) {
        fprintf(fr, "| %d | (%g,%g,%g,%g) | (%g,%g) | %g | %.4f | %s | %s | %s | %s |\n",
                i, hb[i].x, hb[i].y, hb[i].w, hb[i].h, hb[i].vx, hb[i].vy,
                hypot(hb[i].vx, hb[i].vy), ht2[i],
                static_inside(&hb[i], hand_view) ? "是" : "否",
                contains(&ha2, i) ? "是" : "否",
                contains(&ha8, i) ? "是" : "否", notes[i]);
    }
    fprintf(fr, "\n手算结论：旧法候选 {0,2,3}，误剔 {4,6}（2 个）；"
                "自适应 m0 候选 {0,2,3,4,6,7}，误剔 0；m8 再纳入 {5}（共 7 个）。"
                "id=7 为速度方向无关的保守保留（容差按速度大小均匀外扩，"
                "其精确扫掠 AABB 并未入屏），代价是多 1 个候选，换取不漏剔。"
                "完整逐行数据见 `hand.csv`。\n\n");
    free(hl2.ids);
    free(ha2.ids);
    free(ha8.ids);

    fprintf(fr, "## 3. 1k / 1w 量化结果\n\n");
    fprintf(fr, "Ground truth：精确扫掠 AABB（当前帧与下一帧盒的并集）与视口相交即"
                "「本帧应保留」。误剔 = 扫掠入屏但被该方法剔除的对象数。\n\n");
    fprintf(fr, "| 对象数 | 模式 | 对象外扩 tol min/avg/max (px) | 视口外扩 margin (px) | "
                "旧法候选 | 自适应候选 | 扫掠应保留 | 旧法误剔 | 自适应误剔 | 保守多留 |\n");
    fprintf(fr, "|-------:|------|------------------------------:|---------------------:|"
                "---------:|-----------:|-----------:|---------:|-----------:|---------:|\n");
    for (int ni = 0; ni < 2; ++ni)
        for (int mi = 0; mi < 2; ++mi) {
            Metrics *m = &M[ni][mi];
            fprintf(fr, "| %d | margin=%g | %.3f / %.3f / %.3f | %.0f | %d | %d | %d | %d | %d | %d |\n",
                    m->n, m->margin, m->tol_min, m->tol_sum / m->n, m->tol_max,
                    m->margin, m->legacy_count, m->adapt_count, m->swept_count,
                    m->legacy_false, m->adapt_false, m->extra_conservative);
        }

    fprintf(fr, "\n## 4. 耗时（中位数，秒；每对象纳秒）\n\n");
    fprintf(fr, "| 对象数 | 模式 | 旧法总耗时 | 阶段一耗时 | 阶段二耗时 | 两段合计 | 合计 ns/对象 |\n");
    fprintf(fr, "|-------:|------|-----------:|-----------:|-----------:|---------:|-------------:|\n");
    for (int ni = 0; ni < 2; ++ni)
        for (int mi = 0; mi < 2; ++mi) {
            Metrics *m = &M[ni][mi];
            fprintf(fr, "| %d | margin=%g | %.9f | %.9f | %.9f | %.9f | %.2f |\n",
                    m->n, m->margin, m->t_legacy, m->t_stage1, m->t_stage2,
                    m->t_total, m->t_total * 1e9 / m->n);
        }
    fprintf(fr, "\n注：阶段一为 `hypot`+乘加的纯算术遍历；阶段二含一次结果数组分配，"
                "与旧法口径一致。重复次数：1k×%d、1w×%d。\n\n", 201, 51);

    fprintf(fr, "## 5. 导出文件（目录 `%s`）\n\n", outdir);
    fprintf(fr, "- `hand.csv`：8 个手算对象的完整逐行数据\n");
    fprintf(fr, "- `details_<N>_m<margin>.csv`：每个对象的位置/速度/tol/外扩/"
                "静态/扫掠/两种方法判定与误剔标记，供独立复算\n");
    fprintf(fr, "- `candidates_<N>_legacy.txt`：旧法候选 ID（每行一个）\n");
    fprintf(fr, "- `candidates_<N>_m<margin>.txt`：自适应候选 ID（由 "
                "`cull_export` 导出，每行一个）\n");
    fprintf(fr, "- `summary.json`：聚合指标与计时\n\n");

    fprintf(fr, "## 6. 独立 Python 复算\n\n");
    fprintf(fr, "见 `crosscheck.log`（由 `tools/crosscheck.py -I` 独立重算生成）。\n");
    fclose(fr);

    printf("bench done. artifacts in %s\n", outdir);
    return 0;
}
