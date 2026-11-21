#include "tile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

static void test_basic_outline(void) {
    /* 4x4 画布、2x2 块 => 2x2 网格；边缘 = 第0/2行 与 第0/2列 */
    TileOutline ol;
    CK(tile_outline_build(&ol, 4, 4, 2, 2) == TILE_OK);
    CK(ol.cols == 2 && ol.rows == 2);
    /* 轮廓像素：两行(0,2)各4 + 两列(0,2)在非边缘行(1,3)各2 = 8+4 = 12 */
    CK(ol.outline_px == 12);
    static const int expect_mask[4][4] = {
        {1,1,1,1},
        {1,0,1,0},
        {1,1,1,1},
        {1,0,1,0}
    };
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            CK(ol.mask[y*4+x] == expect_mask[y][x]);
    /* 块号 */
    CK(tile_block_at(&ol, 0, 0) == 0);
    CK(tile_block_at(&ol, 3, 0) == 1);
    CK(tile_block_at(&ol, 0, 3) == 2);
    CK(tile_block_at(&ol, 3, 3) == 3);
    CK(tile_block_at(&ol, 99, 99) == -1);
    /* 每块轮廓数之和 == 总轮廓数 */
    int sum = 0;
    for (int i = 0; i < ol.cols * ol.rows; ++i) sum += ol.edge_per_block[i];
    CK(sum == ol.outline_px);
    tile_outline_free(&ol);
}

static void test_ragged_edges(void) {
    /* 不能整除：5x5、块 2x2 => 3x3 网格，补齐块也有闭合边界 */
    TileOutline ol;
    CK(tile_outline_build(&ol, 5, 5, 2, 2) == TILE_OK);
    CK(ol.cols == 3 && ol.rows == 3);
    /* 边缘列 0,2,4；边缘行 0,2,4 => 3*5 + 3*(5-3) = 15+6 = 21 */
    CK(ol.outline_px == 21);
    CK(ol.mask[(4*5)+4] == 1); /* 画布右下角落在块边界上 */
    CK(ol.mask[(1*5)+1] == 0); /* 块内部 */
    tile_outline_free(&ol);

    /* 块尺寸等于画布：整块只有外框 */
    TileOutline one;
    CK(tile_outline_build(&one, 8, 8, 8, 8) == TILE_OK);
    CK(one.cols == 1 && one.rows == 1);
    /* 单块外框：边缘行只有行0（8 像素），边缘列只有列0，取行1..7（7 像素），合计 15 */
    CK(one.outline_px == 15);
    tile_outline_free(&one);
}

static void test_negative_args(void) {
    TileOutline ol;
    int rc;
    /* 每一项为负都要报错，并指出参数名 */
    rc = tile_outline_build(&ol, -128, 128, 16, 16);
    CK(rc == TILE_E_ARG);
    CK(strstr(ol.errbuf, "w") != NULL);
    tile_outline_free(&ol);

    rc = tile_outline_build(&ol, 128, -1, 16, 16);
    CK(rc == TILE_E_ARG);
    CK(strstr(ol.errbuf, "h") != NULL);
    tile_outline_free(&ol);

    rc = tile_outline_build(&ol, 128, 128, -4, 16);
    CK(rc == TILE_E_ARG);
    CK(strstr(ol.errbuf, "tile_w") != NULL);
    tile_outline_free(&ol);

    rc = tile_outline_build(&ol, 128, 128, 16, -8);
    CK(rc == TILE_E_ARG);
    CK(strstr(ol.errbuf, "tile_h") != NULL);
    tile_outline_free(&ol);

    /* 零值与超界也报错并命名 */
    rc = tile_outline_build(&ol, 0, 8, 4, 4);
    CK(rc == TILE_E_ARG && strstr(ol.errbuf, "w") != NULL);
    tile_outline_free(&ol);

    rc = tile_outline_build(&ol, 8, 8, 16, 4);
    CK(rc == TILE_E_ARG && strstr(ol.errbuf, "tile_w") != NULL);
    tile_outline_free(&ol);

    /* 旧路径的负参数同样逐项报错 */
    char eb[128] = {0};
    unsigned char rgb[12] = {0};
    CK(tile_grid_legacy(rgb, -8, 8, 4, 4, (unsigned char[]){255,0,255}, eb, (int)sizeof(eb)) == TILE_E_ARG);
    CK(strstr(eb, "w") != NULL);
    memset(eb, 0, sizeof(eb));
    CK(tile_grid_legacy(rgb, 8, 8, 4, -2, (unsigned char[]){255,0,255}, eb, (int)sizeof(eb)) == TILE_E_ARG);
    CK(strstr(eb, "tile_h") != NULL);
}

