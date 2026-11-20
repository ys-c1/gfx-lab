#ifndef GRID_H
#define GRID_H

/* 均匀网格宽相碰撞检测：轴对齐包围盒候选对查询，支持增量增删。 */

typedef struct { double x, y, w, h; } GdBox;
typedef struct { int a, b; } GdPair;

typedef struct {
    GdPair *pairs;
    int count;
} GdPairList;

typedef struct {
    double cell_w;          /* 格宽（>0）；<=0 时由 density 推导 */
    double density;         /* 期望每格对象数（>=0）；0 用内部默认值 */
    double minx, miny, maxx, maxy; /* 世界范围 */
} GdConfig;

typedef struct {
    long long cell_hits;    /* 本次查询实际访问（触碰）的格数，去重后 */
    long long candidates;   /* 本次查询执行的包围盒两两测试次数 */
    int rebuilds;           /* 累计重建次数 */
    int updates;            /* 累计增量更新（增/删）次数 */
    int dimx, dimy;         /* 网格维度 */
} GdStats;

typedef struct GdGrid GdGrid;

/* 按密度选格宽：把推导结果写回 cfg->cell_w 与 cfg->density。
 * 返回 0 成功；负错误码见实现，错误字段可用 grid_error_field 取名字。 */
int grid_pick_cell(GdConfig *cfg, const GdBox *boxes, int n);

/* 建立/重建网格。返回 0 成功，负值错误。 */
int grid_build(GdGrid **out, const GdBox *boxes, int n, GdConfig cfg);
void grid_free(GdGrid *g);

/* 增量增删：只重写受影响格，更新计数 +1。 */
int grid_insert(GdGrid *g, int id, GdBox box);
int grid_remove(GdGrid *g, int id);

/* 宽相查询：输出去重候选对，统计写入 *stats（可为 NULL）。 */
int grid_pairs(GdGrid *g, GdPairList *out, GdStats *stats);
int grid_stats(const GdGrid *g, GdStats *out);

/* 格表本身占用的字节数（单元表 + 格内条目存储，不含对象包围盒副本）。 */
long long grid_table_bytes(const GdGrid *g);

/* 对照组：暴力遍历与层次包围盒（BVH），candidates_out 可为 NULL。 */
int grid_brute_pairs(const GdBox *boxes, int n, GdPairList *out, long long *candidates_out);
int grid_bvh_pairs(const GdBox *boxes, int n, GdPairList *out, long long *tests_out);
long long grid_bvh_bytes(int n);

/* 两个候选对集合是否完全一致（与顺序无关）。 */
int grid_pairs_equal(const GdPairList *a, const GdPairList *b);
void grid_pairs_free(GdPairList *l);

/* 错误码对应的出错字段名（静态字符串），无错误返回 NULL。 */
const char *grid_error_field(int err);
#endif
