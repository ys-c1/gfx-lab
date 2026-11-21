#include "shadow.h"
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

    /* 配置：类型不符要拒绝并指出字段名 */
    ShConfig c;
    sh_config_default(&c);
    CK(c.segmented == 0 && c.block == 4 && c.threshold == 0.5f);
    const char *bad_field = NULL;
    ShValue vi = {SH_T_INT, 8, 0.0f}, vf = {SH_T_FLOAT, 0, 0.25f};
    CK(sh_config_set(&c, "block", vi, &bad_field) == 0 && c.block == 8);
    CK(sh_config_set(&c, "block", vf, &bad_field) == -2);
    CK(bad_field && strcmp(bad_field, "block") == 0);
    CK(sh_config_set(&c, "threshold", vi, &bad_field) == -2);
    CK(bad_field && strcmp(bad_field, "threshold") == 0);
    CK(sh_config_set(&c, "segmented", vf, &bad_field) == -2);
    CK(bad_field && strcmp(bad_field, "segmented") == 0);
    CK(sh_config_set(&c, "nope", vi, &bad_field) == -1 && bad_field == NULL);
    ShValue vneg = {SH_T_INT, 0, 0.0f};
    CK(sh_config_set(&c, "block", vneg, &bad_field) == -3);
    CK(bad_field && strcmp(bad_field, "block") == 0);
    CK(c.block == 8); /* 被拒绝的写入不落盘 */

    /* 关闭三段式：与 sh_render 逐字节相同 */
    ShadowMap ref, off, seg1, seg8;
    CK(sh_init(&ref, 64, 64) == 0);
    CK(sh_init(&off, 64, 64) == 0);
    CK(sh_init(&seg1, 64, 64) == 0);
    CK(sh_init(&seg8, 64, 64) == 0);
    ShSeg segs[3] = {{10.0f, 20.0f, 54.0f, 20.0f},
                     {5.0f, 40.0f, 30.0f, 44.0f},
                     {40.0f, 50.0f, 60.0f, 30.0f}};
    CK(sh_render(&ref, &l, segs, 3) == 0);
    ShConfig c0; sh_config_default(&c0);
    ShStats st0;
    CK(sh_render_ex(&off, &l, segs, 3, &c0, &st0) == 0);
    CK(memcmp(ref.buf, off.buf, 64 * 64 * sizeof(float)) == 0);
    CK(st0.segments == 3 && st0.reductions == 0);

    /* 三段式 block=1 无损：同样逐字节相同 */
    ShConfig c1; sh_config_default(&c1);
    ShValue one = {SH_T_INT, 1, 0.0f}, on = {SH_T_INT, 1, 0.0f};
    CK(sh_config_set(&c1, "segmented", on, NULL) == 0);
    CK(sh_config_set(&c1, "block", one, NULL) == 0);
    ShStats st1;
    CK(sh_render_ex(&seg1, &l, segs, 3, &c1, &st1) == 0);
    CK(memcmp(ref.buf, seg1.buf, 64 * 64 * sizeof(float)) == 0);
    CK(st1.segments == 3);
    CK(st1.reductions == 3L * 64 * 64);
    CK(st1.pre_post_diff == 0.0 && st1.pre_post_cells == 0);
    CK(st1.local_bytes == 64L * 64 * (long)sizeof(float));
    CK(st1.reduced_bytes == 3L * 64 * 64 * (long)sizeof(float));

    /* 三段式 block=8：归约次数与内存按网格收缩，归约前后有差异 */
    ShConfig c8; sh_config_default(&c8);
    ShValue eight = {SH_T_INT, 8, 0.0f}, on2 = {SH_T_INT, 1, 0.0f};
    CK(sh_config_set(&c8, "segmented", on2, NULL) == 0);
    CK(sh_config_set(&c8, "block", eight, NULL) == 0);
    ShStats st8;
    CK(sh_render_ex(&seg8, &l, segs, 3, &c8, &st8) == 0);
    CK(st8.reductions == 3L * 8 * 8);
    CK(st8.reduced_bytes == 3L * 8 * 8 * (long)sizeof(float));
    CK(st8.pre_post_cells > 0 && st8.pre_post_diff > 0.0);

    /* 配置非法时拒绝渲染 */
    ShConfig cb; sh_config_default(&cb);
    cb.segmented = 1; cb.block = 0;
    CK(sh_render_ex(&seg8, &l, segs, 3, &cb, NULL) == -3);

    sh_free(&m); sh_free(&ref); sh_free(&off); sh_free(&seg1); sh_free(&seg8);
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
