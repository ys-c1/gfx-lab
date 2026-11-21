#include "tile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * 演示：2x2 个 8x8 图块，每块边缘涂不同纯色、内部填充灰度渐变。
 * 分别在 1x / 2x 两档倍率下：
 *   1) 按块读边框色（新逻辑第一段），打印统计误差、采样次数、边框误差；
 *   2) 按读到的色填充边缘（新逻辑第二段），统计填充像素、检查越界；
 *   3) 再跑一遍老路径（统一边框色）作为对照；
 *   4) 导出各块色表 CSV，供纸笔数值对照。
 * 产物：demo.ppm（1x 新路径画布）、colors_1x.csv、colors_2x.csv。
 */

static const unsigned char EDGE[4][3] = {
    {200, 40, 40}, {40, 200, 40}, {40, 40, 200}, {200, 200, 40}
};

static unsigned char *make_src(int tw, int th, int cols, int rows) {
    int W = tw * cols, H = th * rows;
    unsigned char *src = (unsigned char*)malloc((size_t)W * H * 3);
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c) {
            int id = r * cols + c;
            for (int ly = 0; ly < th; ++ly)
                for (int lx = 0; lx < tw; ++lx) {
                    int gx = c * tw + lx, gy = r * th + ly;
                    unsigned char *p = src + ((size_t)gy * W + gx) * 3;
                    int on_edge = lx == 0 || ly == 0 || lx == tw-1 || ly == th-1;
                    if (on_edge) {
                        p[0] = EDGE[id][0]; p[1] = EDGE[id][1]; p[2] = EDGE[id][2];
                    } else {
                        unsigned char g = (unsigned char)((lx + ly) * 8);
                        p[0] = p[1] = p[2] = g;
                    }
                }
        }
    return src;
}

static void write_ppm(const char *path, const unsigned char *px, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    fwrite(px, 1, (size_t)w * h * 3, f);
    fclose(f);
}

static void run_zoom(const TileGrid *g, const unsigned char *src, int zoom,
                     const unsigned char legacy[3]) {
    int n = g->cols * g->rows;
    TileColor *colors = (TileColor*)calloc(n, sizeof(TileColor));
    TileStats rs;
    if (tile_read_colors(g, src, zoom, colors, &rs) != 0) {
        printf("zoom %dx: read failed\n", zoom);
        free(colors);
        return;
    }

    int cw = g->cols * g->tw * zoom;
    int ch = g->rows * g->th * zoom;
    size_t bytes = (size_t)cw * ch * 3;
    unsigned char *dst_new = (unsigned char*)malloc(bytes);
    unsigned char *dst_old = (unsigned char*)malloc(bytes);
    memset(dst_new, 0, bytes);
    memset(dst_old, 0, bytes);

    TileStats fs;
    int rc_new = tile_fill_edges(g, colors, legacy, zoom, cw, ch, dst_new, 1, &fs);
    TileStats os;
    int rc_old = tile_fill_edges(g, colors, legacy, zoom, cw, ch, dst_old, 0, &os);

    printf("== zoom %dx ==\n", zoom);
    printf("read : samples=%ld stat_mae=%.4f border_mae=%.4f oob=%d\n",
           rs.samples, rs.stat_mae, rs.border_mae, rs.oob_block);
    printf("fill : rc=%d fill_pixels=%ld oob_block=%d\n",
           rc_new, fs.fill_pixels, fs.oob_block);
    printf("legacy: rc=%d fill_pixels=%ld (single color %u,%u,%u)\n",
           rc_old, os.fill_pixels, legacy[0], legacy[1], legacy[2]);
    for (int id = 0; id < n; ++id)
        printf("  block %d: rgb=(%u,%u,%u) samples=%d\n",
               id, colors[id].rgb[0], colors[id].rgb[1], colors[id].rgb[2],
               colors[id].samples);

    char csv[32];
    snprintf(csv, sizeof csv, "colors_%dx.csv", zoom);
    if (tile_export_colors(g, colors, csv) != 0)
        printf("export %s failed\n", csv);

    if (zoom == 1) write_ppm("demo.ppm", dst_new, cw, ch);

    /* 演示越界检测：故意给小一圈的画布，应报出具体块号。 */
    int bad = -1;
    int rc = tile_check_bounds(g, zoom, cw - 1, ch, &bad);
    printf("bounds check on %dx%d canvas: rc=%d bad_block=%d\n\n",
           cw - 1, ch, rc, bad);

    free(colors); free(dst_new); free(dst_old);
}

int main(void) {
    TileGrid g = { 8, 8, 2, 2, 1 };
    unsigned char *src = make_src(g.tw, g.th, g.cols, g.rows);
    unsigned char legacy[3] = {250, 250, 250};
    run_zoom(&g, src, 1, legacy);
    run_zoom(&g, src, 2, legacy);
    free(src);
    return 0;
}
