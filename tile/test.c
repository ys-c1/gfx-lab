#include "tile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

/*
 * 手算基准（纸笔可复算）：
 * 2x2 个块，每块 4x4，每个块的全部边缘像素涂成同一个纯色，内部随意。
 *   块0 (0,0) 边缘色 (10,20,30)
 *   块1 (1,0) 边缘色 (40,50,60)
 *   块2 (0,1) 边缘色 (70,80,90)
 *   块3 (1,1) 边缘色 (100,110,120)
 * 纯色边缘 => 任意倍率下采样均值都等于该纯色，四舍五入后不变。
 *
 * 每块边缘去重像素 = 4*4 - 2*2 = 12。
 * 1x：每条边采样长度即边长，四条边 = 4+4+4+4 = 16 次/块（角点计入两次）。
 * 2x：每条边 2*len-1，块 = 7+7+7+7 = 28 次/块。
 */
static const unsigned char EDGE[4][3] = {
    {10,20,30}, {40,50,60}, {70,80,90}, {100,110,120}
};

static unsigned char *make_src(int tw, int th, int cols, int rows) {
    int W = tw * cols, H = th * rows;
    unsigned char *src = (unsigned char*)calloc((size_t)W * H * 3, 1);
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c) {
            int id = r * cols + c;
            for (int ly = 0; ly < th; ++ly)
                for (int lx = 0; lx < tw; ++lx) {
                    int on_edge = lx == 0 || ly == 0 || lx == tw-1 || ly == th-1;
                    if (!on_edge) continue;
                    int gx = c * tw + lx, gy = r * th + ly;
                    unsigned char *p = src + ((size_t)gy * W + gx) * 3;
                    p[0] = EDGE[id][0]; p[1] = EDGE[id][1]; p[2] = EDGE[id][2];
                }
        }
    return src;
}

/*
 * 独立参考实现：用与 tile.c 完全不同的“逐像素判定是否落在边缘框”写法，
 * 直接构造期望画布。用于逐字节校验被测填充路径，而不是拿实现跟自己比。
 * pick_color(id) 给出某块应有的颜色，便于同时验证新/老两条路径。
 */
static void ref_fill(const TileGrid *g, int zoom, int cw, int ch,
                     unsigned char *ref,
                     void (*pick)(int id, unsigned char out[3])) {
    for (int y = 0; y < ch; ++y)
        for (int x = 0; x < cw; ++x) {
            int c = x / (g->tw * zoom);
            int r = y / (g->th * zoom);
            if (c >= g->cols || r >= g->rows) continue;
            int lx = x % (g->tw * zoom);
            int ly = y % (g->th * zoom);
            int bw = g->tw * zoom, bh = g->th * zoom;
            int on = lx < g->border || ly < g->border
                  || lx >= bw - g->border || ly >= bh - g->border;
            if (!on) continue;
            unsigned char rgb[3];
            pick(r * g->cols + c, rgb);
            unsigned char *p = ref + ((size_t)y * cw + x) * 3;
            p[0] = rgb[0]; p[1] = rgb[1]; p[2] = rgb[2];
        }
}

static void pick_legacy(int id, unsigned char out[3]) {
    (void)id;
    static const unsigned char L[3] = {200, 201, 202};
    out[0] = L[0]; out[1] = L[1]; out[2] = L[2];
}

static void pick_perblock(int id, unsigned char out[3]) {
    out[0] = EDGE[id][0]; out[1] = EDGE[id][1]; out[2] = EDGE[id][2];
}

