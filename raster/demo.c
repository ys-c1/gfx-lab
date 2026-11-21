#include "raster.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define NTRI 48

static unsigned int rng = 0x5f3759dfu;
static double frand(void) {
    rng = rng * 1103515245u + 12345u;
    return (double)((rng >> 8) & 0xffff) / 65536.0;
}

/* 同屏大三角形、前后穿插: 遮挡多、被拒像素多, 且每帧序列确定性可复现 */
static void build_scene(int W, RTri *tris) {
    rng = (unsigned int)W * 2654435761u + 1u;
    for (int k = 0; k < NTRI; ++k) {
        double cx = frand() * W, cy = frand() * W;
        double span = W * (0.25 + 0.25 * frand());
        for (int v = 0; v < 3; ++v) {
            double ang = frand() * 6.2831853;
            tris[k].v[v].x = cx + cos(ang) * span;
            tris[k].v[v].y = cy + sin(ang) * span;
            tris[k].v[v].z = 0.1 + frand() * 0.9;
        }
        tris[k].rgb[0] = (unsigned char)(frand() * 255);
        tris[k].rgb[1] = (unsigned char)(frand() * 255);
        tris[k].rgb[2] = (unsigned char)(frand() * 255);
    }
}

typedef struct { double total, prepass, color, best; } FrameTime;

static double run_mode(RTarget *t, const RTri *tris, int frames, int prepass,
                       FrameTime *ft, RStats *acc) {
    RConfig cfg;
    rt_config_default(&cfg);
    cfg.enable_prepass = prepass;
    cfg.track_stats = 1;
    char field[64];
    if (rt_set_config(t, &cfg, field, sizeof field) != 0) {
        printf("config error: %s\n", field);
        return -1.0;
    }
    ft->total = ft->prepass = ft->color = 0.0;
    ft->best = 1e300;
    memset(acc, 0, sizeof *acc);
    for (int f = 0; f < frames; ++f) {
        rt_clear(t);  /* 同时清零逐帧统计 */
        for (int k = 0; k < NTRI; ++k) rt_triangle(t, &tris[k]);
        RStats now; rt_get_stats(t, &now);
        double spent = now.prepass_sec + now.color_sec;
        if (spent < ft->best) ft->best = spent;
        ft->prepass += now.prepass_sec;
        ft->color += now.color_sec;
        ft->total += spent;
        acc->covered += now.covered;
        acc->depth_hits += now.depth_hits;
        acc->rejected += now.rejected;
        acc->written += now.written;
    }
    ft->prepass /= frames;
    ft->color /= frames;
    ft->total /= frames;
    return 0.0;
}

static int count_diffs(const RTarget *a, const RTarget *b, int *zdiff, int *rgbdiff) {
    int n = a->w * a->h, nz = 0, nr = 0;
    for (int i = 0; i < n; ++i) {
        if (a->z[i] != b->z[i]) ++nz;
        if (a->rgb[i*3] != b->rgb[i*3] || a->rgb[i*3+1] != b->rgb[i*3+1]
            || a->rgb[i*3+2] != b->rgb[i*3+2]) ++nr;
    }
    *zdiff = nz; *rgbdiff = nr;
    return nz + nr;
}

static void hand_check(void) {
    puts("== 手算核对 (6x6) ==");
    RTarget t;
    if (rt_init(&t, 6, 6) != 0) return;
    RTri t1 = {{ {1,1,0.2}, {5,1,0.4}, {1,5,0.5} }, {255, 0, 0}};
    RTri t2 = {{ {0,0,0.9}, {6,0,0.9}, {0,6,0.9} }, {0, 255, 0}};
    rt_triangle(&t, &t1);
    rt_triangle(&t, &t2);
    /* 像素 (2,2), 采样中心 (2.5,2.5):
       w0=4, w1=6, w2=6, sum=16; z=(4*.2+6*.4+6*.5)/16 = 0.3875 */
    float z = t.z[2*6+2];
    printf("  pixel(2,2): hand z=0.3875 raster z=%.6f %s, rgb=(%d,%d,%d) 期望(255,0,0)\n",
           z, fabsf(z - 0.3875f) < 1e-6f ? "OK" : "MISMATCH",
           t.rgb[(2*6+2)*3], t.rgb[(2*6+2)*3+1], t.rgb[(2*6+2)*3+2]);
    rt_free(&t);
}

