#include "mixer.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

static int has_id_local(const int *set, int n, int id);

/* 恒值交织信号：第 i 个源的每个采样都是 base + i*step，所有声道相同 */
static void fill_const(float *buf, int frames, int ch, float v) {
    for (int i = 0; i < frames * ch; ++i) buf[i] = v;
}

/* 参考实现：用 overlay 原语把一组源从帧 0 起汇总到对应总线 */
static void ref_mix(MixBus *buses, const MixSource *srcs, int nsrc) {
    for (int i = 0; i < nsrc; ++i) {
        const MixSource *s = &srcs[i];
        mx_mix_into(&buses[s->bus], s->data, s->frames, s->gain);
    }
}

static void test_legacy(void) {
    MixBus b;
    CK(mx_init(&b, 8, 2) == 0);
    float a[8], c[8];
    for (int i = 0; i < 8; ++i) { a[i] = 0.25f; c[i] = 0.5f; }
    CK(mx_add(&b, a, 4, 1.0f) == 4);
    CK(mx_add(&b, c, 4, 0.5f) == 4);
    CK(fabsf(b.buf[0] - 0.25f) < 1e-6f);
    CK(fabsf(b.buf[8] - 0.25f) < 1e-6f);
    CK(mx_add(&b, a, 4, 1.0f) == 0);
    mx_free(&b);

    /* overlay 与流式追加的区别：两个同位置 0.25 叠加成 0.5，写游标停在帧4 */
    MixBus o;
    CK(mx_init(&o, 8, 2) == 0);
    CK(mx_mix_into(&o, a, 4, 1.0f) == 4);
    CK(mx_mix_into(&o, a, 4, 1.0f) == 4);
    CK(fabsf(o.buf[0] - 0.5f) < 1e-6f);
    CK(fabsf(o.buf[2] - 0.5f) < 1e-6f);
    CK(o.frames == 4);
    mx_free(&o);
}

/* 不开启 solo/mute：mx_mix 结果必须与直接 overlay 参考路径逐位相同，2ch 与 8ch 都验 */
static void test_identity(int ch) {
    enum { NSRC = 5, NBUS = 3, FR = 64 };
    MixBus ref[NBUS], got[NBUS];
    MixSource srcs[NSRC];
    size_t smp = (size_t)FR * (size_t)ch;
    for (int j = 0; j < NBUS; ++j) {
        CK(mx_init(&ref[j], FR, ch) == 0);
        CK(mx_init(&got[j], FR, ch) == 0);
    }
    for (int i = 0; i < NSRC; ++i) {
        float *buf = (float*)malloc(sizeof(float) * smp);
        fill_const(buf, FR, ch, 0.1f * (float)(i + 1));
        srcs[i].data = buf; srcs[i].frames = FR;
        srcs[i].bus = i % NBUS; srcs[i].gain = 0.5f + 0.1f * (float)i;
    }
    ref_mix(ref, srcs, NSRC);
    MixStats st;
    MixReject rj;
    CK(mx_mix(got, NBUS, srcs, NSRC, NULL, 0, NULL, 0, &st, &rj) == MX_OK);
    CK(st.stages == 3 && st.solo_passed == NSRC && st.muted == 0 && st.aggregated == NSRC);
    for (int j = 0; j < NBUS; ++j) {
        CK(memcmp(ref[j].buf, got[j].buf, sizeof(float) * smp) == 0);
        CK(got[j].frames == ref[j].frames);
    }
    for (int i = 0; i < NSRC; ++i) free((void*)srcs[i].data);
    for (int j = 0; j < NBUS; ++j) { mx_free(&ref[j]); mx_free(&got[j]); }
}

/* 三段语义：solo 筛选、mute 屏蔽、solo+mute 联合、solo 单独开启（哑音复现） */
static void test_stages(void) {
    enum { NSRC = 6, NBUS = 2, FR = 8 };
    MixBus buses[NBUS];
    MixSource srcs[NSRC];
    for (int j = 0; j < NBUS; ++j) CK(mx_init(&buses[j], FR, 2) == 0);
    for (int i = 0; i < NSRC; ++i) {
        float *buf = (float*)malloc(sizeof(float) * FR * 2);
        fill_const(buf, FR, 2, 1.0f);
        srcs[i].data = buf; srcs[i].frames = FR;
        srcs[i].bus = i % NBUS; srcs[i].gain = 1.0f;
    }
    MixStats st; MixReject rj;

    /* solo={2}：只有源2通过并汇总，源2在总线0，总线1整段为 0（其余全被滤掉） */
    int solo1[] = {2};
    CK(mx_mix(buses, NBUS, srcs, NSRC, solo1, 1, NULL, 0, &st, &rj) == MX_OK);
    CK(st.solo_passed == 1 && st.muted == 0 && st.aggregated == 1);
    CK(fabsf(buses[0].buf[0] - 1.0f) < 1e-6f);
    CK(fabsf(buses[1].buf[0] - 0.0f) < 1e-6f);
    for (int j = 0; j < NBUS; ++j) { mx_free(&buses[j]); CK(mx_init(&buses[j], FR, 2) == 0); }

    /* 只 mute={0,1}：不过滤 solo，仅屏蔽两个源 */
    int mute1[] = {0, 1};
    CK(mx_mix(buses, NBUS, srcs, NSRC, NULL, 0, mute1, 2, &st, &rj) == MX_OK);
    CK(st.solo_passed == NSRC && st.muted == 2 && st.aggregated == 4);
    /* 总线0：源0被屏蔽，剩源2、4 -> 2.0；总线1：源1被屏蔽，剩源3、5 -> 2.0 */
    CK(fabsf(buses[0].buf[0] - 2.0f) < 1e-6f);
    CK(fabsf(buses[1].buf[0] - 2.0f) < 1e-6f);
    for (int j = 0; j < NBUS; ++j) { mx_free(&buses[j]); CK(mx_init(&buses[j], FR, 2) == 0); }

    /* solo={0,1,2} 且 mute={1}：solo 先留 3 个，mute 再去掉 1 个，汇总源0与源2 */
    int solo2[] = {0, 1, 2}, mute2[] = {1};
    CK(mx_mix(buses, NBUS, srcs, NSRC, solo2, 3, mute2, 1, &st, &rj) == MX_OK);
    CK(st.solo_passed == 3 && st.muted == 1 && st.aggregated == 2);
    CK(fabsf(buses[0].buf[0] - 2.0f) < 1e-6f); /* 源0+源2 */
    CK(fabsf(buses[1].buf[0] - 0.0f) < 1e-6f);

    for (int i = 0; i < NSRC; ++i) free((void*)srcs[i].data);
    for (int j = 0; j < NBUS; ++j) mx_free(&buses[j]);
}

