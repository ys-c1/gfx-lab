#include "raster.h"
#include <stdio.h>
#include <string.h>

/* 固定测试场景（顺序即提交顺序），坐标按分辨率比例给出，build_scene 缩放：
   T0 大三角 z=0.8  先铺底
   T1 同形大三角 z=0.9  完全落在 T0 之后 -> 必早退
   T2 同形大三角 z=0.5  覆盖 T0
   T3 小三角 z=0.3  独立区域，不与大三角重叠
   T4 小三角 z=0.9  落在 T3 之后且完全同覆盖 -> 必早退
   T5 大三角 z=0.1  最近，铺满大三角全部像素 */
typedef struct { double bx, by; double z; unsigned char rgb[3]; } TriDef;
static const TriDef DEFS[6] = {
    { 1,  1, 0.8, {200,  40,  40}},
    { 1,  1, 0.9, { 40, 200,  40}},
    { 1,  1, 0.5, { 40,  40, 200}},
    { 2, 26, 0.3, {220, 220,  60}},
    { 2, 26, 0.9, { 60, 220, 220}},
    { 1,  1, 0.1, {230, 230, 230}},
};

static void build_scene(RTri *out, int res) {
    double s = res / 32.0;
    for (int i = 0; i < 6; ++i) {
        double bx = DEFS[i].bx * s, by = DEFS[i].by * s, z = DEFS[i].z;
        RTri big = {{
            {bx,              by,              z},
            {bx + 31.0 * s,   by,              z},
            {bx,              by + 31.0 * s,   z},
        }, {DEFS[i].rgb[0], DEFS[i].rgb[1], DEFS[i].rgb[2]}};
        RTri small = {{
            {bx,              by,              z},
            {bx + 7.0 * s,    by,              z},
            {bx,              by + 7.0 * s,    z},
        }, {DEFS[i].rgb[0], DEFS[i].rgb[1], DEFS[i].rgb[2]}};
        out[i] = (i == 3 || i == 4) ? small : big;
    }
}

static double run_case(int res, int iters, int write_ppm) {
    RTarget te, td;
    if (rt_init(&te, res, res) != 0 || rt_init(&td, res, res) != 0) {
        printf("rt_init failed at %d\n", res);
        return -1.0;
    }
    RTri tris[6];
    build_scene(tris, res);

    RStats se, sd;
    for (int it = 0; it < iters; ++it) {
        rt_stats_init(&se); rt_stats_init(&sd);
        /* 每帧重置到 rt_init 的初始状态，保证两路径输入一致 */
        for (int i = 0; i < res * res; ++i) {
            te.z[i] = 1e30f; td.z[i] = 1e30f;
            te.rgb[i*3] = te.rgb[i*3+1] = te.rgb[i*3+2] = 0;
            td.rgb[i*3] = td.rgb[i*3+1] = td.rgb[i*3+2] = 0;
        }
        if (rt_render(&te, tris, 6, 1, &se) != 0 || rt_render(&td, tris, 6, 0, &sd) != 0) {
            printf("rt_render failed at %d\n", res);
            return -1.0;
        }
    }

    int same_z = memcmp(te.z, td.z, sizeof(float) * (size_t)res * res) == 0;
    int same_rgb = memcmp(te.rgb, td.rgb, (size_t)res * res * 3) == 0;

    printf("== resolution %dx%d, %d iters ==\n", res, res, iters);
    printf("  early=ON : triangles %d, early-out %d, degenerate %d, "
           "probed %ld / shaded %ld edge-pass px, filled %ld, %.6f s/frame\n",
           se.tris_total, se.tris_early_out, se.tris_degenerate,
           se.pixels_probed, se.pixels_shaded, se.pixels_filled, se.elapsed_sec);
    printf("  early=OFF: triangles %d, early-out %d, degenerate %d, "
           "probed %ld / shaded %ld edge-pass px, filled %ld, %.6f s/frame\n",
           sd.tris_total, sd.tris_early_out, sd.tris_degenerate,
           sd.pixels_probed, sd.pixels_shaded, sd.pixels_filled, sd.elapsed_sec);
    printf("  pixel-identical (early off vs on): z=%s rgb=%s\n",
           same_z ? "yes" : "NO", same_rgb ? "yes" : "NO");
    printf("  note: only %d of %d tris are occluded here; probe+shade traverses visible "
           "tris twice, so early-out can cost more than it saves on a scene this small\n",
           se.tris_early_out, se.tris_total);

    if (write_ppm) rt_write_ppm(&td, "demo.ppm");
    rt_free(&te); rt_free(&td);
    return 0.0;
}