static void test_overlay_restore_and_parity(void) {
    enum { W = 16, H = 16, N = W * H * 3 };
    unsigned char *base = (unsigned char*)malloc(N);
    for (int i = 0; i < N; ++i) base[i] = (unsigned char)(i * 7 + 3);

    /* 新路径：两阶段 */
    TileOutline ol;
    CK(tile_outline_build(&ol, W, H, 4, 4) == TILE_OK);
    unsigned char *a = (unsigned char*)malloc(N);
    memcpy(a, base, N);
    TileOverlayStyle st = { {255, 0, 255} };
    CK(tile_overlay_apply(&ol, a, &st) == TILE_OK);
    int changed = 0; long abserr = 0;
    CK(tile_overlay_diff(&ol, &changed, &abserr) == TILE_OK);
    CK(changed == ol.outline_px);
    CK(abserr > 0);

    /* 关掉（restore）后必须逐字节回到当前输出（叠加前的 base） */
    CK(tile_overlay_restore(&ol) == TILE_OK);
    CK(memcmp(a, base, N) == 0);
    /* restore 后再 restore 是空操作，不报错 */
    CK(tile_overlay_restore(&ol) == TILE_OK);

    /* 旧路径：单遍网格 */
    unsigned char *b = (unsigned char*)malloc(N);
    memcpy(b, base, N);
    int legacy_px = tile_grid_legacy(b, W, H, 4, 4, st.rgb, NULL, 0);
    CK(legacy_px == ol.outline_px);

    /* 新路径叠加结果与旧路径逐像素一致（输出对照） */
    CK(tile_overlay_apply(&ol, a, &st) == TILE_OK);
    CK(memcmp(a, b, N) == 0);
    CK(tile_overlay_restore(&ol) == TILE_OK);
    CK(memcmp(a, base, N) == 0);

    /* 没叠加时 diff 为 0；apply 到别的缓冲被拒绝 */
    CK(ol.applied == 0);
    free(a); free(b); free(base);
    tile_outline_free(&ol);
}

static void test_export_and_ppm(void) {
    TileOutline ol;
    CK(tile_outline_build(&ol, 4, 4, 2, 2) == TILE_OK);
    /* 导出到内存流：行数应等于轮廓像素数 */
    FILE *f = tmpfile();
    CK(f != NULL);
    int rows = tile_outline_export_csv(&ol, NULL, f);
    CK(rows == ol.outline_px);
    rewind(f);
    char line[64];
    CK(fgets(line, sizeof(line), f) != NULL);
    CK(strncmp(line, "x,y,block_id,edge", 17) == 0);
    if (fgets(line, sizeof(line), f)) {
        int x, y, id, e;
        CK(sscanf(line, "%d,%d,%d,%d", &x, &y, &id, &e) == 4);
        CK(e == 1 && ol.mask[y*4+x] == 1 && ol.block_id[y*4+x] == id);
    }
    fclose(f);
    tile_outline_free(&ol);

    /* PPM 往返 */
    enum { W = 12, H = 8 };
    unsigned char *rgb = (unsigned char*)malloc(W*H*3);
    for (int i = 0; i < W*H*3; ++i) rgb[i] = (unsigned char)(i & 0xff);
    CK(tile_ppm_write("/tmp/tile_test.ppm", rgb, W, H) == TILE_OK);
    unsigned char *back = NULL; int rw = 0, rh = 0;
    char eb[128] = {0};
    CK(tile_ppm_read("/tmp/tile_test.ppm", &back, &rw, &rh, eb, (int)sizeof(eb)) == TILE_OK);
    CK(rw == W && rh == H);
    CK(memcmp(back, rgb, (size_t)W*H*3) == 0);
    free(back); free(rgb);
}

int main(void) {
    test_basic_outline();
    test_ragged_edges();
    test_negative_args();
    test_overlay_restore_and_parity();
    test_export_and_ppm();
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
