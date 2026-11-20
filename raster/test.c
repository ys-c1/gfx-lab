#include "raster.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

static void build_scene32(RTri *out) {
    out[0] = (RTri){{ {1,1,0.8},  {32,1,0.8},  {1,32,0.8}  }, {200, 40, 40}};
    out[1] = (RTri){{ {1,1,0.9},  {32,1,0.9},  {1,32,0.9}  }, {40, 200, 40}};
    out[2] = (RTri){{ {1,1,0.5},  {32,1,0.5},  {1,32,0.5}  }, {40, 40, 200}};
    out[3] = (RTri){{ {2,26,0.3}, {9,26,0.3},  {2,33,0.3}  }, {220, 220, 60}};
    out[4] = (RTri){{ {2,26,0.9}, {9,26,0.9},  {2,33,0.9}  }, {60, 220, 220}};
    out[5] = (RTri){{ {1,1,0.1},  {32,1,0.1},  {1,32,0.1}  }, {230, 230, 230}};
}

int main(void) {
    /* --- 原有接口行为保持不变 --- */
    RTarget t;
    CK(rt_init(&t, 32, 32) == 0);
    RTri tri = {{ {4,4,0.5}, {28,4,0.5}, {16,28,0.5} }, {255, 0, 0}};
    int n = rt_triangle(&t, &tri);
    CK(n > 200);
    CK(t.z[16*32+16] < 1.0f);
    CK(t.rgb[(16*32+16)*3] == 255);
    RTri flat = {{ {0,0,0}, {8,8,0}, {16,16,0} }, {0,255,0}};
    CK(rt_triangle(&t, &flat) == -2);
    rt_free(&t);

    /* --- 固定场景：早退开/关，逐三角形手算对照（32x32）--- */
    RTri scene[6];
    build_scene32(scene);

    RTarget te, td;
    CK(rt_init(&te, 32, 32) == 0);
    CK(rt_init(&td, 32, 32) == 0);
    RStats se, sd;
    CK(rt_render(&te, scene, 6, 1, &se) == 0);
    CK(rt_render(&td, scene, 6, 0, &sd) == 0);

    CK(se.tris_total == 6);
    CK(se.tris_early_out == 2);       /* T1、T4 被完全遮挡 */
    CK(se.tris_degenerate == 0);
    CK(se.pixels_probed == 2038L);    /* 探测阶段：6 个三角形全部跑边测试，496x4+27x2 */
    CK(se.pixels_shaded == 1515L);    /* 着色阶段：仅 4 个通过者 T0/T2/T3/T5，496x3+27 */
    CK(se.pixels_inside == 3553L);    /* 探测+着色两阶段之和 */
    CK(se.pixels_filled == 1515L);    /* 496+496+27+496，手算见 refcheck */
    CK(sd.tris_early_out == 0);
    CK(sd.pixels_probed == 0);        /* 关闭早退：无探测阶段 */
    CK(sd.pixels_shaded == 2038L);    /* 6 个三角形全部进入着色阶段 */
    CK(sd.pixels_inside == 2038L);
    CK(sd.pixels_filled == 1515L);    /* 最终写入量两模式相同 */
    CK(sd.tris_total == 6);

    /* 关闭早退时画面与开启早退逐像素相同 */
    CK(memcmp(te.z, td.z, sizeof(float) * 32u * 32u) == 0);
    CK(memcmp(te.rgb, td.rgb, 32u * 32u * 3u) == 0);

    /* --- rt_tri_visible 只读：调用前后缓冲不变 --- */
    RTarget probe_t;
    CK(rt_init(&probe_t, 32, 32) == 0);
    RStats sprobe;
    CK(rt_render(&probe_t, scene, 6, 1, &sprobe) == 0);  /* 干净缓冲上渲染场景 */
    size_t bytes_z = sizeof(float) * 32u * 32u, bytes_rgb = 32u * 32u * 3u;
    unsigned char *z_snap = malloc(bytes_z), *rgb_snap = malloc(bytes_rgb);
    CK(z_snap && rgb_snap);
    memcpy(z_snap, probe_t.z, bytes_z);
    memcpy(rgb_snap, probe_t.rgb, bytes_rgb);
    CK(rt_tri_visible(&probe_t, &scene[0]) == 0);   /* T0 已被更近的 T2/T5 全覆盖 */
    CK(rt_tri_visible(&probe_t, &scene[2]) == 0);    /* T2 又被最近的 T5 全覆盖 */
    CK(rt_tri_visible(&probe_t, &scene[3]) == 0);    /* T3 区域同样被 T5 覆盖 */
    CK(rt_tri_visible(&probe_t, &scene[5]) == 0);    /* T5 最近：再探自身，0.1<0.1 为假 */

    /* 全新空缓冲（z=1e30）上探测必有通过像素，且缓冲保持只读 */
    RTarget fresh;
    CK(rt_init(&fresh, 32, 32) == 0);
    CK(rt_tri_visible(&fresh, &scene[3]) == 27);     /* 与 refcheck 的小三角边测试数一致 */
    CK(fresh.z[27 * 32 + 3] == 1e30f);               /* T3 内部像素深度未被写入 */
    CK(fresh.rgb[(27 * 32 + 3) * 3] == 0);           /* 颜色缓冲同样保持只读 */
    rt_free(&fresh);
    CK(memcmp(probe_t.z, z_snap, bytes_z) == 0);
    CK(memcmp(probe_t.rgb, rgb_snap, bytes_rgb) == 0);
    free(z_snap); free(rgb_snap);
    rt_free(&probe_t);

    /* --- 非有限深度：报错并指出三角形号 --- */
    RTri bad[6];
    build_scene32(bad);
    bad[2].v[1].z = INFINITY;
    RStats sbad;
    CK(rt_render(&te, bad, 3, 1, &sbad) == -3);
    CK(sbad.error_index == 2);
    CK(rt_triangle(&td, &bad[2]) == -3);
    CK(rt_tri_visible(&td, &bad[2]) == -3);

    RTri bad2[6];
    build_scene32(bad2);
    bad2[1].v[2].z = NAN;
    CK(rt_render(&te, bad2, 3, 0, &sbad) == -3);
    CK(sbad.error_index == 1);

    /* --- 目标缓冲为空：报错并指出三角形号 --- */
    RTarget empty;
    CK(rt_init(&empty, 8, 8) == 0);
    rt_free(&empty);  /* z/rgb 置 NULL */
    CK(rt_render(&empty, scene, 6, 1, &sbad) == -4);
    CK(sbad.error_index == 0);
    /* 单三角形入口的空缓冲属于参数非法，返回 -1 */
    CK(rt_triangle(&empty, &scene[0]) == -1);
    CK(rt_tri_visible(&empty, &scene[0]) == -1);
    /* --- 退化三角形统计，不影响画面 --- */
    RStats sflat;
    CK(rt_render(&te, &flat, 1, 1, &sflat) == 0);
    CK(sflat.tris_degenerate == 1);
    CK(sflat.pixels_filled == 0);

    rt_free(&te); rt_free(&td);
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
