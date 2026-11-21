#include "tile.h"
#include <stdio.h>
#include <math.h>

/* ---- 内部工具 ---------------------------------------------------------- */

static int valid_zoom(int zoom) { return zoom == 1 || zoom == 2; }

static int block_count(const TileGrid *g) { return g->cols * g->rows; }

/* 读取块 (c,r) 内局部坐标 (lx,ly) 的像素，src 为整幅拼接图。 */
static const unsigned char *px_at(const TileGrid *g, const unsigned char *src,
                                  int c, int r, int lx, int ly) {
    int gx = c * g->tw + lx;
    int gy = r * g->th + ly;
    return src + ((size_t)gy * (g->cols * g->tw) + gx) * 3;
}

/* 累加一个像素的 RGB 到累加器。 */
static void accum(long acc[3], long *n, const unsigned char *p) {
    acc[0] += p[0]; acc[1] += p[1]; acc[2] += p[2]; ++(*n);
}

/*
 * 沿一条长度为 len 的边采样。
 * 1x：逻辑位置 0,1,...,len-1，每个位置 1 次采样。
 * 2x：在每对相邻整数位置 i、i+1 之间插入一个中点采样，中点在两个源像素
 *     间交替落点（偶数间隔取 i，奇数间隔取 i+1），每条边采样次数为
 *     2*len-1，约为 1x 的两倍。
 * sampler 负责把边上的逻辑位置 i 映射到实际像素。
 */
typedef const unsigned char *(*sample_fn)(const TileGrid *, const unsigned char *,
                                          int, int, int);

static const unsigned char *sample_top(const TileGrid *g, const unsigned char *s,
                                       int c, int r, int i) { return px_at(g, s, c, r, i, 0); }
static const unsigned char *sample_bottom(const TileGrid *g, const unsigned char *s,
                                          int c, int r, int i) { return px_at(g, s, c, r, i, g->th - 1); }
static const unsigned char *sample_left(const TileGrid *g, const unsigned char *s,
                                        int c, int r, int i) { return px_at(g, s, c, r, 0, i); }
static const unsigned char *sample_right(const TileGrid *g, const unsigned char *s,
                                         int c, int r, int i) { return px_at(g, s, c, r, g->tw - 1, i); }

static void sample_edge(const TileGrid *g, const unsigned char *src, int zoom,
                        int c, int r, int len, sample_fn fn,
                        long acc[3], long *n) {
    for (int i = 0; i < len; ++i) {
        accum(acc, n, fn(g, src, c, r, i));
        if (zoom == 2 && i + 1 < len) {
            int mid = i + (i & 1);  /* 中点在 i 与 i+1 之间交替取源像素 */
            if (mid >= len) mid = len - 1;
            accum(acc, n, fn(g, src, c, r, mid));
        }
    }
}

/* ---- 公开 API ---------------------------------------------------------- */

int tile_check_grid(const TileGrid *g) {
    if (!g) return TILE_EINVAL;
    if (g->tw <= 0 || g->th <= 0 || g->cols <= 0 || g->rows <= 0)
        return TILE_EINVAL;
    if (g->border < 0) return TILE_NEG_BORDER;
    if (g->border > g->tw || g->border > g->th) return TILE_EINVAL;
    return 0;
}

int tile_check_bounds(const TileGrid *g, int zoom, int canvas_w, int canvas_h,
                      int *bad_block) {
    if (bad_block) *bad_block = -1;
    int rc = tile_check_grid(g);
    if (rc != 0) return rc;
    if (!valid_zoom(zoom) || canvas_w <= 0 || canvas_h <= 0) return TILE_EINVAL;
    for (int r = 0; r < g->rows; ++r)
        for (int c = 0; c < g->cols; ++c) {
            int id = r * g->cols + c;
            int x0 = c * g->tw * zoom;
            int y0 = r * g->th * zoom;
            int x1 = x0 + g->tw * zoom;   /* 排他上界 */
            int y1 = y0 + g->th * zoom;
            if (x0 < 0 || y0 < 0 || x1 > canvas_w || y1 > canvas_h) {
                if (bad_block) *bad_block = id;
                return TILE_ERANGE;
            }
        }
    return 0;
}

