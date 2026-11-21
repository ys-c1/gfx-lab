#define _POSIX_C_SOURCE 199309L
#include "mixer.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { NSRC = 6, NBUS = 3, FRAMES = 4800, REPS = 200 };

typedef struct {
    const char *name;
    int solo[NSRC], nsolo;
    int mute[NSRC], nmute;
    int valid;
} Combo;

static MixSource srcs[NSRC];
static float *srcbuf[NSRC];

/* 源 i 的单位增益恒值信号（所有声道相同），便于写出解析解 */
static float src_value(int i) { return 0.1f * (float)(i + 1); }

static void build_sources(int ch) {
    for (int i = 0; i < NSRC; ++i) {
        size_t n = (size_t)FRAMES * (size_t)ch;
        srcbuf[i] = (float*)malloc(sizeof(float) * n);
        for (size_t k = 0; k < n; ++k) srcbuf[i][k] = src_value(i);
        srcs[i].data = srcbuf[i];
        srcs[i].frames = FRAMES;
        srcs[i].bus = i % NBUS;          /* 源 0,3 -> 总线0；1,4 -> 总线1；2,5 -> 总线2 */
        srcs[i].gain = 1.0f;
    }
}

static void free_sources(void) {
    for (int i = 0; i < NSRC; ++i) { free(srcbuf[i]); srcbuf[i] = NULL; }
}

static int has_id(const int *s, int n, int id) {
    for (int i = 0; i < n; ++i) if (s[i] == id) return 1;
    return 0;
}

/* 解析解：段1 solo 筛选（空集合=全开）、段2 mute 屏蔽、段3 按总线求代数和（double） */
static double analytic(int bus, const Combo *cm) {
    double acc = 0.0;
    for (int i = 0; i < NSRC; ++i) {
        if (srcs[i].bus != bus) continue;
        if (cm->nsolo > 0 && !has_id(cm->solo, cm->nsolo, i)) continue;
        if (has_id(cm->mute, cm->nmute, i)) continue;
        acc += (double)src_value(i) * (double)srcs[i].gain;
    }
    return acc;
}

static void print_set(const char *tag, const int *s, int n) {
    printf("    %s {", tag);
    for (int i = 0; i < n; ++i) printf("%s%d", i ? "," : "", s[i]);
    printf("}\n");
}

