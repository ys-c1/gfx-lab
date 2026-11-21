#include "shadow.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

static void build_scene(ShLight *l, ShSeg *s) {
    /* 光源下方的水平段：点光源沿其边缘投出多像素宽的软半影，
     * 抖动平均后软边像素落在 0 与满强度之间。 */
    l->x = 32.0f; l->y = 6.0f; l->radius = 20.0f; l->intensity = 1.0f;
    s->x0 = 8.0f; s->y0 = 24.0f; s->x1 = 56.0f; s->y1 = 24.0f;
}

int main(void) {
    ShadowMap m;
    CK(sh_init(&m, 64, 64) == 0);
    ShLight l; ShSeg s;
    build_scene(&l, &s);

    /* 旧路径保持不变。 */
    CK(sh_render(&m, &l, &s, 1) == 0);
    CK(m.buf[(size_t)2 * 64 + 32] > 0.5f);
    CK(sh_render(&m, &l, NULL, 0) == 0);
    ShLight bad = {0.0f, 0.0f, 0.0f, 1.0f};
    CK(sh_render(&m, &bad, &s, 1) == -2);

    /* 开关为假：无论给什么矩阵类型，结果与 sh_render 逐像素一致。 */
    ShJitterCfg off = {0, SH_JITTER_NONE, SH_TIER_FAST, 2, NULL};
    ShadowMap a, b;
    CK(sh_init(&a, 64, 64) == 0 && sh_init(&b, 64, 64) == 0);
    CK(sh_render(&a, &l, &s, 1) == 0);
    int ns = -1;
    CK(sh_render_jit(&b, &l, &s, 1, &off, &ns) == 0);
    CK(ns == 1);
    int identical = 1;
    for (int i = 0; i < 64 * 64; ++i) if (a.buf[i] != b.buf[i]) { identical = 0; break; }
    CK(identical);
    /* NULL 配置同样等价于关闭。 */
    CK(sh_render_jit(&b, &l, &s, 1, NULL, &ns) == 0 && ns == 1);
    identical = 1;
    for (int i = 0; i < 64 * 64; ++i) if (a.buf[i] != b.buf[i]) { identical = 0; break; }
    CK(identical);

    /* 两段流程：低速档 4 点、高速档 64 点；软边像素取到 0/1 之间的平均灰阶。 */
    ShJitterCfg fast = {1, SH_JITTER_BAYER, SH_TIER_FAST, 2, NULL};
    ShJitterCfg high = {1, SH_JITTER_BAYER, SH_TIER_HIGH, 8, NULL};
    CK(sh_render_jit(&b, &l, &s, 1, &fast, &ns) == 0 && ns == 4);
    int saw_gray = 0;
    for (int i = 0; i < 64 * 64; ++i)
        if (b.buf[i] > 0.001f && b.buf[i] < 0.999f * l.intensity) { saw_gray = 1; break; }
    CK(saw_gray);
    CK(sh_render_jit(&b, &l, &s, 1, &high, &ns) == 0 && ns == 64);
    /* 纯阴影区仍为 0，受光区非零：平均没有破坏两端。 */
    CK(b.buf[(size_t)32 * 64 + 32] >= 0.0f);

    /* 冲突配置：必须报错且信息里点名两方。 */
    char err[200];
    ShJitterCfg c1 = {1, SH_JITTER_NONE, SH_TIER_FAST, 2, NULL};
    CK(sh_jitter_validate(&c1, err, (int)sizeof(err)) != 0);
    CK(strstr(err, "enabled=1") && strstr(err, "SH_JITTER_NONE"));

    ShJitterCfg c2 = {0, SH_JITTER_BAYER, SH_TIER_FAST, 2, NULL};
    CK(sh_jitter_validate(&c2, err, (int)sizeof(err)) != 0);
    CK(strstr(err, "enabled=0") && strstr(err, "BAYER"));

    ShJitterCfg c3 = {1, SH_JITTER_CUSTOM, SH_TIER_FAST, 2, NULL};
    CK(sh_jitter_validate(&c3, err, (int)sizeof(err)) != 0);
    CK(strstr(err, "CUSTOM") && strstr(err, "mat=NULL"));

    float dummy[4] = {0.1f, 0.2f, 0.3f, 0.4f};
    ShJitterCfg c4 = {1, SH_JITTER_BAYER, SH_TIER_FAST, 2, dummy};
    CK(sh_jitter_validate(&c4, err, (int)sizeof(err)) != 0);
    CK(strstr(err, "built-in") && strstr(err, "mat"));

    ShJitterCfg c5 = {1, SH_JITTER_BAYER, SH_TIER_FAST, 8, NULL};
    CK(sh_jitter_validate(&c5, err, (int)sizeof(err)) != 0);
    CK(strstr(err, "FAST") && strstr(err, "tile=8"));

    ShJitterCfg c6 = {1, SH_JITTER_BAYER, SH_TIER_HIGH, 2, NULL};
    CK(sh_jitter_validate(&c6, err, (int)sizeof(err)) != 0);
    CK(strstr(err, "HIGH") && strstr(err, "tile=2"));
    /* 渲染入口对冲突同样返回错误码，而不是默默采用某一方。 */
    CK(sh_render_jit(&b, &l, &s, 1, &c5, NULL) < 0);

    /* 自定义矩阵：可渲染、点数正确。 */
    ShJitterCfg custom = {1, SH_JITTER_CUSTOM, SH_TIER_FAST, 2, dummy};
    CK(sh_jitter_validate(&custom, NULL, 0) == 0);
    CK(sh_render_jit(&b, &l, &s, 1, &custom, &ns) == 0 && ns == 4);

    /* 矩阵生成与导出：Bayer 2x2 的四个值应覆盖四个等概率区间。 */
    float mat4[4], mat64[64];
    CK(sh_jitter_build(SH_JITTER_BAYER, 2, mat4) == 0);
    CK(mat4[0] >= 0.0f && mat4[0] < 0.25f);
    for (int i = 0; i < 4; ++i) CK(mat4[i] >= 0.0f && mat4[i] < 1.0f);
    CK(sh_jitter_build(SH_JITTER_BAYER, 8, mat64) == 0);
    CK(sh_jitter_build(SH_JITTER_ROTATED, 8, mat64) == 0);
    CK(sh_jitter_build(SH_JITTER_NONE, 8, mat64) == -1);
    CK(sh_jitter_export("jitter_8x8.txt", SH_JITTER_BAYER, 8, mat64) == 0);
    /* 回读导出文件并核对点数与取值。 */
    FILE *f = fopen("jitter_8x8.txt", "r");
    CK(f != NULL);
    if (f) {
        char header[128];
        CK(fgets(header, sizeof(header), f) != NULL);
        CK(strstr(header, "tile=8") && strstr(header, "count=64"));
        int cnt = 0; float v, lo = 2.0f, hi = -1.0f;
        while (fscanf(f, "%f", &v) == 1) { ++cnt; if (v < lo) lo = v; if (v > hi) hi = v; }
        CK(cnt == 64 && lo >= 0.0f && hi < 1.0f);
        fclose(f);
    }
    CK(sh_jitter_export("junk.txt", SH_JITTER_BAYER, 3, mat64) == -1);

    /* 旋转矩阵可能含负零：导出必须接受 -0.0f 且不写出 "-0.000000"。 */
    float rmat[4];
    sh_jitter_build(SH_JITTER_ROTATED, 2, rmat);
    CK(sh_jitter_export("jitter_rot_2x2.txt", SH_JITTER_ROTATED, 2, rmat) == 0);
    FILE *fr = fopen("jitter_rot_2x2.txt", "r");
    CK(fr != NULL);
    if (fr) {
        char line[64]; int has_neg_zero = 0;
        if (fgets(line, sizeof(line), fr) == NULL) { CK(0); }
        while (fgets(line, sizeof(line), fr))
            if (strstr(line, "-0.000000")) has_neg_zero = 1;
        CK(!has_neg_zero);
        fclose(fr);
    }
    /* NaN 与越界值必须被拒绝。 */
    float nan_mat[4] = {0.1f, 0.2f, NAN, 0.4f};
    CK(sh_jitter_export("junk2.txt", SH_JITTER_CUSTOM, 2, nan_mat) == -3);
    float bad_mat[4] = {0.1f, 0.2f, 1.0f, 0.4f};
    CK(sh_jitter_export("junk3.txt", SH_JITTER_CUSTOM, 2, bad_mat) == -3);

    /* 旋转矩阵与标准 Bayer 不同（确实被打散过），且必须是完整双射：
     * 每个元素都在 [0,1)，且作为 Bayer 值集合无重复、无遗漏。 */
    float rb[64], bb[64];
    sh_jitter_build(SH_JITTER_ROTATED, 8, rb);
    sh_jitter_build(SH_JITTER_BAYER, 8, bb);
    int differs = 0;
    for (int i = 0; i < 64; ++i) if (fabsf(rb[i] - bb[i]) > 1e-6f) { differs = 1; break; }
    CK(differs);
    /* 双射检查：旋转矩阵排序后应与标准 Bayer 排序结果完全一致（同一集合的置换）。 */
    float sr[64], sb[64];
    memcpy(sr, rb, sizeof(rb)); memcpy(sb, bb, sizeof(bb));
    for (int i = 0; i < 64; ++i)
        for (int j = i + 1; j < 64; ++j) {
            if (sr[j] < sr[i]) { float t = sr[i]; sr[i] = sr[j]; sr[j] = t; }
            if (sb[j] < sb[i]) { float t = sb[i]; sb[i] = sb[j]; sb[j] = t; }
        }
    int bijection = 1;
    for (int i = 0; i < 64; ++i) if (fabsf(sr[i] - sb[i]) > 1e-6f) { bijection = 0; break; }
    CK(bijection);
    /* 2x2 旋转矩阵同样必须填满 4 个合法槽位（回归：旧实现只写了一半）。 */
    float rb4[4];
    sh_jitter_build(SH_JITTER_ROTATED, 2, rb4);
    int all_finite = 1;
    for (int i = 0; i < 4; ++i) if (!(rb4[i] >= 0.0f && rb4[i] < 1.0f)) all_finite = 0;
    CK(all_finite);

    sh_free(&m); sh_free(&a); sh_free(&b);
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
