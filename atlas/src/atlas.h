#ifndef ATLAS_H
#define ATLAS_H

typedef struct { int w, h; } AtlasRect;
typedef struct { int x, y; int placed; } AtlasPos;
typedef struct { int width; int used_px; int shelves; } AtlasStats;

/* 旧路径：一次性货架打包。行为与输出保持不变，作为新路径的对照基准。 */
int atlas_pack(const AtlasRect *rects, int n, int width, AtlasPos *out, AtlasStats *stats);

/* 错误码。出错时 err_item 指出是哪一项（从 0 计），无项信息时为 -1。 */
enum {
    ATLAS_OK    =  0,
    ATLAS_EARG  = -1, /* 非法参数（含负的尺寸/容量） */
    ATLAS_ERECT = -2, /* 某一项尺寸非法（含负值）或超出图集 */
    ATLAS_EFULL = -3  /* 图集已满且回收已关闭（不再崩溃） */
};

/* 同 atlas_pack，但出错时通过 err_item 报告是哪一项（可为 NULL）。 */
int atlas_pack_ex(const AtlasRect *rects, int n, int width, AtlasPos *out,
                  AtlasStats *stats, int *err_item);

/* 新路径：固定容量图集 + 两段式 LRU 回收。
 * 第一段：按最近使用时间（last_used 逻辑时钟）标记存活块；
 * 第二段：回收最久未用的块，并把存活块重排为连续索引（0..n-1）。 */
typedef struct {
    AtlasRect rect;
    AtlasPos pos;
    unsigned long last_used; /* 最近使用时间（逻辑时钟） */
    int id;                  /* 稳定索引号：回收日志与报告中使用 */
    int live;
} AtlasBlock;

typedef struct {
    int width, height;    /* 图集容量（像素） */
    int recycling;        /* 0=关：输出与旧路径一致，满则 ATLAS_EFULL；1=开 */
    int cap;              /* 槽位上限 */
    int n;                /* 存活块数，槽位恒为 0..n-1（回收后已重排） */
    int shelves;
    int used_px;          /* 存活块像素总数 */
    unsigned long tick;   /* LRU 逻辑时钟 */
    int next_id;
    AtlasBlock *blocks;
    int *recycle_log;     /* 被回收块的索引号，按回收顺序排列（可复现） */
    long recycle_log_n, recycle_log_cap;
    int cur_x, cur_y, shelf_h; /* 货架游标 */
} Atlas;

int  atlas_open(Atlas *a, int width, int height, int cap, int recycling);
void atlas_close(Atlas *a);
/* 放入一块，返回槽位号(>=0)；失败返回负错误码，err_item 指出是哪一项。 */
int  atlas_add(Atlas *a, AtlasRect r, int *err_item);
/* 批量放入；某项失败时 err_item 指出是哪一项。 */
int  atlas_add_many(Atlas *a, const AtlasRect *rects, int n, int *err_item);
/* 标记最近使用（刷新 last_used）。 */
int  atlas_touch(Atlas *a, int slot);
/* 占用率 = used_px / (width*height)。 */
double atlas_occupancy(const Atlas *a);
/* 回收索引号序列（按回收先后），*count 为回收块数。 */
const int *atlas_recycled(const Atlas *a, long *count);

#endif
