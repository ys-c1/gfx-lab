#include "shadow.h"
#include <math.h>
#include <stdio.h>
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

    /* ---- 环境光染色 ---- */
    CK(sh_render(&m, &l, &s, 1) == 0); /* 重新铺上灰度亮度 */
    ShTintState st;
    CK(sh_tint_init(&st) == 0);
    float rgb[64 * 64 * 3];
    ShTintStats ts;
    const long ctr = 32 * 64 + 32; /* 已知被遮挡像素 */

    /* 开关为假：行为不变 */
    ShTintConfig off = {0};
    CK(sh_tint_render(&st, &m, &l, &s, 1, &off, rgb, &ts) == 0);
    CK(ts.pixels == 64 * 64);
    CK(fabsf(rgb[3 * ctr] - m.buf[ctr]) < 1e-6f);
    CK(fabsf(rgb[3 * ctr + 1] - m.buf[ctr]) < 1e-6f);
    CK(fabsf(rgb[3 * ctr + 2] - m.buf[ctr]) < 1e-6f);
    CK(ts.blend_ratio == 0.0 && ts.time_exact_ms == 0.0 && ts.time_fast_ms == 0.0);

    /* 冲突配置：ambient 与 shadow_color 同时给出，报错指明两方 */
    ShTintConfig conflict = {1, SH_TINT_EXACT, 1, {0.1f, 0.2f, 0.3f}, 1, {0.4f, 0.4f, 0.4f}};
    CK(sh_tint_render(&st, &m, &l, &s, 1, &conflict, rgb, &ts) == -3);
    CK(sh_tint_compare(&st, &m, &l, &s, 1, &conflict, &ts) == -3);
    ShTintConfig badmode = {1, 7, 1, {0.1f, 0.2f, 0.3f}, 0, {0, 0, 0}};
    CK(sh_tint_render(&st, &m, &l, &s, 1, &badmode, rgb, &ts) == -4);

    /* 低速档：阴影随环境色染色，不再变黑 */
    ShTintConfig cfg = {1, SH_TINT_EXACT, 1, {0.2f, 0.3f, 0.4f}, 0, {0, 0, 0}};
    CK(sh_tint_render(&st, &m, &l, &s, 1, &cfg, rgb, &ts) == 0);
    CK(ts.pixels == 64 * 64);
    CK(ts.blend_ratio > 0.0 && ts.blend_ratio < 1.0);
    CK(ts.time_exact_ms >= 0.0);
    CK(fabsf(rgb[3 * ctr] - 0.2f) < 0.05f);   /* 全遮挡处 ≈ 环境色 */
    CK(fabsf(rgb[3 * ctr + 1] - 0.3f) < 0.05f);
    CK(fabsf(rgb[3 * ctr + 2] - 0.4f) < 0.05f);
    const long lit = 2 * 64 + 32;             /* 已知受光像素 */
    CK(rgb[3 * lit] > 0.5f && rgb[3 * lit + 2] < rgb[3 * lit] + 0.4f);
    CK(ts.ambient_changes == 1);              /* 首次设置环境色 */

    /* 环境色不变不重复计数；变化才单独统计 */
    CK(sh_tint_render(&st, &m, &l, &s, 1, &cfg, rgb, &ts) == 0);
    CK(ts.ambient_changes == 1);
    cfg.ambient.b = 0.5f;
    CK(sh_tint_render(&st, &m, &l, &s, 1, &cfg, rgb, &ts) == 0);
    CK(ts.ambient_changes == 2);

    /* 高速档：色表可取得，随阴影色变化重建 */
    cfg.mode = SH_TINT_FAST;
    CK(sh_tint_render(&st, &m, &l, &s, 1, &cfg, rgb, &ts) == 0);
    CK(ts.table_rebuilds == 1);
    int tn = 0;
    const ShColor *tab = sh_tint_table(&st, &tn);
    CK(tab != NULL && tn == SH_TINT_TABLE_SIZE);
    CK(tab[0].r == 0.0f && tab[0].g == 0.0f && tab[0].b == 0.0f);
    CK(fabsf(tab[256].b - 0.5f) < 1e-6f);     /* 表尾 = 当前阴影色 */
    CK(sh_tint_render(&st, &m, &l, &s, 1, &cfg, rgb, &ts) == 0);
    CK(ts.table_rebuilds == 1);               /* 同色不重建 */
    CK(ts.ambient_changes == 2);              /* 同色不计变化 */
    cfg.ambient.g = 0.1f;
    CK(sh_tint_render(&st, &m, &l, &s, 1, &cfg, rgb, &ts) == 0);
    CK(ts.table_rebuilds == 2 && ts.ambient_changes == 3);

    /* 双档核对：高速档与穷举结果比对，误差、耗时、混合比例、像素数齐全 */
    ShTintStats cs;
    CK(sh_tint_compare(&st, &m, &l, &s, 1, &cfg, &cs) == 0);
    CK(cs.pixels == 64 * 64);
    CK(cs.tint_error >= 0.0 && cs.tint_error < 0.15);
    CK(cs.blend_ratio > 0.0 && cs.blend_ratio < 1.0);
    CK(cs.time_exact_ms >= 0.0 && cs.time_fast_ms >= 0.0);
    CK(cs.ambient_changes == 3 && cs.table_rebuilds == 2);

    sh_free(&m);
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
