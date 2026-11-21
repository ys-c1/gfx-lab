#include "tile.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ---------- 内部工具 ---------- */

static void set_err(char *buf, int sz, const char *fmt, ...) {
    if (!buf || sz <= 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, (size_t)sz, fmt, ap);
    va_end(ap);
}

static int is_edge_pixel(int x, int y, int tw, int th) {
    /* 块边界：落在块的左/上边缘，或画布右/下边缘（补齐块的闭合边）。
     * 相邻块共享同一条网格线，因此轮廓像素不会被重复统计。 */
    return (x % tw == 0) || (y % th == 0);
}

/* ---------- 阶段一：按块边界描轮廓 ---------- */

int tile_outline_build(TileOutline *ol, int w, int h, int tile_w, int tile_h) {
    if (!ol) return TILE_E_ARG;
    memset(ol, 0, sizeof(*ol));

    /* 参数为负 / 非正必须报错，并明确指出是哪一项 */
    if (w < 0 || h < 0 || tile_w < 0 || tile_h < 0) {
        const char *which =
            (w < 0)      ? "w(画布宽)"      :
            (h < 0)      ? "h(画布高)"      :
            (tile_w < 0) ? "tile_w(块宽)"   : "tile_h(块高)";
        set_err(ol->errbuf, (int)sizeof(ol->errbuf),
                "参数 %s 为负值，必须 >= 0（实际：w=%d h=%d tile_w=%d tile_h=%d）",
                which, w, h, tile_w, tile_h);
        return TILE_E_ARG;
    }
    if (w == 0 || h == 0) {
        const char *which = (w == 0) ? "w(画布宽)" : "h(画布高)";
        set_err(ol->errbuf, (int)sizeof(ol->errbuf),
                "参数 %s 为 0，画布尺寸必须为正", which);
        return TILE_E_ARG;
    }
    if (tile_w == 0 || tile_h == 0) {
        const char *which = (tile_w == 0) ? "tile_w(块宽)" : "tile_h(块高)";
        set_err(ol->errbuf, (int)sizeof(ol->errbuf),
                "参数 %s 为 0，块尺寸必须为正", which);
        return TILE_E_ARG;
    }
    if (tile_w > w || tile_h > h) {
        const char *which = (tile_w > w) ? "tile_w(块宽)" : "tile_h(块高)";
        set_err(ol->errbuf, (int)sizeof(ol->errbuf),
                "参数 %s 大于画布对应维度（tile=%dx%d，画布=%dx%d）",
                which, tile_w, tile_h, w, h);
        return TILE_E_ARG;
    }

    int cols = (w + tile_w - 1) / tile_w;
    int rows = (h + tile_h - 1) / tile_h;
    size_t npix = (size_t)w * (size_t)h;

    ol->w = w; ol->h = h;
    ol->tile_w = tile_w; ol->tile_h = tile_h;
    ol->cols = cols; ol->rows = rows;
    ol->mask = (unsigned char*)calloc(npix, 1);
    ol->block_id = (int*)malloc(npix * sizeof(int));
    ol->edge_per_block = (int*)calloc((size_t)cols * (size_t)rows, sizeof(int));
    if (!ol->mask || !ol->block_id || !ol->edge_per_block) {
        tile_outline_free(ol);
        set_err(ol->errbuf, (int)sizeof(ol->errbuf), "内存分配失败");
        return TILE_E_MEM;
    }

    int outline = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            size_t i = (size_t)y * (size_t)w + (size_t)x;
            int c = x / tile_w, r = y / tile_h;
            int id = r * cols + c;
            ol->block_id[i] = id;
            if (is_edge_pixel(x, y, tile_w, tile_h)) {
                ol->mask[i] = 1;
                ++outline;
                ++ol->edge_per_block[(size_t)id];
            }
        }
    }
    ol->outline_px = outline;
    return TILE_OK;
}

void tile_outline_free(TileOutline *ol) {
    if (!ol) return;
    free(ol->mask);
    free(ol->block_id);
    free(ol->edge_per_block);
    free(ol->backup);
    ol->mask = NULL; ol->block_id = NULL; ol->edge_per_block = NULL;
    ol->backup = NULL; ol->target = NULL;
    ol->applied = 0; ol->outline_px = 0;
}

int tile_block_at(const TileOutline *ol, int x, int y) {
    if (!ol || !ol->block_id || x < 0 || y < 0 || x >= ol->w || y >= ol->h)
        return -1;
    return ol->block_id[(size_t)y * (size_t)ol->w + (size_t)x];
}

