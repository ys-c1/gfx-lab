#define _POSIX_C_SOURCE 199309L
#include "tile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* 简单可移植计时（秒，单调钟） */
static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static unsigned char *make_base(int w, int h) {
    size_t n = (size_t)w * (size_t)h * 3u;
    unsigned char *p = (unsigned char*)malloc(n);
    if (!p) return NULL;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            unsigned char *q = p + ((size_t)y * (size_t)w + (size_t)x) * 3u;
            q[0] = (unsigned char)(x & 0xff);
            q[1] = (unsigned char)(y & 0xff);
            q[2] = (unsigned char)(((x ^ y) * 9) & 0xff);
        }
    return p;
}

static long buf_abs_diff(const unsigned char *a, const unsigned char *b, size_t n) {
    long s = 0;
    for (size_t i = 0; i < n; ++i) s += (long)abs((int)a[i] - (int)b[i]);
    return s;
}

static int run_case(int size, int tile) {
    const size_t npix3 = (size_t)size * (size_t)size * 3u;
    char path[160];

    unsigned char *base = make_base(size, size);
    unsigned char *new_rgb = (unsigned char*)malloc(npix3);
    unsigned char *legacy_rgb = (unsigned char*)malloc(npix3);
    if (!base || !new_rgb || !legacy_rgb) {
        printf("size=%d 内存分配失败\n", size);
        free(base); free(new_rgb); free(legacy_rgb);
        return 1;
    }
    memcpy(new_rgb, base, npix3);
    memcpy(legacy_rgb, base, npix3);

    printf("==== size=%d tile=%d ====\n", size, tile);

    /* 阶段一：按块边界描轮廓（只建表，不碰预览像素） */
    TileOutline ol;
    double t0 = now_sec();
    int rc = tile_outline_build(&ol, size, size, tile, tile);
    double t_build = now_sec() - t0;
    if (rc != TILE_OK) {
        printf("tile_outline_build 失败: %s\n", ol.errbuf);
        return 1;
    }
    printf("网格 %d 列 x %d 行，共 %d 块；轮廓像素数=%d；建表耗时 %.6f s\n",
           ol.cols, ol.rows, ol.cols * ol.rows, ol.outline_px, t_build);

    /* 抽样报块号：中心与四角 */
    const int probes[][2] = {{0,0},{size-1,0},{0,size-1},{size/2,size/2},{size-1,size-1}};
    for (unsigned k = 0; k < sizeof(probes)/sizeof(probes[0]); ++k) {
        int x = probes[k][0], y = probes[k][1];
        printf("  像素(%d,%d) 块号=%d 该块轮廓像素=%d\n",
               x, y, tile_block_at(&ol, x, y),
               ol.edge_per_block[tile_block_at(&ol, x, y)]);
    }
    /* 每块轮廓数合计应等于总轮廓数 */
    int esum = 0;
    for (int i = 0; i < ol.cols * ol.rows; ++i) esum += ol.edge_per_block[i];
    printf("  各块轮廓像素合计=%d（应等于总轮廓像素 %d）\n", esum, ol.outline_px);

    /* 阶段二：把轮廓叠加到预览 */
    TileOverlayStyle st = { {255, 0, 255} };
    t0 = now_sec();
    rc = tile_overlay_apply(&ol, new_rgb, &st);
    double t_apply = now_sec() - t0;
    if (rc != TILE_OK) { printf("apply 失败: %s\n", ol.errbuf); return 1; }
    int changed = 0; long abserr = 0;
    tile_overlay_diff(&ol, &changed, &abserr);
    printf("叠加：改写像素=%d，通道绝对误差合计=%ld，叠加耗时 %.6f s\n",
           changed, abserr, t_apply);
    printf("两阶段总耗时 %.6f s\n", t_build + t_apply);

    /* 旧路径：单遍直接画网格（对照基线），同色 */
    char eb[128] = {0};
    t0 = now_sec();
    int legacy_px = tile_grid_legacy(legacy_rgb, size, size, tile, tile,
                                     st.rgb, eb, (int)sizeof(eb));
    double t_legacy = now_sec() - t0;
    if (legacy_px < 0) { printf("legacy 失败: %s\n", eb); return 1; }
    printf("旧路径：轮廓像素=%d，耗时 %.6f s\n", legacy_px, t_legacy);

    /* 新旧路径输出对照：逐像素比较 */
    int mismatch = memcmp(new_rgb, legacy_rgb, npix3) != 0;
    long cross_err = buf_abs_diff(new_rgb, legacy_rgb, npix3);
    printf("新旧路径逐像素对照：%s（通道绝对误差合计=%ld）\n",
           mismatch ? "不一致" : "完全一致", cross_err);

    /* 关掉调试：还原到当前输出，并验证与原始 base 逐字节一致 */
    rc = tile_overlay_restore(&ol);
    int restored = (memcmp(new_rgb, base, npix3) == 0);
    printf("restore 后回到当前输出：%s（rc=%d）\n", restored ? "是" : "否", rc);

    /* 落盘产物，便于人工/外部工具对照 */
    snprintf(path, sizeof(path), "demo_%d_base.ppm", size);
    tile_ppm_write(path, base, size, size);
    snprintf(path, sizeof(path), "demo_%d_overlay.ppm", size);
    tile_overlay_apply(&ol, new_rgb, &st); /* 重新叠加后再导出调试图 */
    tile_ppm_write(path, new_rgb, size, size);
    tile_overlay_restore(&ol);
    snprintf(path, sizeof(path), "demo_%d_legacy.ppm", size);
    tile_ppm_write(path, legacy_rgb, size, size);
    snprintf(path, sizeof(path), "demo_%d_outline.csv", size);
    int csv_rows = tile_outline_export_csv(&ol, path, NULL);
    printf("已导出：%s（%d 行轮廓）、base/overlay/legacy 三个 PPM\n\n", path, csv_rows);

    int outline_px = ol.outline_px;
    free(base); free(new_rgb); free(legacy_rgb);
    tile_outline_free(&ol);
    return (mismatch || !restored || cross_err != 0 || legacy_px != outline_px) ? 1 : 0;
}

int main(void) {
    int bad = 0;
    bad |= run_case(128, 16);
    bad |= run_case(1024, 128);

    /* 负参数必须报错并指出项名 */
    TileOutline ol;
    int rc = tile_outline_build(&ol, 1024, 1024, -16, 128);
    printf("负参数检查：rc=%d，说明：%s\n", rc, rc == TILE_E_ARG ? ol.errbuf : "(未报错!)");
    if (rc != TILE_E_ARG) bad = 1;
    tile_outline_free(&ol);

    return bad ? 1 : 0;
}
