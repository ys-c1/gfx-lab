/* bench.c —— 面光软阴影两档（高速 FAST / 低速 SLOW）对照穷举参考 EXHAUST：
 *   记录耗时、覆盖比例误差（RMSE/最大误差）、半影宽度，并导出各朝向的采点表
 *   与采点分布（局部+世界坐标）。 */
#define _POSIX_C_SOURCE 199309L
#include "shadow.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define W 128
#define H 128

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static double mean_cov(const float *cov, int n) {
    double s = 0;
    for (int i = 0; i < n; ++i) s += cov[i];
    return s / n;
}

static void rmse_max(const float *a, const float *b, int n, double *rmse, double *maxe) {
    double se = 0; *maxe = 0;
    for (int i = 0; i < n; ++i) {
        double e = fabs((double)a[i] - b[i]);
        se += (double)(a[i] - b[i]) * (a[i] - b[i]);
        if (e > *maxe) *maxe = e;
    }
    *rmse = sqrt(se / n);
}

/* 导出某档位在四个朝向下的采点表：每个采样点一行，含局部坐标与四个朝向的
 * 世界坐标（各朝向采点表并排给出，便于逐点核对旋转）。 */
static void dump_sample_tables(const ShLight *base, const ShRenderCfg *cfg, FILE *f) {
    enum { NA = 4 };
    int n = cfg->cols * cfg->rows;
    const float angles[NA] = {0.0f, (float)M_PI/4, (float)M_PI/2, (float)M_PI};
    const char *names[NA] = {"0", "pi/4", "pi/2", "pi"};
    ShPoint *world[NA];
    for (int a = 0; a < NA; ++a) {
        world[a] = (ShPoint*)malloc((size_t)n * sizeof(ShPoint));
        ShLight l = *base; l.angle = angles[a];
        if (sh_sample_quad(&l, cfg, world[a]) != n) {
            fprintf(f, "sample error: %s\n", sh_last_error());
            for (int k = 0; k <= a; ++k) free(world[k]);
            return;
        }
    }
    fprintf(f, "# sample table gear=%d grid=%dx%d points=%d half-width=%.2f half-height=%.2f\n",
            cfg->gear, cfg->cols, cfg->rows, n, base->aw, base->ah);
    fprintf(f, "# idx\tlocal_x\tlocal_y");
    for (int a = 0; a < NA; ++a) fprintf(f, "\tw%s_x\tw%s_y", names[a], names[a]);
    fprintf(f, "\n");
    for (int i = 0; i < n; ++i) {
        float lx, ly;
        sh_local_point(cfg, i, &lx, &ly);
        fprintf(f, "%d\t%.5f\t%.5f", i, lx * base->aw, ly * base->ah);
        for (int a = 0; a < NA; ++a)
            fprintf(f, "\t%.5f\t%.5f", world[a][i].x, world[a][i].y);
        fprintf(f, "\n");
    }
    for (int a = 0; a < NA; ++a) free(world[a]);
}

