#ifndef TILE_H
#define TILE_H

/*
 * tile —— 拼图边缘填充模块
 *
 * 背景：整张大图被切成若干图块（tile）。老逻辑只能给所有图块指定同一种
 * 边框色，逐块采样边缘时，相邻块接缝处会露出底色（露底）。
 *
 * 新逻辑分两段：
 *   1) tile_read_colors：按块读取各自的边框色（对每个块的边缘像素做采样，
 *      采样次数与倍率挂钩），得到每块独立的色表；
 *   2) tile_fill_edges：按每个块自己的边框色填充其外缘，接缝两侧颜色一致，
 *      不再露底。
 *
 * 兼容：fill 时 enable_new 为 0 则完全走老路径（单一边框色、老采样规则），
 * 输出字节与引入新逻辑之前保持一致。
 */

#define TILE_NEG_BORDER   (-3)  /* 边缘宽度越界（负值） */
#define TILE_EINVAL       (-1)  /* 空指针/非法尺寸/非法参数 */
#define TILE_ERANGE       (-2)  /* 网格越界访问 */

typedef struct {
    int tw, th;               /* 每个图块的尺寸（像素） */
    int cols, rows;           /* 图块网格：列数、行数 */
    int border;               /* 需要填充的外缘宽度（像素） */
} TileGrid;

typedef struct {
    unsigned char rgb[3];     /* 该块的边框色（RGB，每通道 0..255） */
    int samples;              /* 读取该色时的采样次数 */
} TileColor;

typedef struct {
    int zoom;                 /* 倍率：1 = 1x，2 = 2x */
    long samples;             /* 采样总次数：read 段累计所有块边缘采样点 */
    long fill_pixels;         /* 实际填充的边缘像素总数：fill 段累计 */
    double stat_mae;          /* 统计误差：各块采样色相对全局平均色的 MAE */
    double border_mae;        /* 边框误差：边缘采样点相对本块均值色的 MAE */
    int oob_block;            /* 检出的越界块编号（>=0），无越界为 -1 */
} TileStats;

/* 网格尺寸/指针合法性校验，不访问任何像素。合法返回 0。 */
int tile_check_grid(const TileGrid *g);

/*
 * 越界访问检测：检查把 cols*rows 个块铺进画布时，每个块在 zoom 倍率下的
 * 像素包围盒是否落在 canvas_w*canvas_h 之内。检出越界时把第一个越界块的
 * 编号写入 *bad_block（块号 = row*cols + col，行优先）并返回 TILE_ERANGE。
 */
int tile_check_bounds(const TileGrid *g, int zoom, int canvas_w, int canvas_h,
                      int *bad_block);

/*
 * 第一段：按块读边框色。
 * src 为整块拼接源图（cols*tw × rows*th，RGB，行优先，3 字节/像素）。
 * out 数组长度必须 >= cols*rows；每个元素得到该块的边框色与采样次数。
 * zoom 取 1 或 2：2x 在每条边的每个采样位置上额外插入一个中点采样，
 * 采样次数约为 1x 的两倍。
 * stats 可为 NULL；非 NULL 时写入 zoom、samples、stat_mae、border_mae，
 * 并把 oob_block 置 -1（本段不做画布边界检查）。
 */
int tile_read_colors(const TileGrid *g, const unsigned char *src, int zoom,
                     TileColor *out, TileStats *stats);

/*
 * 第二段 + 老路径：把边缘宽度 border 的外缘填充为边框色。
 * dst 为画布缓冲（canvas_w*canvas_h*3 字节），块按 zoom 倍率铺放，
 * 块 (c,r) 的左上角像素为 (c*tw*zoom, r*th*zoom)。
 *
 * enable_new != 0：新路径，逐块使用 colors[] 里该块自己的边框色。
 * enable_new == 0：老路径，所有块统一使用 legacy_rgb，且填充走与旧版本
 *                  完全相同的循环顺序与写法，保证输出字节不变。
 * 访问前逐块做边界检查，越界即停并在 stats->oob_block 报出块号。
 * stats 可为 NULL；非 NULL 时在函数入口被完整初始化，本函数只负责
 * zoom/fill_pixels/oob_block，其余字段置 0。read 段的 samples/误差等请
 * 从 tile_read_colors 自己的 stats 读取，两段用各自独立的 TileStats。
 */
int tile_fill_edges(const TileGrid *g, const TileColor *colors,
                    const unsigned char legacy_rgb[3], int zoom,
                    int canvas_w, int canvas_h, unsigned char *dst,
                    int enable_new, TileStats *stats);

/*
 * 导出各块色表为 CSV（块号,列,行,R,G,B,采样次数），用于和纸笔推导的
 * 数值逐块对照。返回 0 成功。
 */
int tile_export_colors(const TileGrid *g, const TileColor *colors,
                       const char *path);

#endif
