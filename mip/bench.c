/*
 * bench.c — mip 层链基准与核对程序
 *
 * 对 1 千（32x32）与 1 万（100x100）两种规模分别：
 *   - 按当前可用内存（/proc/meminfo MemAvailable，失败退回 sysinfo）设定层数上限；
 *   - 按上限按需生成层链，记录耗时、实际层数、降级后的层数与每层字节；
 *   - 用独立参考实现（直接 2x2 盒式滤波，不经过被测库的逐层路径）逐层核对，
 *     报告每层级最大绝对误差与平均绝对误差；
 *   - 通过 /proc/self/status VmHWM 报告进程内存峰值。
 *
 * 降级策略（与 mip_plan 实现一致，见 src/mip.c 顶部注释）：
 *   从第 0 层起按层累加像素字节，第一层使累计值超过预算的层不再生成；
 *   若预算连第 0 层都装不下，仍保留第 0 层并标记 degraded。
 */
#define _POSIX_C_SOURCE 200809L
#include "mip.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#ifdef __linux__
#include <sys/sysinfo.h>
#endif

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static size_t mem_available(void) {
#ifdef __linux__
    FILE *f = fopen("/proc/meminfo", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            long kb;
            if (sscanf(line, "MemAvailable: %ld kB", &kb) == 1) {
                fclose(f);
                return (size_t)kb * 1024u;
            }
        }
        fclose(f);
    }
    struct sysinfo si;
    if (sysinfo(&si) == 0) return (size_t)si.freeram * si.mem_unit;
#endif
    return 0;
}

static long peak_rss_kb(void) {
#ifdef __linux__
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return -1;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        long kb;
        if (sscanf(line, "VmHWM: %ld kB", &kb) == 1) { fclose(f); return kb; }
    }
    fclose(f);
#endif
    return -1;
}

/* 独立参考：对给定层号直接从源图做 2^l 邻域盒式平均（含奇数边尺寸的钳制规则，
 * 与库实现相同的 x0/x1/y0/y1 钳制，但路径独立、不调用 mip_*）。 */
static unsigned char ref_pixel(const unsigned char *src, int w, int h, int l,
                               int x, int y, int ch) {
    int scale = 1;
    for (int i = 0; i < l; ++i) scale *= 2;
    long sum = 0; int cnt = 0;
    int ymax = y * scale + scale, xmax = x * scale + scale;
    if (ymax > h) ymax = h;
    if (xmax > w) xmax = w;
    for (int yy = y * scale; yy < ymax; ++yy)
        for (int xx = x * scale; xx < xmax; ++xx) {
            sum += src[(yy * w + xx) * 3 + ch]; ++cnt;
        }
    return (unsigned char)(sum / cnt);
}

static void run_case(int w, int h, size_t budget, const char *tag) {
    size_t src_bytes = (size_t)w * h * 3;
    unsigned char *src = (unsigned char*)malloc(src_bytes);
    if (!src) { printf("alloc failed %dx%d\n", w, h); return; }
    for (size_t i = 0; i < src_bytes; ++i)
        src[i] = (unsigned char)((i * 1103515245u + 12345u) >> 16);

    MipPlan plan;
    int idx = -1;
    if (mip_plan(w, h, budget, &plan, &idx) != 0) {
        printf("plan error idx=%d\n", idx); free(src); return;
    }

    /* 计时：小图单次在微秒级，重复 200 轮取平均，最后一轮的链保留下来做逐层核对 */
    enum { REPS = 200 };
    MipChain c;
    int rc = 0;
    double t0 = now_sec();
    for (int r = 0; r < REPS; ++r) {
        if (r) mip_free(&c);
        rc = mip_build(&c, src, w, h, budget);
    }
    double t1 = now_sec();
    if (rc != 0) printf("note: mip_build returned %d (allocation cutoff), chain kept %d levels\n",
                        rc, c.levels);

    printf("== [%s] %dx%d (%d px), src=%zu B, budget=%zu B (MemAvailable=%zu B)\n",
           tag, w, h, w * h, src_bytes, budget, mem_available());
    printf("   full_levels=%d cap_levels=%d actual_levels=%d degraded=%d build_avg=%.6f s (%d reps) peak_rss=%ld kB\n",
           plan.full_levels, plan.cap_levels, c.levels, c.degraded,
           (t1 - t0) / REPS, REPS, peak_rss_kb());
    if (c.degraded)
        printf("   DOWNGRADE: %d -> %d levels (layer %d and above not generated under budget)\n",
               plan.full_levels, c.levels, c.levels);

    /* 逐层核对：尺寸、字节数、逐像素误差，并给出层号 */
    for (int l = 0; l < c.levels; ++l) {
        long max_err = 0, sum_err = 0;
        size_t n = (size_t)c.w[l] * c.h[l] * 3;
        for (int y = 0; y < c.h[l]; ++y)
            for (int x = 0; x < c.w[l]; ++x)
                for (int ch = 0; ch < 3; ++ch) {
                    unsigned char want = ref_pixel(src, w, h, l, x, y, ch);
                    unsigned char got = c.pix[l][(y * c.w[l] + x) * 3 + ch];
                    long e = (long)got - want; if (e < 0) e = -e;
                    if (e > max_err) max_err = e;
                    sum_err += e;
                }
        printf("   layer %2d: %4dx%-4d %10zu B  max_abs_err=%ld mean_abs_err=%.4f\n",
               l, c.w[l], c.h[l], n, max_err, (double)sum_err / (double)n);
    }
    mip_free(&c);
    free(src);
}

int main(void) {
    const size_t reserve = 16u * 1024 * 1024; /* 给源图与进程其他部分预留 16 MiB */
    const struct { int w, h; } cases[] = { {32, 32}, {100, 100} }; /* 1024 / 10000 像素 */
    for (size_t k = 0; k < sizeof(cases) / sizeof(cases[0]); ++k) {
        int w = cases[k].w, h = cases[k].h;
        /* 轮 1：按当前可用内存设预算（完整链，验证默认结果） */
        size_t avail = mem_available();
        size_t budget = avail > reserve ? avail - reserve : 0;
        run_case(w, h, budget, "available-memory budget");

        /* 轮 2：人为收紧预算到只够前 2 层，验证降级路径与降级后层数 */
        MipPlan p;
        if (mip_plan(w, h, 0, &p, NULL) == 0 && p.full_levels > 2)
            run_case(w, h, p.level_bytes[0] + p.level_bytes[1], "tight budget (2 levels)");
    }
    return 0;
}
