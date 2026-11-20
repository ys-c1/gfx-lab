#include "shadow.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FRAMES 48
#define FLIP_THRESH 0.02f /* 相邻帧亮度差超过此值记为跳变像素 */

static unsigned rng_state;
static float frand(void) {
    rng_state = rng_state * 1664525u + 1013904223u;
    return (float)((rng_state >> 8) & 0xFFFF) / 65535.0f;
}

/* 第 t 帧的“视角跳变”原始偏移（世界单位）：缓慢漂移 + 逐帧抖动 */
static void raw_offset(int t, float *ox, float *oy) {
    float jx = (frand() * 2.0f - 1.0f) * 1.5f;
    float jy = (frand() * 2.0f - 1.0f) * 1.5f;
    *ox = 0.05f * (float)t + jx;
    *oy = 0.03f * (float)t + jy;
}

typedef struct {
    double flips_avg;  /* 相邻帧跳变像素数（均值） */
    long   flips_max;  /* 相邻帧跳变像素数（峰值） */
    double off_err;    /* 偏移误差 |out - raw|（均值，像素） */
    double ms_frame;   /* 渲染耗时（毫秒/帧） */
} RunRes;

static int run(int res, int suppress, const char *csv, const char *pgm, RunRes *r) {
    ShadowMap cur, prv;
    if (sh_init(&cur, res, res) != 0 || sh_init(&prv, res, res) != 0) return 1;
    float fr = (float)res;
    ShLight light = {0.50f * fr, 0.15f * fr, 0.40f * fr, 1.0f};
    ShSeg segs[3] = {
        {0.15f * fr, 0.45f * fr, 0.45f * fr, 0.45f * fr},
        {0.55f * fr, 0.55f * fr, 0.85f * fr, 0.55f * fr},
        {0.30f * fr, 0.70f * fr, 0.30f * fr, 0.90f * fr},
    };
    ShStab st;
    sh_stab_init(&st);
    ShStabCfg cfg = {0.35f, 0.10f, suppress};
    float scale = fr / 128.0f; /* 同一世界跳变，像素偏移随分辨率缩放 */
    rng_state = 22222u;        /* 两种模式、两种分辨率喂同一串原始偏移 */

    long flips_total = 0, flips_max = 0;
    double err_sum = 0.0, clk = 0.0;
    for (int t = 0; t < FRAMES; ++t) {
        float rx, ry;
        raw_offset(t, &rx, &ry);
        rx *= scale; ry *= scale;
        if (t == 0) sh_stab_seed(&st, rx, ry);
        ShStabSample sm;
        float slope_x = 0.3f * scale, slope_y = 0.1f * scale; /* 恒定坡度演示斜率修正 */
        if (sh_stab_frame(&st, &cfg, rx, ry, slope_x, slope_y, &sm) != SH_STAB_OK) return 1;
        clock_t t0 = clock();
        if (sh_render_off(&cur, &light, segs, 3, sm.out_x, sm.out_y) != 0) return 1;
        clk += (double)(clock() - t0);
        if (t > 0) {
            long flips = 0;
            for (long i = 0; i < (long)res * res; ++i)
                if (fabsf(cur.buf[i] - prv.buf[i]) > FLIP_THRESH) ++flips;
            flips_total += flips;
            if (flips > flips_max) flips_max = flips;
        }
        float ex = sm.out_x - rx, ey = sm.out_y - ry;
        err_sum += sqrtf(ex * ex + ey * ey);
        memcpy(prv.buf, cur.buf, (size_t)res * res * sizeof(float));
    }
    r->flips_avg = (double)flips_total / (FRAMES - 1);
    r->flips_max = flips_max;
    r->off_err = err_sum / FRAMES;
    r->ms_frame = clk / CLOCKS_PER_SEC * 1000.0 / FRAMES;
    if (csv && sh_stab_export_csv(&st, csv) != SH_STAB_OK) return 1;
    if (pgm && sh_write_pgm(&cur, pgm) != 0) return 1;
    ShStabStats stt;
    sh_stab_stats(&st, &stt);
    printf("  [stats] frames=%lu jump mean=%.3f max=%.3f last=%.3f (px)\n",
           stt.frames, stt.mean, stt.max, stt.last);
    sh_stab_free(&st);
    sh_free(&cur); sh_free(&prv);
    return 0;
}

int main(void) {
    const int res[2] = {128, 512};
    RunRes raw[2], stab[2];
    printf("== shadow 时序稳定对比（%d 帧，跳变阈值 %.2f）==\n", FRAMES, (double)FLIP_THRESH);
    for (int i = 0; i < 2; ++i) {
        printf("-- res %d, raw (不抑制) --\n", res[i]);
        if (run(res[i], 0, i == 0 ? "demo_jump_raw.csv" : NULL, NULL, &raw[i]) != 0) return 1;
        printf("-- res %d, stab (平滑+斜率修正) --\n", res[i]);
        if (run(res[i], 1, i == 0 ? "demo_jump_stab.csv" : NULL, i == 0 ? "demo.pgm" : NULL, &stab[i]) != 0) return 1;
    }
    printf("\n分辨率  模式   跳变像素(均/峰)      偏移误差(px)  耗时(ms/帧)\n");
    for (int i = 0; i < 2; ++i) {
        printf("%-7d raw    %-18.0f/%-6ld %-12.3f %.3f\n",
               res[i], raw[i].flips_avg, raw[i].flips_max, raw[i].off_err, raw[i].ms_frame);
        printf("%-7d stab   %-18.0f/%-6ld %-12.3f %.3f\n",
               res[i], stab[i].flips_avg, stab[i].flips_max, stab[i].off_err, stab[i].ms_frame);
    }
    printf("跳变曲线已导出: demo_jump_raw.csv, demo_jump_stab.csv (128)\n");
    return 0;
}