static void validation_demo(void) {
    puts("== 参数越界校验 ==");
    struct { const char *name; RConfig cfg; } cases[] = {
        {"enable_prepass=2",  (RConfig){2, 0, 0.0f}},
        {"track_stats=-1",    (RConfig){0, -1, 0.0f}},
        {"depth_eps=-0.5",    (RConfig){0, 0, -0.5f}},
        {"depth_eps=2.0",     (RConfig){0, 0, 2.0f}},
        {"depth_eps=INFINITY",(RConfig){0, 0, INFINITY}},
        {"depth_eps=NAN",     (RConfig){0, 0, NAN}},
        {"valid(prepass on)", (RConfig){1, 1, 0.0f}},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        char field[64] = "";
        int rc = rt_config_validate(&cases[i].cfg, field, sizeof field);
        if (rc == 0) printf("  %-20s OK\n", cases[i].name);
        else printf("  %-20s rc=%d field=%s\n", cases[i].name, rc, field);
    }
}

int main(void) {
    validation_demo();
    hand_check();

    int sizes[2] = {32, 256};
    int frames[2] = {2000, 200};
    for (int si = 0; si < 2; ++si) {
        int W = sizes[si], F = frames[si];
        printf("\n== 基准 %dx%d, %d 帧 x %d 三角形 ==\n", W, W, F, NTRI);
        RTri tris[NTRI];
        build_scene(W, tris);

        RTarget single, prepass;
        if (rt_init(&single, W, W) != 0 || rt_init(&prepass, W, W) != 0) return 1;
        FrameTime ts, tp;
        RStats ss, sp;
        if (run_mode(&single, tris, F, 0, &ts, &ss) < 0) return 1;
        if (run_mode(&prepass, tris, F, 1, &tp, &sp) < 0) return 1;

        long pixels = (long)F * (long)W * W;
        printf("%-22s %16s %16s\n", "指标", "单遍(关)", "预通道(开)");
        printf("%-22s %16.3f %16.3f  us/帧(均值)\n", "总耗时", ts.total*1e6, tp.total*1e6);
        printf("%-22s %16.3f %16.3f  us/帧(最佳)\n", "最佳帧耗时", ts.best*1e6, tp.best*1e6);
        printf("%-22s %16s %16.3f  us/帧\n", "  预通道耗时", "-", tp.prepass*1e6);
        printf("%-22s %16.3f %16.3f  us/帧\n", "  颜色遍耗时", ts.color*1e6, tp.color*1e6);
        printf("%-22s %16ld %16ld  累计\n", "覆盖像素评估", ss.covered, sp.covered);
        printf("%-22s %16ld %16ld  累计\n", "深度命中(深度缓冲更新)", ss.depth_hits, sp.depth_hits);
        printf("%-22s %16ld %16ld  累计\n", "颜色写入", ss.written, sp.written);
        printf("%-22s %16s %16ld  累计\n", "被深度拒绝像素", "-", sp.rejected);
        printf("%-22s %16.2f %16.2f  %%\n", "拒绝率(被拒/覆盖)",
               100.0 * (double)ss.rejected / (double)ss.covered,
               100.0 * (double)sp.rejected / (double)sp.covered);
        printf("%-22s %16.6f %16.6f  占画面像素\n", "每像素评估次数",
               (double)ss.covered / (double)pixels, (double)sp.covered / (double)pixels);

        int zd = 0, cd = 0;
        count_diffs(&single, &prepass, &zd, &cd);
        puts("");
        printf("两遍画面差异: 深度不一致像素=%d, 颜色不一致像素=%d (期望均为 0)\n", zd, cd);
        double err = 0.0, maxe = 0.0;
        for (int i = 0; i < W*W; ++i) {
            double d = fabs((double)single.z[i] - (double)prepass.z[i]);
            err += d; if (d > maxe) maxe = d;
        }
        printf("深度误差: 平均=%.3g 最大=%.3g (位精确时为 0)\n",
               err / (double)(W*W), maxe);

        if (W == 32 && rt_write_ppm(&prepass, "demo.ppm") != 0) {
            printf("ppm write failed\n");
            return 1;
        }
        rt_free(&single);
        rt_free(&prepass);
    }
    puts("\ndone.");
    return 0;
}
