#include "shadow.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

/* 两帧手算比对用：8x8，光源 (2.5,0.5)，竖直遮挡段 x=4。
 * 帧A 偏移 (0,0)：左半亮右半影；帧B 原始偏移 (2,0)：光源移到段右侧，整图翻转 → 64 像素全跳。
 * k=0.5 平滑后帧B 输出偏移 (1,0)：光源仍在段左侧，阴影分布不变 → 0 像素跳。 */
static int count_flips(const ShadowMap *a, const ShadowMap *b) {
    int n = 0;
    for (int i = 0; i < a->w * a->h; ++i)
        if ((a->buf[i] == 0.0f) != (b->buf[i] == 0.0f)) ++n;
    return n;
}

int main(void) {
    ShadowMap m;
    CK(sh_init(&m, 64, 64) == 0);
    ShLight l = {32.0f, 8.0f, 20.0f, 1.0f};
    ShSeg s = {10.0f, 20.0f, 54.0f, 20.0f};
    CK(sh_render(&m, &l, &s, 1) == 0);
    CK(m.buf[(size_t)32 * 64 + 32] < 0.01f);
    CK(m.buf[(size_t)2 * 64 + 32] > 0.5f);
    CK(sh_render(&m, &l, NULL, 0) == 0);
    ShLight bad = {0.0f, 0.0f, 0.0f, 1.0f};
    CK(sh_render(&m, &bad, &s, 1) == -2);

    /* sh_render_off 零偏移与 sh_render 逐位一致 */
    {
        ShadowMap a, b;
        CK(sh_init(&a, 32, 32) == 0 && sh_init(&b, 32, 32) == 0);
        ShLight l2 = {16.0f, 4.0f, 12.0f, 1.0f};
        ShSeg s2 = {6.0f, 12.0f, 26.0f, 12.0f};
        CK(sh_render(&a, &l2, &s2, 1) == 0);
        CK(sh_render_off(&b, &l2, &s2, 1, 0.0f, 0.0f) == 0);
        CK(memcmp(a.buf, b.buf, 32 * 32 * sizeof(float)) == 0);
        sh_free(&a); sh_free(&b);
    }

    /* 错误：字段 k 越界 / slope_k 越界 / 前帧缺失，均指出字段 */
    {
        ShStab st; ShStabSample o;
        CK(sh_stab_init(&st) == SH_STAB_OK);
        ShStabCfg c = {0.5f, 0.0f, 1};
        c.k = -0.1f;
        CK(sh_stab_frame(&st, &c, 0, 0, 0, 0, &o) == SH_STAB_E_K);
        c.k = 1.5f;
        CK(sh_stab_frame(&st, &c, 0, 0, 0, 0, &o) == SH_STAB_E_K);
        c.k = NAN;
        CK(sh_stab_frame(&st, &c, 0, 0, 0, 0, &o) == SH_STAB_E_K);
        c.k = 0.5f; c.slope_k = -0.5f;
        CK(sh_stab_frame(&st, &c, 0, 0, 0, 0, &o) == SH_STAB_E_SLOPE);
        c.slope_k = 0.0f;
        CK(sh_stab_frame(&st, &c, 1, 0, 0, 0, &o) == SH_STAB_E_PREV); /* 未 seed */
        CK(strstr(sh_stab_err(SH_STAB_E_K), "k") != NULL);
        CK(strstr(sh_stab_err(SH_STAB_E_SLOPE), "slope_k") != NULL);
        CK(strstr(sh_stab_err(SH_STAB_E_PREV), "prev") != NULL);
        sh_stab_free(&st);
    }

    /* 两帧手算比对：seed (0,0)，k=0.5，slope_k=0
     * 帧1 raw=(1,0)：sm = 0 + 0.5*(1-0) = 0.5，jump = |1-0| = 1
     * 帧2 raw=(1,0)：sm = 0.5 + 0.5*(1-0.5) = 0.75，jump = |1-0.5| = 0.5
     * 统计：frames=2, mean=(1+0.5)/2=0.75, max=1, last=0.5 */
    {
        ShStab st; ShStabSample o; ShStabStats stt;
        CK(sh_stab_init(&st) == SH_STAB_OK);
        CK(sh_stab_seed(&st, 0.0f, 0.0f) == SH_STAB_OK);
        ShStabCfg c = {0.5f, 0.0f, 1};
        CK(sh_stab_frame(&st, &c, 1.0f, 0.0f, 0, 0, &o) == SH_STAB_OK);
        CK(fabsf(o.out_x - 0.5f) < 1e-6f && fabsf(o.out_y) < 1e-6f);
        CK(fabsf(o.jump - 1.0f) < 1e-6f);
        CK(sh_stab_frame(&st, &c, 1.0f, 0.0f, 0, 0, &o) == SH_STAB_OK);
        CK(fabsf(o.out_x - 0.75f) < 1e-6f);
        CK(fabsf(o.jump - 0.5f) < 1e-6f);
        CK(sh_stab_stats(&st, &stt) == SH_STAB_OK);
        CK(stt.frames == 2);
        CK(fabs(stt.mean - 0.75) < 1e-6 && fabs(stt.max - 1.0) < 1e-6 && fabs(stt.last - 0.5) < 1e-6);
        sh_stab_free(&st);
    }

    /* 斜率修正手算：k=1（不平滑），slope_k=0.25，slope=(2,-4) → 修正量 (0.5,-1) */
    {
        ShStab st; ShStabSample o;
        sh_stab_init(&st);
        sh_stab_seed(&st, 0.0f, 0.0f);
        ShStabCfg c = {1.0f, 0.25f, 1};
        CK(sh_stab_frame(&st, &c, 0.0f, 0.0f, 2.0f, -4.0f, &o) == SH_STAB_OK);
        CK(fabsf(o.out_x - 0.5f) < 1e-6f && fabsf(o.out_y + 1.0f) < 1e-6f);
        sh_stab_free(&st);
    }

    /* 不抑制：输出逐位等于原始偏移，渲染结果与未启用稳定时相同 */
    {
        ShStab st; ShStabSample o;
        sh_stab_init(&st);
        ShStabCfg c = {0.5f, 0.3f, 0}; /* suppress=0，k/slope_k 不应生效 */
        CK(sh_stab_frame(&st, &c, 0.3f, -0.2f, 5.0f, 5.0f, &o) == SH_STAB_OK);
        CK(o.out_x == 0.3f && o.out_y == -0.2f);
        CK(sh_stab_frame(&st, &c, 1.7f, 0.9f, 5.0f, 5.0f, &o) == SH_STAB_OK);
        CK(o.out_x == 1.7f && o.out_y == 0.9f);
        ShadowMap a, b;
        sh_init(&a, 16, 16); sh_init(&b, 16, 16);
        ShLight l3 = {8.0f, 2.0f, 9.0f, 1.0f};
        ShSeg s3 = {3.0f, 6.0f, 13.0f, 6.0f};
        sh_render_off(&a, &l3, &s3, 1, 1.7f, 0.9f);
        sh_render_off(&b, &l3, &s3, 1, o.out_x, o.out_y);
        CK(memcmp(a.buf, b.buf, 16 * 16 * sizeof(float)) == 0);
        sh_free(&a); sh_free(&b);
        sh_stab_free(&st);
    }

    /* 两帧手算像素比对：8x8 整图翻转 64 像素；k=0.5 平滑后 0 像素跳变 */
    {
        ShadowMap fa, fb;
        CK(sh_init(&fa, 8, 8) == 0 && sh_init(&fb, 8, 8) == 0);
        ShLight l4 = {2.5f, 0.5f, 50.0f, 1.0f};
        ShSeg wall = {4.0f, 0.0f, 4.0f, 8.0f};
        CK(sh_render_off(&fa, &l4, &wall, 1, 0.0f, 0.0f) == 0);
        CK(sh_render_off(&fb, &l4, &wall, 1, 2.0f, 0.0f) == 0); /* raw 帧B */
        CK(count_flips(&fa, &fb) == 64);
        ShStab st; ShStabSample o;
        sh_stab_init(&st);
        sh_stab_seed(&st, 0.0f, 0.0f);
        ShStabCfg c = {0.5f, 0.0f, 1};
        CK(sh_stab_frame(&st, &c, 2.0f, 0.0f, 0, 0, &o) == SH_STAB_OK);
        CK(fabsf(o.out_x - 1.0f) < 1e-6f); /* 手算：0 + 0.5*(2-0) = 1 */
        CK(sh_render_off(&fb, &l4, &wall, 1, o.out_x, o.out_y) == 0);
        CK(count_flips(&fa, &fb) == 0);
        sh_stab_free(&st);
        sh_free(&fa); sh_free(&fb);
    }

    /* 跳变曲线导出 */
    {
        ShStab st; ShStabSample o;
        sh_stab_init(&st);
        sh_stab_seed(&st, 0.0f, 0.0f);
        ShStabCfg c = {0.5f, 0.0f, 1};
        sh_stab_frame(&st, &c, 1.0f, 0.0f, 0, 0, &o);
        sh_stab_frame(&st, &c, 1.0f, 0.0f, 0, 0, &o);
        CK(sh_stab_export_csv(&st, "test_jump.csv") == SH_STAB_OK);
        FILE *f = fopen("test_jump.csv", "r");
        CK(f != NULL);
        if (f) {
            char line[128];
            CK(fgets(line, sizeof line, f) != NULL);
            CK(strstr(line, "frame,raw_x,raw_y,out_x,out_y,jump") == line);
            int rows = 0;
            while (fgets(line, sizeof line, f)) ++rows;
            CK(rows == 2);
            fclose(f);
            remove("test_jump.csv");
        }
        sh_stab_free(&st);
    }

    sh_free(&m);
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