int tile_read_colors(const TileGrid *g, const unsigned char *src, int zoom,
                     TileColor *out, TileStats *stats) {
    int rc = tile_check_grid(g);
    if (rc != 0) return rc;
    if (!src || !out || !valid_zoom(zoom)) return TILE_EINVAL;

    long total_samples = 0;
    double stat_err_sum = 0.0;   /* 各块均值 vs 全局平均 */
    double border_err_sum = 0.0; /* 边缘采样点 vs 本块均值 */
    long border_err_n = 0;

    /* 先算全局平均色（所有块所有边缘采样点），作为统计误差基准。 */
    long gacc[3] = {0, 0, 0}, gn = 0;
    for (int r = 0; r < g->rows; ++r)
        for (int c = 0; c < g->cols; ++c) {
            long acc[3] = {0, 0, 0}, n = 0;
            sample_edge(g, src, zoom, c, r, g->tw, sample_top, acc, &n);
            sample_edge(g, src, zoom, c, r, g->tw, sample_bottom, acc, &n);
            sample_edge(g, src, zoom, c, r, g->th, sample_left, acc, &n);
            sample_edge(g, src, zoom, c, r, g->th, sample_right, acc, &n);
            gacc[0] += acc[0]; gacc[1] += acc[1]; gacc[2] += acc[2]; gn += n;
        }
    if (gn == 0) return TILE_EINVAL;
    double gmean[3] = { (double)gacc[0] / gn, (double)gacc[1] / gn,
                        (double)gacc[2] / gn };

    /* 再逐块定色，并累计两档误差。 */
    for (int r = 0; r < g->rows; ++r)
        for (int c = 0; c < g->cols; ++c) {
            int id = r * g->cols + c;
            long acc[3] = {0, 0, 0}, n = 0;
            sample_edge(g, src, zoom, c, r, g->tw, sample_top, acc, &n);
            sample_edge(g, src, zoom, c, r, g->tw, sample_bottom, acc, &n);
            sample_edge(g, src, zoom, c, r, g->th, sample_left, acc, &n);
            sample_edge(g, src, zoom, c, r, g->th, sample_right, acc, &n);

            double mean[3] = { (double)acc[0] / n, (double)acc[1] / n,
                               (double)acc[2] / n };
            for (int ch = 0; ch < 3; ++ch)
                out[id].rgb[ch] = (unsigned char)(mean[ch] + 0.5);
            out[id].samples = (int)n;
            total_samples += n;

            for (int ch = 0; ch < 3; ++ch)
                stat_err_sum += fabs(mean[ch] - gmean[ch]);

            /* 边框误差：本块每条边的实际采样像素与本块均值之差。 */
            sample_fn fns[4] = { sample_top, sample_bottom, sample_left, sample_right };
            int lens[4] = { g->tw, g->tw, g->th, g->th };
            for (int e = 0; e < 4; ++e)
                for (int i = 0; i < lens[e]; ++i) {
                    const unsigned char *p = fns[e](g, src, c, r, i);
                    for (int ch = 0; ch < 3; ++ch)
                        border_err_sum += fabs((double)p[ch] - mean[ch]);
                    ++border_err_n;
                }
        }

    if (stats) {
        /* 完整初始化，保证调用方即便没先跑 read 段，所有字段也有确定值。 */
        stats->zoom = zoom;
        stats->samples = total_samples;
        stats->fill_pixels = 0;
        stats->oob_block = -1;
        stats->stat_mae = stat_err_sum / (3.0 * block_count(g));
        stats->border_mae = border_err_n ? border_err_sum / (3.0 * border_err_n) : 0.0;
    }
    return 0;
}

/* 在画布上写一个像素，返回 0 或 TILE_ERANGE。 */
static int put_pixel(unsigned char *dst, int canvas_w, int canvas_h,
                     int x, int y, const unsigned char rgb[3]) {
    if (x < 0 || y < 0 || x >= canvas_w || y >= canvas_h) return TILE_ERANGE;
    unsigned char *p = dst + ((size_t)y * canvas_w + x) * 3;
    p[0] = rgb[0]; p[1] = rgb[1]; p[2] = rgb[2];
    return 0;
}