int tile_outline_export_csv(const TileOutline *ol, const char *path, FILE *stream) {
    if (!ol || !ol->mask || (!path && !stream)) return TILE_E_ARG;
    FILE *f = stream;
    int opened = 0;
    if (path) {
        if (strcmp(path, "-") == 0) {
            f = stdout;
        } else {
            f = fopen(path, "wb");
            if (!f) return TILE_E_IO;
            opened = 1;
        }
    }
    fprintf(f, "x,y,block_id,edge\n");
    int rows = 0;
    for (int y = 0; y < ol->h; ++y)
        for (int x = 0; x < ol->w; ++x) {
            size_t i = (size_t)y * (size_t)ol->w + (size_t)x;
            if (!ol->mask[i]) continue; /* 只导出轮廓像素，控制表体积 */
            fprintf(f, "%d,%d,%d,1\n", x, y, ol->block_id[i]);
            ++rows;
        }
    if (opened) fclose(f);
    return rows;
}

/* ---------- 阶段二：把轮廓叠加到预览 ---------- */

int tile_overlay_apply(TileOutline *ol, unsigned char *rgb,
                       const TileOverlayStyle *style) {
    if (!ol || !ol->mask || !rgb) {
        if (ol) set_err(ol->errbuf, (int)sizeof(ol->errbuf),
                        "apply 参数 ol/rgb 为空");
        return TILE_E_ARG;
    }
    TileOverlayStyle def = { {255, 0, 255} };
    const TileOverlayStyle *st = style ? style : &def;

    /* 首次叠加：备份整张预览，保证 restore 能回到当前输出 */
    if (!ol->applied) {
        size_t n = (size_t)ol->w * (size_t)ol->h * 3u;
        ol->backup = (unsigned char*)malloc(n);
        if (!ol->backup) {
            set_err(ol->errbuf, (int)sizeof(ol->errbuf), "内存分配失败");
            return TILE_E_MEM;
        }
        memcpy(ol->backup, rgb, n);
        ol->target = rgb;
    } else if (ol->target != rgb) {
        set_err(ol->errbuf, (int)sizeof(ol->errbuf),
                "apply 目标缓冲与已叠加缓冲不一致，请先 restore");
        return TILE_E_ARG;
    }

    int changed = 0;
    long abs_err = 0;
    size_t npix = (size_t)ol->w * (size_t)ol->h;
    for (size_t i = 0; i < npix; ++i) {
        if (!ol->mask[i]) continue;
        for (int ch = 0; ch < 3; ++ch) {
            unsigned char old = rgb[i * 3 + ch];
            unsigned char nv = st->rgb[ch];
            abs_err += (long)abs((int)nv - (int)old);
            rgb[i * 3 + ch] = nv;
        }
        ++changed;
    }
    ol->applied = 1;
    ol->last_changed = changed;
    ol->last_abs_err = abs_err;
    return TILE_OK;
}

int tile_overlay_restore(TileOutline *ol) {
    if (!ol) return TILE_E_ARG;
    if (!ol->applied || !ol->backup || !ol->target) {
        ol->applied = 0;
        return TILE_OK; /* 没叠加过时为空操作：当前输出本来就没被动过 */
    }
    size_t n = (size_t)ol->w * (size_t)ol->h * 3u;
    memcpy(ol->target, ol->backup, n);
    free(ol->backup);
    ol->backup = NULL;
    ol->target = NULL;
    ol->applied = 0;
    return TILE_OK;
}

int tile_overlay_diff(const TileOutline *ol, int *changed_px,
                      long *abs_channel_error) {
    if (!ol) return TILE_E_ARG;
    if (changed_px) *changed_px = ol->last_changed;
    if (abs_channel_error) *abs_channel_error = ol->last_abs_err;
    return TILE_OK;
}

/* ---------- 旧路径（对照基线）：单遍直接画网格 ---------- */

