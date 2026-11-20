#define _POSIX_C_SOURCE 199309L
#include "shadow.h"
#include "softshadow.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* 解析解：理想阶跃边经标准差 sigma 的高斯模糊后，10%~90% 过渡带宽
 *   W = 2 * z(0.9) * sigma = 2 * 1.28155 * sigma （CDF 反函数） */
#define Z90 1.2815515655446004f

static float analytic_width(float sigma) { return 2.0f * Z90 * sigma; }

/* 恒定半径的合成阶跃边（上半 0 / 下半 1），用于干净地核对宽度解析解。
 * 返回该行数据：实测宽度、解析宽度、误差，供汇总表使用。 */
typedef struct { int r; float measured, analytic, err; } StepRow;
static int validate_step_edge(int size, int r, float sigma_scale, StepRow *row) {
    ShadowMap hard, soft;
    if (sh_init(&hard, size, size) || sh_init(&soft, size, size)) return -1;
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
            hard.buf[(size_t)y * size + x] = (y >= size / 2) ? 1.0f : 0.0f;
    float *rad = malloc((size_t)size * size * sizeof(float));
    float *kern = malloc(sizeof(float) * (2 * r + 1));
    for (int i = 0; i < size * size; ++i) rad[i] = (float)r;
    sb_kernel(r, sigma_scale, kern);
    SbConfig cfg = {.enable=1, .blur_radius_px=r, .blur_radius_min=r, .dist_begin=0.0f, .dist_scale=1.0f, .sigma_scale=sigma_scale, .border_mode=SB_BORDER_CLAMP};
    if (sb_blur_separable(&hard, &soft, rad, r, kern, &cfg) != SB_OK) return -1;
    SbSoftEdge se;
    if (sb_measure_soft_edge(&hard, &soft, size / 2, &se) != SB_OK) return -1;
    float expect = analytic_width(sigma_scale * (float)r);
    row->r = r; row->measured = se.width_px; row->analytic = expect;
    row->err = se.width_px - expect;
    free(rad); free(kern);
    sh_free(&hard); sh_free(&soft);
    return 0;
}

typedef struct {
    int   res;
    float width_px;        /* 实测 10%-90% 软边宽度 */
    float mean_radius;     /* 过渡带内平均模糊半径，证明半径随距离变化 */
    long  taps;
    double ms;
} Row;

/* 完整三段式管线跑一遍阴影场景，返回填表数据 */
static int run_scene(int res, Row *row) {
    ShadowMap hard, soft, final;
    if (sh_init(&hard, res, res) || sh_init(&soft, res, res) || sh_init(&final, res, res))
        return -1;
    ShLight light = {(float)res / 2.0f, (float)res * 0.125f, (float)res * 0.47f, 1.0f};
    ShSeg seg = {(float)res * 0.125f, (float)res * 0.47f,
                 (float)res * 0.875f, (float)res * 0.47f};
    if (sh_render(&hard, &light, &seg, 1) != 0) return -1;

    float *dist = malloc((size_t)res * res * sizeof(float));
    float *rad  = malloc((size_t)res * res * sizeof(float));
    /* dist_begin=0：紧贴遮挡物即开始软化；scale 取大，使边缘过渡带内半径迅速达到 rmax */
    SbConfig cfg = {.enable=1, .blur_radius_px=4, .blur_radius_min=2, .dist_begin=0.0f, .dist_scale=2.0f, .sigma_scale=0.5f, .border_mode=SB_BORDER_CLAMP};
    int rmax = 0;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    if (sb_distance_field(&hard, &seg, 1, &cfg, dist, rad, &rmax) != SB_OK) return -1;
    float *kern = malloc(sizeof(float) * (2 * rmax + 1));
    sb_kernel(rmax, cfg.sigma_scale, kern);
    if (sb_blur_separable(&hard, &soft, rad, rmax, kern, &cfg) != SB_OK) return -1;
    if (sb_composite(&hard, &soft, &cfg, &final) != SB_OK) return -1;
    clock_gettime(CLOCK_MONOTONIC, &t1);

    SbSoftEdge se;
    if (sb_measure_soft_edge(&hard, &final, res / 2, &se) != SB_OK) return -1;

    /* 过渡带 [lo_y, hi_y] 内该列的平均有效半径（半径确实随距离在增长）。
     * 变半径场景不对应单一解析宽度，故这里只记录实测宽度与平均半径，
     * 宽度误差的解析基准放到恒定半径阶跃边那张表。 */
    int ylo = (int)floorf(fminf(se.lo_y, se.hi_y));
    int yhi = (int)ceilf(fmaxf(se.lo_y, se.hi_y));
    if (ylo < 0) ylo = 0;
    if (yhi >= res) yhi = res - 1;
    double rsum = 0.0; int rcnt = 0;
    for (int y = ylo; y <= yhi; ++y) { rsum += rad[(size_t)y * res + res / 2]; ++rcnt; }

    row->res = res;
    row->width_px = se.width_px;
    row->mean_radius = (float)(rsum / (rcnt ? rcnt : 1));
    row->taps = cfg.sample_count;
    row->ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1.0e6;

    char path[128];
    snprintf(path, sizeof path, "soft_%d_hard.pgm", res);
    sb_write_pgm_f(path, res, res, hard.buf);
    snprintf(path, sizeof path, "soft_%d_final.pgm", res);
    sb_write_pgm_f(path, res, res, final.buf);
    snprintf(path, sizeof path, "kernel_%d.txt", res);
    sb_kernel_export(path, rmax, cfg.sigma_scale, cfg.border_mode);

    free(dist); free(rad); free(kern);
    sh_free(&hard); sh_free(&soft); sh_free(&final);
    return 0;
}

