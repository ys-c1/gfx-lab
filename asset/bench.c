#define _POSIX_C_SOURCE 199309L
#include "asset.h"
#include "apack.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static unsigned long long s_state;
static unsigned int rnd(void) {
    s_state = s_state * 6364136223846793005ULL + 1442695040888963407ULL;
    return (unsigned int)(s_state >> 33);
}

static double now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

static long file_size(const char *p) {
    FILE *f = fopen(p, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long s = ftell(f);
    fclose(f);
    return s;
}

typedef struct { unsigned int off, size; } Span;
static int cmp_span(const void *pa, const void *pb) {
    const Span *a = (const Span *)pa, *b = (const Span *)pb;
    return a->off < b->off ? -1 : (a->off > b->off);
}

/* 校验布局:对齐达标率 + 无重叠。返回对齐达标块数。 */
static int check_layout(const ApBlock *b, const unsigned int *off, int n, unsigned int total) {
    Span *sp = (Span *)malloc(n * sizeof *sp);
    int aligned = 0;
    for (int i = 0; i < n; ++i) {
        if (off[i] % b[i].align == 0) ++aligned;
        sp[i].off = off[i]; sp[i].size = b[i].size;
    }
    qsort(sp, n, sizeof *sp, cmp_span);
    for (int i = 1; i < n; ++i)
        if (sp[i].off < sp[i - 1].off + sp[i - 1].size) { free(sp); return -1; }
    if (sp[n - 1].off + sp[n - 1].size > total) { free(sp); return -1; }
    free(sp);
    return aligned;
}

static void run_case(int n, int optimize, const char *label,
                     const ApBlock *blocks, unsigned long long sum) {
    unsigned int *off = (unsigned int *)malloc(n * sizeof *off);
    unsigned int total = 0;
    int reps = n <= 64 ? 3000 : 60;
    double t0 = now_us();
    for (int r = 0; r < reps; ++r)
        ap_pack_plan(blocks, n, optimize, off, &total, NULL);
    double us = (now_us() - t0) / reps;

    int aligned = check_layout(blocks, off, n, total);
    double align_rate = aligned >= 0 ? 100.0 * aligned / n : 0.0;

    /* 回填索引并实际写包,量真实包体字节数 */
    ApEntry *e = (ApEntry *)calloc(n, sizeof *e);
    unsigned char *data = (unsigned char *)malloc(total ? total : 1);
    for (unsigned int i = 0; i < total; ++i) data[i] = (unsigned char)(i * 31 + 7);
    for (int i = 0; i < n; ++i) { sprintf(e[i].name, "b%06d.bin", i); e[i].size = blocks[i].size; }
    ap_pack_apply(e, off, n);
    char path[64];
    sprintf(path, "/tmp/bench_%d_%d.gpak", n, optimize);
    long fsz = ap_write(path, e, n, data, total) == 0 ? file_size(path) : -1;

    /* 误差上界:OPT >= sum(sizes),故相对误差 <= (total - sum)/sum */
    double bound = 100.0 * (total - (double)sum) / sum;
    printf("  %-12s %10u %10ld %8.1f%% %8.1f%% %10.2f %8.2f%% %s\n",
           label, total, fsz, align_rate, 100.0 * sum / total, us, bound,
           aligned == n ? "OK" : "LAYOUT-BAD");
    free(e); free(data); free(off);
}

static void gen_blocks(ApBlock *b, int n, unsigned long long seed, unsigned long long *sum) {
    /* 混合对齐:从小对象到页对齐缓冲,尺寸 24..900 字节 */
    static const unsigned int aligns[10] = {1, 1, 2, 4, 8, 16, 64, 256, 1024, 4096};
    s_state = seed;
    *sum = 0;
    for (int i = 0; i < n; ++i) {
        b[i].size = 24 + rnd() % 877;
        b[i].align = aligns[rnd() % 10];
        *sum += b[i].size;
    }
}

int main(void) {
    int sizes[2] = {64, 1024};
    for (int s = 0; s < 2; ++s) {
        int n = sizes[s];
        ApBlock *blocks = (ApBlock *)malloc(n * sizeof *blocks);
        unsigned long long sum;
        gen_blocks(blocks, n, 0xC0FFEEULL + n, &sum);
        printf("== %d 条目 (数据量 %llu 字节) ==\n", n, sum);
        printf("  %-12s %10s %10s %9s %9s %10s %9s\n",
               "模式", "数据区字节", "包体字节", "对齐率", "利用率", "耗时us", "误差上界");
        run_case(n, AP_PACK_GREEDY, "贪心(对照)", blocks, sum);
        run_case(n, AP_PACK_TIGHT, "紧凑+回填", blocks, sum);
        free(blocks);
    }
    printf("注: 误差上界 = (布局跨度 - 数据量)/数据量, 因 OPT >= 数据量, 为保守上界。\n");
    return 0;
}
