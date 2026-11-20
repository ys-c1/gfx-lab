#include "shadow.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

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

    /* 错误码指出字段 */
    CK(SH_ERR_RADIUS == -2);
    CK(strcmp(sh_err_field(SH_ERR_RADIUS), "radius") == 0);
    CK(strcmp(sh_err_field(SH_ERR_SIZE), "size") == 0);
    CK(strcmp(sh_err_field(SH_ERR_SAMPLES), "samples") == 0);

    /* 面光源场景：短遮挡物，半影带完整落在图内 */
    ShSeg occ = {30.0f, 24.0f, 34.0f, 24.0f};
    ShAreaLight al = {32.0f, 4.0f, 60.0f, 1.0f, 1.5f, 16};

    /* 半径为零、采样数为零分别报错并指出字段 */
    ShAreaLight al_bad_radius = al; al_bad_radius.radius = 0.0f;
    ShAreaLight al_bad_size = al; al_bad_size.size = 0.0f;
    ShAreaLight al_bad_samples = al; al_bad_samples.samples = 0;
    CK(sh_render_area(&m, &al_bad_radius, &occ, 1) == SH_ERR_RADIUS);
    CK(sh_render_area(&m, &al_bad_size, &occ, 1) == SH_ERR_SIZE);
    CK(sh_render_area(&m, &al_bad_samples, &occ, 1) == SH_ERR_SAMPLES);
    CK(sh_render_area(&m, &al, NULL, 1) == SH_ERR_ARG);

    /* 第一段：采样点分布可取到，且在发光圆盘内 */
    float pts[2 * 16];
    int n = 0;
    CK(sh_area_points(&al, pts, 16, &n) == SH_OK);
    CK(n == 16);
    int inside = 1;
    for (int k = 0; k < n; ++k) {
        float dx = pts[2 * k] - al.x, dy = pts[2 * k + 1] - al.y;
        if (sqrtf(dx * dx + dy * dy) > al.size + 1e-4f) inside = 0;
    }
    CK(inside);
    CK(sh_area_points(&al_bad_size, pts, 16, &n) == SH_ERR_SIZE);
    CK(sh_area_points(&al_bad_samples, pts, 16, &n) == SH_ERR_SAMPLES);
    n = 0;
    CK(sh_area_points(&al, pts, 4, &n) == SH_OK && n == 16); /* cap 截断，n 报总数 */

    /* 第二段：覆盖比例合成 —— 本影黑、亮区亮、半影有过渡值 */
    CK(sh_render_area(&m, &al, &occ, 1) == 0);
    CK(m.buf[(size_t)40 * 64 + 32] < 0.01f);          /* 本影 */
    CK(m.buf[(size_t)2 * 64 + 32] > 0.5f);            /* 亮区 */
    int soft = 0;
    for (int x = 0; x < 64; ++x) {                    /* 半影行有过渡像素 */
        float v = m.buf[(size_t)40 * 64 + x];
        if (v > 0.05f && v < 0.5f) soft = 1;
    }
    CK(soft);

    /* 第三段：半影带宽，与解析值对照（最底行 yg=63.5） */
    float pw = -1.0f;
    CK(sh_penumbra_width(&m, &al, &pw) == 0);
    CK(pw > 0.0f);
    float analytic = 2.0f * al.size * (63.5f - 24.0f) / (24.0f - 4.0f);
    CK(fabsf(pw - analytic) <= 2.0f);

    /* 点光源路径照旧：sh_render 结果不受面光源代码影响 */
    CK(sh_render(&m, &l, &s, 1) == 0);
    CK(m.buf[(size_t)32 * 64 + 32] < 0.01f);

    sh_free(&m);
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
