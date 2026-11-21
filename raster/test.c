#include "raster.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

static unsigned int rng = 0x12345678u;
static double frand(void) {
    rng = rng * 1103515245u + 12345u;
    return (double)((rng >> 8) & 0xffff) / 65536.0;
}

/* 手算场景: 6x6, 三角 T1 (1,1,z.2)(5,1,.4)(1,5,.5); T2 全屏 z=.9 */
static void hand_scene(RTarget *t) {
    RTri t1 = {{ {1,1,0.2}, {5,1,0.4}, {1,5,0.5} }, {255, 0, 0}};
    RTri t2 = {{ {0,0,0.9}, {6,0,0.9}, {0,6,0.9} }, {0, 255, 0}};
    rt_triangle(t, &t1);
    rt_triangle(t, &t2);
}

int main(void) {
    /* ---- 历史行为保持 ---- */
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

    /* ---- 参数越界要报错并指明字段 ---- */
    char field[64];
    CK(rt_config_validate(NULL, field, sizeof field) == -1);
    RConfig bad;
    rt_config_default(&bad);
    bad.enable_prepass = 2;
    CK(rt_config_validate(&bad, field, sizeof field) == -3 && strcmp(field, "enable_prepass") == 0);
    rt_config_default(&bad);
    bad.track_stats = -1;
    CK(rt_config_validate(&bad, field, sizeof field) == -3 && strcmp(field, "track_stats") == 0);
    rt_config_default(&bad);
    bad.depth_eps = -0.01f;
    CK(rt_config_validate(&bad, field, sizeof field) == -3 && strcmp(field, "depth_eps") == 0);
    rt_config_default(&bad);
    bad.depth_eps = 1.01f;
    CK(rt_config_validate(&bad, field, sizeof field) == -3 && strcmp(field, "depth_eps") == 0);
    rt_config_default(&bad);
    bad.depth_eps = INFINITY;
    CK(rt_config_validate(&bad, field, sizeof field) == -3 && strcmp(field, "depth_eps") == 0);
    rt_config_default(&bad);
    bad.depth_eps = NAN;
    CK(rt_config_validate(&bad, field, sizeof field) == -3 && strcmp(field, "depth_eps") == 0);
    RConfig ok; rt_config_default(&ok);
    CK(rt_config_validate(&ok, field, sizeof field) == 0);
    CK(rt_set_config(NULL, &ok, field, sizeof field) == -1);

    /* 非法配置必须被拒绝且不改动目标 */
    CK(rt_init(&t, 16, 16) == 0);
    RConfig bad2; rt_config_default(&bad2);
    bad2.enable_prepass = 7;
    CK(rt_set_config(&t, &bad2, field, sizeof field) == -3
       && strcmp(field, "enable_prepass") == 0);
    CK(t.cfg.enable_prepass == 0 && t.zprev == NULL);

    /* ---- 手算 6x6: 近三角先画, 远平面三角随后全部被拒 ---- */
    rt_free(&t);
    CK(rt_init(&t, 6, 6) == 0);
    RConfig pp; rt_config_default(&pp);
    pp.enable_prepass = 1; pp.track_stats = 1;
    CK(rt_set_config(&t, &pp, NULL, 0) == 0);
    CK(t.zprev != NULL);
    hand_scene(&t);
    RStats s; CK(rt_get_stats(&t, &s) == 0);
    /* T1 像素(2,2) 采样中心(2.5,2.5): w0=4,w1=6,w2=6,sum=16
       z=(4*.2+6*.4+6*.5)/16 = 0.3875 */
    CK(fabsf(t.z[2*6+2] - 0.3875f) < 1e-6f);
    CK(t.rgb[(2*6+2)*3+0] == 255 && t.rgb[(2*6+2)*3+1] == 0);
    CK(s.depth_hits == s.written);          /* eps=0: 深度命中数=颜色写入数 */
    CK(s.covered == s.written + s.rejected);
    CK(s.rejected > 0);                     /* T2 大面积被 T1 遮挡拒绝 */
    CK(s.prepass_sec >= 0.0 && s.color_sec >= 0.0);

    /* 同一手算场景在单遍下结果一致 */
    RTarget ref; CK(rt_init(&ref, 6, 6) == 0);
    hand_scene(&ref);
    CK(memcmp(t.z, ref.z, sizeof(float)*36) == 0);
    CK(memcmp(t.rgb, ref.rgb, (size_t)36*3) == 0);
    rt_free(&ref);

    /* 反向绘制顺序: 先远后近, 近的全部写入 */
    rt_clear(&t);
    RTri t1 = {{ {1,1,0.2}, {5,1,0.4}, {1,5,0.5} }, {255, 0, 0}};
    RTri t2 = {{ {0,0,0.9}, {6,0,0.9}, {0,6,0.9} }, {0, 255, 0}};
    CK(rt_triangle(&t, &t2) >= 0);
    CK(rt_triangle(&t, &t1) >= 0);
    CK(fabsf(t.z[2*6+2] - 0.3875f) < 1e-6f);
    CK(t.rgb[(2*6+2)*3+0] == 255);
    RStats s2; rt_get_stats(&t, &s2);
    CK(s2.rejected == 0);                   /* 近三角没有任何像素被拒 */
    rt_free(&t);

    /* ---- 随机场景: 开关关掉与打开, 画面与深度逐像素一致 (32 与 256 两档) ---- */
    for (int dim = 0; dim < 2; ++dim) {
        int W = dim == 0 ? 32 : 256;
        RTarget a, b;
        CK(rt_init(&a, W, W) == 0 && rt_init(&b, W, W) == 0);
        RConfig cfg; rt_config_default(&cfg);
        cfg.enable_prepass = 1; cfg.track_stats = 1;
        CK(rt_set_config(&b, &cfg, NULL, 0) == 0);
        rng = 0xabcdef01u;
        int total = 0, total_ref = 0;
        for (int k = 0; k < 24; ++k) {
            RTri r;
            for (int v = 0; v < 3; ++v) {
                r.v[v].x = frand() * W;
                r.v[v].y = frand() * W;
                r.v[v].z = 0.1 + frand() * 0.9;
            }
            r.rgb[0] = (unsigned char)(frand()*255);
            r.rgb[1] = (unsigned char)(frand()*255);
            r.rgb[2] = (unsigned char)(frand()*255);
            total += rt_triangle(&b, &r);
            total_ref += rt_triangle(&a, &r);
        }
        CK(total == total_ref);
        CK(memcmp(a.z, b.z, sizeof(float)*(size_t)W*W) == 0);
        CK(memcmp(a.rgb, b.rgb, (size_t)W*W*3) == 0);
        RStats st; rt_get_stats(&b, &st);
        CK(st.covered == st.written + st.rejected);
        CK(st.depth_hits == st.written);
        CK(st.prepass_sec > 0.0 && st.color_sec > 0.0);
        rt_free(&a); rt_free(&b);
    }

    /* ---- depth_eps 行为: 远三角完全落在近三角 footprint 内时被整拒 ---- */
    CK(rt_init(&t, 8, 8) == 0);
    RConfig ec; rt_config_default(&ec);
    ec.enable_prepass = 1; ec.depth_eps = 1.0f;
    CK(rt_set_config(&t, &ec, NULL, 0) == 0);
    RTri front = {{ {0,0,0.5}, {8,0,0.5}, {0,8,0.5} }, {9, 9, 9}};
    CK(rt_triangle(&t, &front) > 0);      /* 近三角全屏, 首画全部写入 */
    RTri behind = {{ {2,2,0.9}, {6,2,0.9}, {2,6,0.9} }, {1, 1, 1}};
    int w2n = rt_triangle(&t, &behind);
    CK(w2n == 0);                          /* z=.9 不满足 .9 < .5-1, 全拒 */
    CK(t.rgb[(3*8+3)*3] == 9);             /* 内部像素保持近三角颜色 */
    rt_free(&t);

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