int main(void) {
    ShadowMap m, mref;
    if (sh_init(&m, W, H) != 0 || sh_init(&mref, W, H) != 0) return 1;
    float *cov  = (float*)malloc((size_t)W*H*sizeof(float));
    float *cref = (float*)malloc((size_t)W*H*sizeof(float));
    if (!cov || !cref) return 1;

    /* 矩形面光在上方（aw!=ah，朝向可观测），中间一道横墙；墙后应有本影+半影。 */
    ShLight al = {64.0f, 16.0f, 60.0f, 1.0f, 6.0f, 3.0f, 0.0f};
    ShSeg wall = {20.0f, 70.0f, 108.0f, 70.0f};

    const ShRenderCfg *gears[3] = {
        sh_preset_cfg(SH_GEAR_FAST), sh_preset_cfg(SH_GEAR_SLOW), sh_preset_cfg(SH_GEAR_EXHAUST)
    };
    const char *gnames[3] = {"FAST(high-speed)", "SLOW(low-speed)", "EXHAUST(reference)"};
    ShPenumbra pens[3];

    /* 穷举参考先跑。 */
    double t0 = now_sec();
    if (sh_render_ex(&mref, cref, &pens[2], &al, gears[2], &wall, 1) != 0) {
        printf("exhaust failed: %s\n", sh_last_error()); return 1;
    }
    double tref = now_sec() - t0;

    printf("== gfx-lab shadow area-light bench (%dx%d, 1 occluder) ==\n", W, H);
    printf("light: center=(%.1f,%.1f) half-width=%.2f half-height=%.2f radius=%.1f\n",
           al.x, al.y, al.aw, al.ah, al.radius);
    printf("\n%-18s %6s %10s %10s %10s %10s %10s %10s\n",
           "gear", "points", "time_ms", "mean_cov", "rmse", "max_err", "pen_avg", "pen_max");
    printf("%-18s %6d %10.3f %10.4f %10s %10s %10.3f %10.3f\n",
           gnames[2], gears[2]->cols*gears[2]->rows, tref*1e3, mean_cov(cref, W*H),
           "-", "-", pens[2].width_avg, pens[2].width_max);

    for (int g = 0; g < 2; ++g) {
        t0 = now_sec();
        if (sh_render_ex(&m, cov, &pens[g], &al, gears[g], &wall, 1) != 0) {
            printf("%s failed: %s\n", gnames[g], sh_last_error()); return 1;
        }
        double dt = now_sec() - t0;
        double rmse, maxe;
        rmse_max(cov, cref, W*H, &rmse, &maxe);
        printf("%-18s %6d %10.3f %10.4f %10.5f %10.5f %10.3f %10.3f\n",
               gnames[g], gears[g]->cols*gears[g]->rows, dt*1e3, mean_cov(cov, W*H),
               rmse, maxe, pens[g].width_avg, pens[g].width_max);
    }

    /* 与穷举核对结论。 */
    double rmse_f, max_f, rmse_s, max_s;
    /* 再跑一次取误差（上面已输出，这里重算用于结论行）。 */
    sh_render_ex(&m, cov, NULL, &al, gears[0], &wall, 1);
    rmse_max(cov, cref, W*H, &rmse_f, &max_f);
    sh_render_ex(&m, cov, NULL, &al, gears[1], &wall, 1);
    rmse_max(cov, cref, W*H, &rmse_s, &max_s);
    printf("\ncheck vs exhaustive:\n");
    printf("  FAST  rmse=%.5f max=%.5f (16 samples)\n", rmse_f, max_f);
    printf("  SLOW  rmse=%.5f max=%.5f (144 samples)\n", rmse_s, max_s);
    printf("  SLOW is %dx the cost of FAST and %s accurate (rmse ratio %.2f)\n",
           (gears[1]->cols*gears[1]->rows)/(gears[0]->cols*gears[0]->rows),
           rmse_s < rmse_f ? "more" : "NOT more", rmse_f / (rmse_s > 1e-9 ? rmse_s : 1e-9));

    /* 各朝向采点表导出（两档各一份）。 */
    FILE *tf = fopen("build/sample_table.tsv", "w");
    if (tf) {
        dump_sample_tables(&al, gears[0], tf);
        fprintf(tf, "\n");
        dump_sample_tables(&al, gears[1], tf);
        fclose(tf);
        printf("\nsample tables (4 orientations, local+world) -> build/sample_table.tsv\n");
    }
    if (sh_write_pgm(&mref, "build/bench_exhaust.pgm") == 0 &&
        sh_write_pgm(&m, "build/bench_slow.pgm") == 0)
        printf("coverage PGMs -> build/bench_exhaust.pgm, build/bench_slow.pgm\n");

    free(cov); free(cref);
    sh_free(&m); sh_free(&mref);
    return 0;
}