static void run_section(int ch, const Combo *combos, int ncombo) {
    build_sources(ch);
    printf("================ 声道数 ch = %d ================\n", ch);
    printf("通道号范围 [0,%d]，源->总线: ", NSRC - 1);
    for (int i = 0; i < NSRC; ++i) printf("%d->b%d%s", i, srcs[i].bus, i + 1 < NSRC ? " " : "\n");
    printf("帧数/源 = %d，重复次数 = %d，总线数 = %d\n\n", FRAMES, REPS, NBUS);

    for (int ci = 0; ci < ncombo; ++ci) {
        const Combo *cm = &combos[ci];
        printf("组合 %d: %s\n", ci, cm->name);
        print_set("solo", cm->solo, cm->nsolo);
        print_set("mute", cm->mute, cm->nmute);

        MixReject rj = {0};
        int rc = mx_validate(NSRC, cm->solo, cm->nsolo, cm->mute, cm->nmute, &rj);
        if (!cm->valid) {
            const char *why = rc == MX_ERR_RANGE ? "MX_ERR_RANGE"
                           : rc == MX_ERR_DUP   ? "MX_ERR_DUP" : "其它";
            printf("    -> 拒绝 (%s)，冲突项如下：\n", why);
            if (rj.out_of_range_count) {
                printf("    越界通道号 {");
                for (int i = 0; i < rj.out_of_range_count; ++i) printf("%s%d", i ? "," : "", rj.out_of_range[i]);
                printf("}\n");
            }
            if (rj.duplicate_count) {
                printf("    重复通道号 {");
                for (int i = 0; i < rj.duplicate_count; ++i) printf("%s%d", i ? "," : "", rj.duplicate[i]);
                printf("}\n");
            }
            mx_reject_free(&rj);
            printf("\n");
            continue;
        }
        if (rc != MX_OK) { printf("    -> 意外拒绝 rc=%d\n\n", rc); mx_reject_free(&rj); continue; }

        MixBus buses[NBUS];
        for (int j = 0; j < NBUS; ++j) mx_init(&buses[j], FRAMES, ch);

        MixStats st = {0};
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int rep = 0; rep < REPS; ++rep) {
            for (int j = 0; j < NBUS; ++j) {
                memset(buses[j].buf, 0, sizeof(float) * (size_t)FRAMES * (size_t)ch);
                buses[j].frames = 0;
            }
            mx_mix(buses, NBUS, srcs, NSRC, cm->solo, cm->nsolo, cm->mute, cm->nmute, &st, NULL);
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        long wall_ns = (t1.tv_sec - t0.tv_sec) * 1000000000L + (t1.tv_nsec - t0.tv_nsec);

        printf("    段1通过=%d 段2屏蔽=%d 段3汇总次数=%d（每次 %d 帧 x %d 声道）\n",
               st.solo_passed, st.muted, st.aggregated, FRAMES, ch);
        printf("    上次三段耗时(ns): 筛选=%ld 屏蔽=%ld 汇总=%ld 合计=%ld；%d 次平均墙钟=%.1f us\n",
               st.nsec_stage[0], st.nsec_stage[1], st.nsec_stage[2], st.nsec_total,
               REPS, (double)wall_ns / REPS / 1000.0);
        printf("    %-6s %-10s %-12s %-12s %-12s %s\n",
               "总线", "解析解", "实测值", "绝对误差", "相对误差", "逐声道核对");
        double worst_abs = 0.0;
        for (int j = 0; j < NBUS; ++j) {
            double ref = analytic(j, cm);
            float got = buses[j].buf[0];
            double abserr = fabs((double)got - ref);
            double relerr = ref != 0.0 ? abserr / fabs(ref) : 0.0;
            if (abserr > worst_abs) worst_abs = abserr;
            int per_ch_ok = 1;
            for (int f = 0; f < FRAMES && per_ch_ok; ++f)
                for (int c = 0; c < ch; ++c) {
                    float v = buses[j].buf[(size_t)f * (size_t)ch + (size_t)c];
                    if (fabsf(v - got) > 1e-6f) { per_ch_ok = 0; break; }
                }
            printf("    b%-5d %-10.6f %-12.8f %-12.3e %-12.3e %s\n",
                   j, ref, got, abserr, relerr, per_ch_ok ? "全部一致" : "不一致");
        }
        printf("    本组合最大绝对误差 = %.3e\n\n", worst_abs);
        for (int j = 0; j < NBUS; ++j) mx_free(&buses[j]);
    }
    free_sources();
}

/* 与旧 demo 完全相同的 440Hz 单音演示，保证 demo.wav 产物不变 */
static int legacy_demo(void) {
    MixBus b;
    if (mx_init(&b, 4800, 2) != 0) return 1;
    float buf[960];
    for (int i = 0; i < 480; ++i) {
        float s = 0.3f * sinf(2.0f * 3.14159265f * 440.0f * (float)i / 48000.0f);
        buf[i*2+0] = s; buf[i*2+1] = s;
    }
    int n = mx_add(&b, buf, 480, 1.0f);
    printf("mixed %d frames\n", n);
    int rc = mx_write_wav(&b, "demo.wav");
    mx_free(&b);
    return rc == 0 ? 0 : 1;
}

int main(void) {
    int rc = legacy_demo();
    if (rc != 0) return rc;

    Combo combos[] = {
        { "none        (不开启：与旧 mx_add 逐位一致)", {0},0, {0},0, 1 },
        { "mute only   (只屏蔽 0、3)",                 {0},0, {0,3},2, 1 },
        { "solo only   (只留 1、4；其余全部哑音)",     {1,4},2, {0},0, 1 },
        { "solo+mute   (solo 0..3 再屏蔽 1、3)",       {0,1,2,3},4, {1,3},2, 1 },
        { "overlap     (solo 1、2 再 mute 1：重叠合法，仅源2存活)", {1,2},2, {1},1, 1 },
        { "solo all    (solo 全集，等价不筛选)",       {0,1,2,3,4,5},6, {0},0, 1 },
        { "REJECT range (通道号 9 越界)",              {9},1, {0},0, 0 },
        { "REJECT range (负通道号 -1)",                {0},0, {-1},1, 0 },
        { "REJECT dup   (solo 内通道1重复)",           {1,1},2, {0},0, 0 },
    };
    int ncombo = (int)(sizeof(combos) / sizeof(combos[0]));

    run_section(2, combos, ncombo);
    run_section(8, combos, ncombo);
    return 0;
}
