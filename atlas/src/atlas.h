#ifndef ATLAS_H
#define ATLAS_H
typedef struct { int w, h; } AtlasRect;
typedef struct { int x, y; int placed; } AtlasPos;
typedef struct { int width; int used_px; int shelves; } AtlasStats;
int atlas_pack(const AtlasRect *rects, int n, int width, AtlasPos *out, AtlasStats *stats);

/* ---- 多图集合并 -------------------------------------------------------
 * 两段式：先 atlas_merge_layout 算新偏移与全局布局，
 * 再 atlas_rewrite_indices 依据偏移表改写旧索引。
 * do_merge=0 时不合并：各源图集独立打包、布局照旧、索引恒等改写。 */

typedef struct {
    const AtlasRect *blocks;   /* 该图集中的块 */
    int nblocks;               /* 块数 */
    int base;                  /* 输出：该图集在合并序列中的起始槽位偏移 */
} AtlasSource;

typedef struct {
    int total_blocks;          /* 所有源图集块数之和 */
    int used_px;               /* 被块占用的像素总数 */
    int total_height;          /* 货架总高（占用带高度） */
    int shelves;               /* 货架总数 */
    int rewritten;             /* 成功改写条数 */
    int errors;                /* 越界/无效条数 */
} AtlasMergeStats;

typedef struct {
    int width;                 /* 目标画布宽度（128 / 512 ...） */
    int do_merge;              /* 1=合并布局，0=布局照旧 */
    AtlasSource *sources;      /* 输入：源图集；base 由阶段一回填 */
    int nsrc;
    AtlasPos *positions;       /* 输出：全局块位置，容量 total_blocks */
    AtlasMergeStats stats;     /* 输出：度量 */
} AtlasMerge;

/* 旧索引表中的一条引用：来自哪个源图集(src)、该图集内的局部块号(old_idx)。
 * 阶段二回填 new_idx（合并后的全局槽位）与 valid（0=越界无效）。 */
typedef struct {
    int src;
    int old_idx;
    int new_idx;
    int valid;
} AtlasIndexEntry;

/* 返回 0 成功；负值为错误，*err_at 为出错条目/源图集序号（-1 表示不适用）。
 *  -1 非法参数   -2 块尺寸非法或超出宽度（err_at=全局块序号）
 *  -3 内存不足   -4 偏移越界（err_at=条目序号）
 *  -5 索引冲突（err_at=后到的条目序号，err_slot=被重复占用的全局槽位） */
int atlas_merge_layout(AtlasMerge *m, int *err_at);
int atlas_rewrite_indices(AtlasMerge *m, AtlasIndexEntry *entries, int nent,
                          int *err_at, int *err_slot);

/* 导出偏移表与索引表（纯文本，每行一条，首行为表头）。返回 0 成功，-6 写文件失败。 */
int atlas_export_offsets(const AtlasMerge *m, const char *path);
int atlas_export_indices(const AtlasIndexEntry *entries, int nent, const char *path);
#endif