int tile_grid_legacy(unsigned char *rgb, int w, int h,
                     int tile_w, int tile_h, const unsigned char color[3],
                     char *errbuf, int errbuf_sz) {
    if (w < 0 || h < 0 || tile_w < 0 || tile_h < 0) {
        const char *which =
            (w < 0)      ? "w"      :
            (h < 0)      ? "h"      :
            (tile_w < 0) ? "tile_w" : "tile_h";
        if (errbuf && errbuf_sz > 0)
            snprintf(errbuf, (size_t)errbuf_sz,
                     "legacy 参数 %s 为负值（w=%d h=%d tile_w=%d tile_h=%d）",
                     which, w, h, tile_w, tile_h);
        return TILE_E_ARG;
    }
    if (!rgb || w <= 0 || h <= 0 || tile_w <= 0 || tile_h <= 0 ||
        tile_w > w || tile_h > h) {
        if (errbuf && errbuf_sz > 0)
            snprintf(errbuf, (size_t)errbuf_sz, "legacy 参数非法");
        return TILE_E_ARG;
    }
    unsigned char c0 = color ? color[0] : 255;
    unsigned char c1 = color ? color[1] : 0;
    unsigned char c2 = color ? color[2] : 255;
    int drawn = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            if (!is_edge_pixel(x, y, tile_w, tile_h)) continue;
            size_t i = ((size_t)y * (size_t)w + (size_t)x) * 3u;
            rgb[i + 0] = c0; rgb[i + 1] = c1; rgb[i + 2] = c2;
            ++drawn;
        }
    return drawn;
}

/* ---------- PPM P6 读写 ---------- */

int tile_ppm_write(const char *path, const unsigned char *rgb, int w, int h) {
    if (!path || !rgb || w <= 0 || h <= 0) return TILE_E_ARG;
    FILE *f = fopen(path, "wb");
    if (!f) return TILE_E_IO;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    size_t n = (size_t)w * (size_t)h * 3u;
    if (fwrite(rgb, 1, n, f) != n) { fclose(f); return TILE_E_IO; }
    fclose(f);
    return TILE_OK;
}

/* 读 PPM 头，跳过空白与 # 注释 */
static int read_token(FILE *f, char *tok, int max) {
    int c;
    do {
        do { c = fgetc(f); } while (c != EOF && (c == ' ' || c == '\t' ||
                                                 c == '\n' || c == '\r' || c == '\v'));
        if (c == '#') { while (c != EOF && c != '\n') c = fgetc(f); }
    } while (c == '#' || c == ' ' || c == '\n' || c == '\t' || c == '\r' || c == '\v');
    int i = 0;
    while (c != EOF && c != ' ' && c != '\t' && c != '\n' &&
           c != '\r' && c != '\v' && i < max - 1) {
        tok[i++] = (char)c;
        c = fgetc(f);
    }
    tok[i] = '\0';
    return i > 0 ? 0 : -1;
}

int tile_ppm_read(const char *path, unsigned char **rgb, int *w, int *h,
                  char *errbuf, int errbuf_sz) {
    if (!path || !rgb || !w || !h) return TILE_E_ARG;
    *rgb = NULL; *w = 0; *h = 0;
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (errbuf && errbuf_sz > 0)
            snprintf(errbuf, (size_t)errbuf_sz, "无法打开 %s", path);
        return TILE_E_IO;
    }
    char tok[16];
    if (read_token(f, tok, (int)sizeof(tok)) != 0 || strcmp(tok, "P6") != 0) {
        if (errbuf && errbuf_sz > 0)
            snprintf(errbuf, (size_t)errbuf_sz, "不是 P6 PPM 文件");
        fclose(f); return TILE_E_FMT;
    }
    int iw, ih, maxv;
    if (read_token(f, tok, (int)sizeof(tok)) != 0 ||
        (iw = atoi(tok)) <= 0 ||
        read_token(f, tok, (int)sizeof(tok)) != 0 ||
        (ih = atoi(tok)) <= 0 ||
        read_token(f, tok, (int)sizeof(tok)) != 0 ||
        (maxv = atoi(tok)) <= 0 || maxv != 255) {
        if (errbuf && errbuf_sz > 0)
            snprintf(errbuf, (size_t)errbuf_sz, "PPM 头非法");
        fclose(f); return TILE_E_FMT;
    }
    /* read_token 已消费 maxval 之后的单个分隔空白，下一个字节即像素数据 */
    size_t n = (size_t)iw * (size_t)ih * 3u;
    unsigned char *buf = (unsigned char*)malloc(n);
    if (!buf) { fclose(f); return TILE_E_MEM; }
    if (fread(buf, 1, n, f) != n) {
        free(buf); fclose(f);
        if (errbuf && errbuf_sz > 0)
            snprintf(errbuf, (size_t)errbuf_sz, "PPM 像素数据不完整");
        return TILE_E_FMT;
    }
    fclose(f);
    *rgb = buf; *w = iw; *h = ih;
    return TILE_OK;
}