int main(void) {
    TileGrid g = { 4, 4, 2, 2, 1 };
    unsigned char *src = make_src(g.tw, g.th, g.cols, g.rows);
    TileColor colors[4];

    /* ---- 参数与边界校验 ---- */
    CK(tile_check_grid(&g) == 0);
    TileGrid badg = { 0, 4, 2, 2, 1 };
    CK(tile_check_grid(&badg) == TILE_EINVAL);
    TileGrid negb = { 4, 4, 2, 2, -1 };
    CK(tile_check_grid(&negb) == TILE_NEG_BORDER);
    CK(tile_read_colors(&g, src, 3, colors, NULL) == TILE_EINVAL);

    int bad = 99;
    CK(tile_check_bounds(&g, 1, 8, 8, &bad) == 0 && bad == -1);
    /* 1x 画布只有 7 宽：块1/块3 右缘越界，行优先首个越界块为 1 */
    CK(tile_check_bounds(&g, 1, 7, 8, &bad) == TILE_ERANGE && bad == 1);
    /* 2x 需要 16x16；给 15 宽，首个越界块为 1 */
    CK(tile_check_bounds(&g, 2, 15, 16, &bad) == TILE_ERANGE && bad == 1);

    /* ---- 第一段：1x 读色 ---- */
    TileStats st1;
    CK(tile_read_colors(&g, src, 1, colors, &st1) == 0);
    CK(st1.samples == 16 * 4);
    for (int id = 0; id < 4; ++id) {
        CK(colors[id].samples == 16);
        CK(colors[id].rgb[0] == EDGE[id][0]);
        CK(colors[id].rgb[1] == EDGE[id][1]);
        CK(colors[id].rgb[2] == EDGE[id][2]);
    }
    /* 纯色边缘 => 边框误差为 0；各块颜色不同 => 统计误差为正 */
    CK(st1.border_mae == 0.0);
    CK(st1.stat_mae > 0.0);

    /* ---- 第一段：2x 读色，采样次数约翻倍，颜色不变 ---- */
    TileColor colors2[4];
    TileStats st2;
    CK(tile_read_colors(&g, src, 2, colors2, &st2) == 0);
    CK(st2.samples == 28 * 4);
    for (int id = 0; id < 4; ++id) {
        CK(colors2[id].samples == 28);
        CK(colors2[id].rgb[0] == EDGE[id][0]);
        CK(colors2[id].rgb[1] == EDGE[id][1]);
        CK(colors2[id].rgb[2] == EDGE[id][2]);
    }
    CK(st2.border_mae == 0.0);

    /* ---- 第二段：新路径逐块填色 ---- */
    int CW = 8, CH = 8;
    size_t bytes = (size_t)CW * CH * 3;
    unsigned char *dst_new = (unsigned char*)malloc(bytes);
    unsigned char *dst_old = (unsigned char*)malloc(bytes);
    memset(dst_new, 77, bytes);
    memset(dst_old, 77, bytes);
    unsigned char legacy[3] = {200, 201, 202};

    TileStats fs1;
    CK(tile_fill_edges(&g, colors, legacy, 1, CW, CH, dst_new, 1, &fs1) == 0);
    /* border=1、1x：每块 12 个边缘像素，4 块 = 48 */
    CK(fs1.fill_pixels == 48);
    CK(fs1.oob_block == -1);
    /* 块0 左上角必须是块0自己的边框色，而不是统一 legacy 色 */
    CK(dst_new[0] == EDGE[0][0] && dst_new[1] == EDGE[0][1]);
    /* 块1 左上角 (4,0) 是块1的色 */
    unsigned char *p1 = dst_new + (0 * CW + 4) * 3;
    CK(p1[0] == EDGE[1][0]);
    /* 块内部 (1,1) 保持预置的 77，未被填充 */
    unsigned char *pin = dst_new + (1 * CW + 1) * 3;
    CK(pin[0] == 77);

    /* ---- 老路径：统一 legacy 色 ---- */
    CK(tile_fill_edges(&g, colors, legacy, 1, CW, CH, dst_old, 0, NULL) == 0);
    CK(dst_old[0] == legacy[0] && dst_old[1] == legacy[1]);
    CK(dst_old[(0 * CW + 4) * 3] == legacy[0]);

    /* ---- 关键兼容项：关闭新逻辑走老路径，字节不变 ----
     * 同一块网格、同一预置底图，老路径两次输出必须逐字节相同。 */
    unsigned char *dst_old2 = (unsigned char*)malloc(bytes);
    memset(dst_old2, 77, bytes);
    CK(tile_fill_edges(&g, colors, legacy, 1, CW, CH, dst_old2, 0, NULL) == 0);
    CK(memcmp(dst_old, dst_old2, bytes) == 0);
    /* 新老路径在颜色不同处必然有差异，证明开关确实改变取色来源 */
    CK(memcmp(dst_new, dst_old, bytes) != 0);

    /* ---- 独立参考实现逐字节比对（新路径与老路径） ---- */
    unsigned char *ref_new = (unsigned char*)malloc(bytes);
    unsigned char *ref_old = (unsigned char*)malloc(bytes);
    memset(ref_new, 77, bytes);
    memset(ref_old, 77, bytes);
    ref_fill(&g, 1, CW, CH, ref_new, pick_perblock);
    ref_fill(&g, 1, CW, CH, ref_old, pick_legacy);
    CK(memcmp(dst_new, ref_new, bytes) == 0);
    CK(memcmp(dst_old, ref_old, bytes) == 0);

    /* ---- border=2：参考实现与被测路径仍须一致，填充像素数另算 ----
     * 每块边缘框像素 = 4*4 - (4-2*2)*(4-2*2) = 16 - 0 = 16；4 块 = 64。 */
    TileGrid g2 = g;
    g2.border = 2;
    unsigned char *dst_b2 = (unsigned char*)malloc(bytes);
    unsigned char *ref_b2 = (unsigned char*)malloc(bytes);
    memset(dst_b2, 77, bytes);
    memset(ref_b2, 77, bytes);
    TileStats fs_b2;
    CK(tile_fill_edges(&g2, colors, legacy, 1, CW, CH, dst_b2, 1, &fs_b2) == 0);
    CK(fs_b2.fill_pixels == 64);
    ref_fill(&g2, 1, CW, CH, ref_b2, pick_perblock);
    CK(memcmp(dst_b2, ref_b2, bytes) == 0);
    free(dst_b2); free(ref_b2);

    /* ---- 2x 填充：画布 16x16，每块 8x8，border=1 => 每块 28 像素 ---- */
    int CW2 = 16, CH2 = 16;
    size_t bytes2 = (size_t)CW2 * CH2 * 3;
    unsigned char *dst2 = (unsigned char*)malloc(bytes2);
    memset(dst2, 77, bytes2);
    TileStats fs2;
    CK(tile_fill_edges(&g, colors2, legacy, 2, CW2, CH2, dst2, 1, &fs2) == 0);
    CK(fs2.fill_pixels == 28 * 4);
    CK(dst2[0] == EDGE[0][0]);
    CK(dst2[((0) * CW2 + 8) * 3] == EDGE[1][0]);

    /* ---- 填充时检出越界并报块号 ---- */
    TileStats oob;
    memset(&oob, 0, sizeof(oob));
    int rc = tile_fill_edges(&g, colors, legacy, 1, 7, 8, dst_new, 1, &oob);
    CK(rc == TILE_ERANGE && oob.oob_block == 1);

    /* ---- 色表导出，供纸笔对照 ---- */
    CK(tile_export_colors(&g, colors, "colors_1x.csv") == 0);
    CK(tile_export_colors(&g, colors2, "colors_2x.csv") == 0);
    CK(tile_export_colors(&g, colors, NULL) == TILE_EINVAL);
    FILE *f = fopen("colors_1x.csv", "r");
    CK(f != NULL);
    if (f) {
        char line[128];
        CK(fgets(line, sizeof line, f) != NULL);            /* 表头 */
        CK(fgets(line, sizeof line, f) != NULL);
        CK(strncmp(line, "0,0,0,10,20,30,16", 17) == 0);    /* 块0 手算值 */
        CK(fgets(line, sizeof line, f) != NULL);
        CK(strncmp(line, "1,1,0,40,50,60,16", 17) == 0);    /* 块1 手算值 */
        fclose(f);
    }

    printf("%d checks, %d failures\n", checks, fails);
    free(src); free(dst_new); free(dst_old); free(dst_old2); free(dst2);
    free(ref_new); free(ref_old);
    return fails ? 1 : 0;
}