int main(void) {
    /* 表一：宽度误差的解析基准（恒定半径阶跃边，半径不随距离变化，解析解成立） */
    printf("== table 1: soft-edge width error vs analytic gaussian step edge ==\n");
    StepRow sr[2];
    if (validate_step_edge(128, 2, 0.5f, &sr[0])) return 1;
    if (validate_step_edge(128, 4, 0.5f, &sr[1])) return 1;
    printf("%-6s | %-9s | %-9s | %-24s\n", "r(px)", "measured", "analytic", "width err (10-90%)");
    printf("-------+-----------+-----------+--------------------------\n");
    for (int i = 0; i < 2; ++i)
        printf("%-6d | %9.3f | %9.3f | %+8.3f px (%6.2f%%)\n",
               sr[i].r, sr[i].measured, sr[i].analytic, sr[i].err,
               100.0f * sr[i].err / sr[i].analytic);

    /* 表二：完整三段式管线，两分辨率，宽度误差栏对应表一的同尺度基准 */
    printf("\n== table 2: full distance-driven soft-shadow pipeline ==\n");
    int resolutions[2] = {64, 256};
    Row rows[2];
    for (int i = 0; i < 2; ++i)
        if (run_scene(resolutions[i], &rows[i])) return 1;

    printf("%-6s | %-9s | %-11s | %-18s | %-9s | %-9s\n",
           "res", "width px", "mean radius", "width err*", "taps", "time(ms)");
    printf("-------+-----------+-------------+--------------------+-----------+----------\n");
    for (int i = 0; i < 2; ++i) {
        /* 用过渡带平均半径映射到表一同类基准，给一个量级参考（变半径，仅供对照） */
        float ref = analytic_width(0.5f * rows[i].mean_radius);
        float err = rows[i].width_px - ref;
        printf("%-6d | %9.3f | %11.2f | %+8.3f (%5.1f%%) | %-9ld | %9.3f\n",
               rows[i].res, rows[i].width_px, rows[i].mean_radius,
               err, 100.0f * err / ref, rows[i].taps, rows[i].ms);
    }
    printf("* width err uses the constant-radius analytic baseline at the band's mean radius;\n");
    printf("  variable-radius scenes have no single analytic width, see table 1 for true accuracy.\n");
    printf("mean effective radius grows with distance from occluder -> blur is distance-driven.\n");
    printf("outputs: soft_<res>_hard.pgm, soft_<res>_final.pgm, kernel_<res>.txt\n");
    printf("border: clamp (out-of-range taps reuse edge pixel; zero mode also tested in unit tests)\n");
    return 0;
}
