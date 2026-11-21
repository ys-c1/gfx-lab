#ifndef TILE_H
#define TILE_H

#include <stdio.h>

/*
 * tile 模块：图块边界轮廓调试。
 *
 * 分两个阶段，各自独立、可单独调用：
 *   阶段一 tile_outline_build —— 按 tile 网格划分块，仅生成块边界轮廓表
 *                                （不接触任何预览像素）。
 *   阶段二 tile_overlay_apply  —— 把轮廓表叠加到一份预览图上，输出调试图。
 *
 * 关掉调试时不要调用阶段二，原始预览缓冲原样保留，即“回到当前输出”；
 * 也可以用 tile_overlay_restore() 还原被叠加阶段改写过的像素。
 */

/* 错误码 */
enum {
    TILE_OK = 0,
    TILE_E_ARG = -1,     /* 通用参数错误，详见 errbuf 中指出的具体参数名 */
    TILE_E_MEM = -2,     /* 内存分配失败 */
    TILE_E_IO  = -3,     /* 文件读写失败 */
    TILE_E_FMT = -4      /* PPM 文件格式错误 */
};

typedef struct {
    int w, h;            /* 画布宽高（像素），必须为正 */
    int tile_w, tile_h;  /* 块宽、块高（像素），必须为正且不大于画布 */
    int cols, rows;      /* 网格列数/行数（自动计算） */
    unsigned char *mask; /* w*h，1=轮廓像素，0=块内部像素 */
    int *block_id;       /* w*h，每个像素所属块号（行主序，id = r*cols+c） */
    int *edge_per_block; /* cols*rows，每个块贡献的轮廓像素数（按属主块统计，
                          * 所有块之和恰等于 outline_px） */
    int outline_px;      /* 轮廓像素总数 */
    /* 阶段二的叠加状态：首次 apply 时备份整张预览，restore 原样还原 */
    unsigned char *target;  /* 当前叠加的预览缓冲，未叠加时为 NULL */
    unsigned char *backup;  /* w*h*3，叠加前的像素备份 */
    int applied;            /* 是否处于已叠加状态 */
    int last_changed;       /* 上次叠加改写的像素数 */
    long last_abs_err;      /* 上次叠加的通道绝对误差之和 */
    char errbuf[128];    /* 最近一次错误的可读说明，指出具体参数项 */
} TileOutline;

typedef struct {
    unsigned char rgb[3]; /* 轮廓颜色 */
} TileOverlayStyle;

/* ---- 阶段一：按块边界描轮廓 ---- */

/* 生成轮廓表。tile_w/tile_h 为负或为零、大于画布等都会通过返回值
 * TILE_E_ARG 报错，并在 ol->errbuf 中指出是哪一项。 */
int tile_outline_build(TileOutline *ol, int w, int h, int tile_w, int tile_h);
void tile_outline_free(TileOutline *ol);

/* 像素 (x,y) 的块号；越界返回 -1。 */
int tile_block_at(const TileOutline *ol, int x, int y);

/* 导出轮廓表：每行 "x,y,block_id,edge"，edge 为 0/1。
 * path 为 "-" 时写到 stream。返回写出的行数，负值为错误码。 */
int tile_outline_export_csv(const TileOutline *ol, const char *path, FILE *stream);

/* ---- 阶段二：把轮廓叠加到预览 ---- */

/* 把轮廓按 style 描到 rgb（w*h*3，行主序）上。被改写的像素会先备份到
 * 内部备份区，可由 tile_overlay_restore() 还原。
 * 同一轮廓多次 apply 不会重复备份。 */
int tile_overlay_apply(TileOutline *ol, unsigned char *rgb,
                       const TileOverlayStyle *style);

/* 还原 apply 改写过的像素，回到叠加前的当前输出。无备份时为空操作。 */
int tile_overlay_restore(TileOutline *ol);

/* 叠加误差：叠加图与原图之间被改写的像素数（即轮廓命中像素数），
 * 以及这些像素的通道绝对误差之和。apply 之后才有数据。 */
int tile_overlay_diff(const TileOutline *ol, int *changed_px,
                      long *abs_channel_error);

/* ---- 旧路径（对照用）：单遍直接在预览上画网格，不分两阶段 ---- */

/* 直接在 rgb 上按 tile 网格描线，返回描线像素数。无备份、不可还原，
 * 作为与两阶段新路径输出做像素级对照的基线。 */
int tile_grid_legacy(unsigned char *rgb, int w, int h,
                     int tile_w, int tile_h, const unsigned char color[3],
                     char *errbuf, int errbuf_sz);

/* ---- P6 PPM 读写，供 demo/对照使用 ---- */
int tile_ppm_write(const char *path, const unsigned char *rgb, int w, int h);
int tile_ppm_read(const char *path, unsigned char **rgb, int *w, int *h,
                  char *errbuf, int errbuf_sz);

#endif