int tile_fill_edges(const TileGrid *g, const TileColor *colors,
                    const unsigned char legacy_rgb[3], int zoom,
                    int canvas_w, int canvas_h, unsigned char *dst,
                    int enable_new, TileStats *stats) {
    /* 入口即完整初始化：本函数只负责 fill_pixels/oob_block/zoom，其余字段
     * 置确定的零值，避免调用方传入未初始化结构时读到垃圾。需要 read 段
     * 统计时，调用方应把同一个 stats 先传给 tile_read_colors 再传进来，
     * 本函数末尾只覆盖自己负责的三个字段。 */
    if (stats) {
        stats->zoom = valid_zoom(zoom) ? zoom : 0;
        stats->samples = 0;
        stats->fill_pixels = 0;
        stats->stat_mae = 0.0;
        stats->border_mae = 0.0;
        stats->oob_block = -1;
    }

    int rc = tile_check_grid(g);
    if (rc != 0) return rc;
    if (!dst || !legacy_rgb || !valid_zoom(zoom) || canvas_w <= 0 || canvas_h <= 0)
        return TILE_EINVAL;
    if (enable_new && !colors) return TILE_EINVAL;

    int bad = -1;
    rc = tile_check_bounds(g, zoom, canvas_w, canvas_h, &bad);
    if (rc != 0) {
        if (stats) stats->oob_block = bad;
        return rc;
    }

    long filled = 0;
    int b = g->border;
    int id = -1;

    /*
     * 新老路径共用同一套遍历顺序（块行优先 → 框环 d → 上边 → 下边 →
     * 左边 → 右边），老路径仅把取色换成统一的 legacy_rgb，保证关闭新逻辑
     * 后写出的每个字节与旧版本一致。
     */
    for (int r = 0; r < g->rows; ++r)
        for (int c = 0; c < g->cols; ++c) {
            id = r * g->cols + c;
            const unsigned char *rgb = enable_new ? colors[id].rgb : legacy_rgb;
            int ox = c * g->tw * zoom;
            int oy = r * g->th * zoom;
            int bw = g->tw * zoom, bh = g->th * zoom;

            for (int d = 0; d < b; ++d) {
                int top = oy + d, bottom = oy + bh - 1 - d;
                int left = ox + d, right = ox + bw - 1 - d;
                for (int x = left; x <= right; ++x) {
                    if (put_pixel(dst, canvas_w, canvas_h, x, top, rgb) != 0) goto oob;
                    ++filled;
                    if (bottom != top) {
                        if (put_pixel(dst, canvas_w, canvas_h, x, bottom, rgb) != 0) goto oob;
                        ++filled;
                    }
                }
                for (int y = top + 1; y <= bottom - 1; ++y) {
                    if (put_pixel(dst, canvas_w, canvas_h, left, y, rgb) != 0) goto oob;
                    ++filled;
                    if (right != left) {
                        if (put_pixel(dst, canvas_w, canvas_h, right, y, rgb) != 0) goto oob;
                        ++filled;
                    }
                }
            }
        }

    if (stats) {
        stats->zoom = zoom;
        stats->fill_pixels = filled;
        stats->oob_block = -1;
        /* samples/stat_mae/border_mae 属 read 段，入口已置 0，不在此覆盖。 */
    }
    return 0;

oob:
    if (stats) {
        stats->zoom = zoom;
        stats->fill_pixels = filled;
        stats->oob_block = id;
    }
    return TILE_ERANGE;
}

int tile_export_colors(const TileGrid *g, const TileColor *colors,
                       const char *path) {
    int rc = tile_check_grid(g);
    if (rc != 0) return rc;
    if (!colors || !path) return TILE_EINVAL;
    FILE *f = fopen(path, "w");
    if (!f) return TILE_EINVAL;
    fprintf(f, "block,col,row,r,g,b,samples\n");
    for (int r = 0; r < g->rows; ++r)
        for (int c = 0; c < g->cols; ++c) {
            int id = r * g->cols + c;
            fprintf(f, "%d,%d,%d,%u,%u,%u,%d\n", id, c, r,
                    colors[id].rgb[0], colors[id].rgb[1], colors[id].rgb[2],
                    colors[id].samples);
        }
    fclose(f);
    return 0;
}