/* 压力场景：先铺一个近景大三角，后面堆 occluders 个完全被挡住的远三角。
   每模式重复 iters 次取最佳帧时，统计着色遍历实际被省掉多少。*/
static void run_stress(int res, int iters) {
    enum { OCC = 200 };
    static RTri tris[OCC + 1];
    double s = res / 32.0;
    tris[0] = (RTri){{ {1*s,1*s,0.1}, {30*s,1*s,0.1}, {1*s,30*s,0.1} }, {255,255,255}};
    for (int i = 0; i < OCC; ++i)
        tris[i+1] = (RTri){{ {2*s,2*s,0.9}, {29*s,2*s,0.9}, {2*s,29*s,0.9} }, {100,100,100}};

    RTarget t;
    rt_init(&t, res, res);
    double best_on = 1e9, best_off = 1e9;
    RStats st;
    for (int mode = 0; mode < 2; ++mode) {
        for (int it = 0; it < iters; ++it) {
            for (int i = 0; i < res * res; ++i) {
                t.z[i] = 1e30f;
                t.rgb[i*3] = t.rgb[i*3+1] = t.rgb[i*3+2] = 0;
            }
            rt_render(&t, tris, OCC + 1, mode == 0, &st);
            if (mode == 0 && st.elapsed_sec < best_on) best_on = st.elapsed_sec;
            if (mode == 1 && st.elapsed_sec < best_off) best_off = st.elapsed_sec;
        }
    }

    /* 再各跑一次取最终统计 */
    for (int i = 0; i < res * res; ++i) { t.z[i] = 1e30f; t.rgb[i*3]=t.rgb[i*3+1]=t.rgb[i*3+2]=0; }
    rt_render(&t, tris, OCC + 1, 1, &st);
    RStats son = st;
    for (int i = 0; i < res * res; ++i) { t.z[i] = 1e30f; t.rgb[i*3]=t.rgb[i*3+1]=t.rgb[i*3+2]=0; }
    rt_render(&t, tris, OCC + 1, 0, &st);
    RStats soff = st;

    printf("== stress %dx%d: %d fully-occluded triangles behind one front tri ==\n",
           res, res, OCC);
    printf("  early=ON : early-out %d/%d, probed %ld / shaded %ld edge-pass px, "
           "filled %ld, best %.6f s/frame\n",
           son.tris_early_out, son.tris_total, son.pixels_probed, son.pixels_shaded,
           son.pixels_filled, best_on);
    printf("  early=OFF: early-out %d/%d, probed %ld / shaded %ld edge-pass px, "
           "filled %ld, best %.6f s/frame\n",
           soff.tris_early_out, soff.tris_total, soff.pixels_probed, soff.pixels_shaded,
           soff.pixels_filled, best_off);
    long saved = soff.pixels_shaded - son.pixels_shaded;
    printf("  shade-stage traversals saved: %ld edge-pass pixels (%.1f%% of off-mode "
           "shading), speedup %.2fx\n",
           saved, 100.0 * (double)saved / (double)soff.pixels_shaded, best_off / best_on);
    rt_free(&t);
}

int main(void) {
    if (run_case(32, 2000, 0) < 0) return 1;
    if (run_case(256, 200, 1) < 0) return 1;
    run_stress(32, 2000);
    run_stress(256, 200);
    return 0;
}
