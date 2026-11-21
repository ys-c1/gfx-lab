#ifndef ATLAS_H
#define ATLAS_H
typedef struct { int w, h; } AtlasRect;
typedef struct { int x, y; int placed; } AtlasPos;
typedef struct { int width; int used_px; int shelves; } AtlasStats;

/* 旧路径：按输入顺序逐块货架打包，行为保持不变。 */
int atlas_pack(const AtlasRect *rects, int n, int width, AtlasPos *out, AtlasStats *stats);

/* 两段式打包开关：sort=1 时先按面积从大到小重排，再逐块存放；
 * sort=0 时与 atlas_pack 输出完全一致。 */
#define ATLAS_SORT_OFF 0
#define ATLAS_SORT_AREA_DESC 1

/* sort     : ATLAS_SORT_OFF 或 ATLAS_SORT_AREA_DESC
 * moved    : 可空；返回因重排而改变打包次序的块数（sort=0 时为 0）
 * err_index: 可空；返回 -2 时置为出问题的那一项的下标（如 w/h 为负或超宽）
 * 返回值   : 0 成功；-1 参数整体非法；-2 某一项尺寸非法（见 err_index）；
 *           -3 sort 取值未知；-4 内存不足
 */
int atlas_pack2(const AtlasRect *rects, int n, int width, int sort,
                AtlasPos *out, AtlasStats *stats, int *moved, int *err_index);
#endif