static void test_reject(void) {
    enum { NSRC = 4 };
    MixReject rj; int rc;

    /* solo 与 mute 重叠是合法组合：段1 通过、段2 屏蔽，不拒绝 */
    int solo1[] = {1, 2}, mute1[] = {2, 1, 3};
    rj = (MixReject){0};
    rc = mx_validate(NSRC, solo1, 2, mute1, 3, &rj);
    CK(rc == MX_OK && rj.out_of_range_count == 0 && rj.duplicate_count == 0);
    mx_reject_free(&rj);

    /* 越界：通道号超出 [0,NSRC) */
    int solo2[] = {4}, mute2[] = {-1};
    rj = (MixReject){0};
    rc = mx_validate(NSRC, solo2, 1, mute2, 1, &rj);
    CK(rc == MX_ERR_RANGE && rj.out_of_range_count == 2);
    CK(has_id_local(rj.out_of_range, rj.out_of_range_count, 4));
    CK(has_id_local(rj.out_of_range, rj.out_of_range_count, -1));
    mx_reject_free(&rj);

    /* 重复：同一集合内重复通道号 */
    int solo3[] = {0, 0}, mute3[] = {2, 2, 2};
    rj = (MixReject){0};
    rc = mx_validate(NSRC, solo3, 2, mute3, 3, &rj);
    CK(rc == MX_ERR_DUP && rj.duplicate_count == 2);
    CK(has_id_local(rj.duplicate, rj.duplicate_count, 0));
    CK(has_id_local(rj.duplicate, rj.duplicate_count, 2));
    mx_reject_free(&rj);

    /* 参数错误：负长度 / 有长度却空指针 */
    CK(mx_validate(NSRC, NULL, -1, NULL, 0, NULL) == MX_ERR_ARG);
    CK(mx_validate(NSRC, solo3, 1, NULL, 0, NULL) == MX_OK);
    CK(mx_validate(NSRC, NULL, 1, NULL, 0, NULL) == MX_ERR_ARG);

    /* 非法组合下总线不得被改动 */
    MixBus b; CK(mx_init(&b, 8, 2) == 0);
    fill_const(b.buf, 8, 2, 0.75f);
    MixSource s = {b.buf, 4, 0, 1.0f};
    int bad_solo[] = {9};
    rj = (MixReject){0};
    CK(mx_mix(&b, 1, &s, 1, bad_solo, 1, NULL, 0, NULL, &rj) == MX_ERR_RANGE);
    CK(rj.out_of_range_count == 1 && rj.out_of_range[0] == 9);
    CK(fabsf(b.buf[0] - 0.75f) < 1e-6f);
    mx_reject_free(&rj);

    /* 总线号非法：段3 拦截，返回 MX_ERR_BUS，且不改动总线 */
    int ok_solo[] = {0};
    MixSource bad = {b.buf, 4, 7, 1.0f};
    CK(mx_mix(&b, 1, &bad, 1, ok_solo, 1, NULL, 0, NULL, NULL) == MX_ERR_BUS);
    CK(fabsf(b.buf[0] - 0.75f) < 1e-6f);
    mx_free(&b);
}

/* 重叠语义端到端：solo={1,2} mute={1} -> 仅源2 存活 */
static void test_overlap(void) {
    enum { NSRC = 3, NBUS = 1, FR = 4 };
    MixBus bus; CK(mx_init(&bus, FR, 2) == 0);
    MixSource srcs[NSRC];
    float *bufs[NSRC];
    for (int i = 0; i < NSRC; ++i) {
        bufs[i] = (float*)malloc(sizeof(float) * FR * 2);
        fill_const(bufs[i], FR, 2, 1.0f);
        srcs[i] = (MixSource){bufs[i], FR, 0, 1.0f};
    }
    int solo[] = {1, 2}, mute[] = {1};
    MixStats st; MixReject rj = {0};
    CK(mx_mix(&bus, NBUS, srcs, NSRC, solo, 2, mute, 1, &st, &rj) == MX_OK);
    CK(st.solo_passed == 2 && st.muted == 1 && st.aggregated == 1);
    CK(fabsf(bus.buf[0] - 1.0f) < 1e-6f);
    mx_reject_free(&rj);
    for (int i = 0; i < NSRC; ++i) free(bufs[i]);
    mx_free(&bus);
}

/* test.c 内小工具，避免引入源文件内部 static 函数 */
static int has_id_local(const int *set, int n, int id) {
    for (int i = 0; i < n; ++i) if (set[i] == id) return 1;
    return 0;
}

int main(void) {
    test_legacy();
    test_identity(2);
    test_identity(8);
    test_stages();
    test_overlap();
    test_reject();
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
