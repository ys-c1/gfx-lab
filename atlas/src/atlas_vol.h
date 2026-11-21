#ifndef ATLAS_VOL_H
#define ATLAS_VOL_H

#include "atlas.h"

/*
 * 超限分册（experimental, opt-in）——一张图集的显存占用超过单册上限时，把它
 * 拆成若干“册”（volume，每册是一张可独立驻留/上传的图集纹理）。
 *
 * 老路径 atlas_pack() 一行未改。本文件只在 ATLAS_USE_VOL 非零时参与编译；
 * 默认 0，此时 atlas 模块的 test/demo 与引入本文件之前逐字节相同。
 *
 * 两段式，可分别调用或由 avol_pack 一次跑完：
 *   阶段一 avol_split：按每册上限（page_w/page_h 与 cap_bytes）切册，只决定
 *                       每个矩形落在第几册（vol_of[]），不写任何坐标。
 *                       矩形放不下时返回 AVOL_ERR_TOO_BIG 并经 err_item
 *                       报出矩形原始序号。
 *   阶段二 avol_layout：按 vol_of[] 逐册用与 atlas_pack 相同的货架规则回填
 *                       册内坐标，写回索引 cells[] 的 (volume, x, y)。
 *
 * 两段都保持原始矩形序号升序；单册即可放下时，册内坐标与 atlas_pack 完全一致。
 *
 * 倍率 scale 取 1 或 2：坐标与页尺寸始终用 1x 点（point）表示，物理像素与
 * 字节数按 scale 放大；cap_bytes 是每册的硬上限，所以 2x 下每册能装的矩形
 * 更少，册数会上升。
 *
 * 坐标约定：cells[i].x/y 是 1x 点坐标；册 p 的物理页宽高为 page_w*scale、
 * page_h*scale，矩形第 (tx,ty) 个物理像素相对册缓冲的偏移由 avol_texel 给出。
 */

#ifndef ATLAS_USE_VOL
#define ATLAS_USE_VOL 0
#endif

#if ATLAS_USE_VOL

#include <stdio.h>

/* 返回值 */
#define AVOL_OK           0
#define AVOL_ERR_ARG     (-1) /* 空指针 / 非法尺寸 / 非法倍率 */
#define AVOL_ERR_TOO_BIG (-2) /* 矩形大于单册上限；err_item 为矩形序号 */
#define AVOL_ERR_OOB     (-3) /* 越界访问；bad_index 为矩形序号 */
#define AVOL_ERR_NOMEM   (-4) /* 内存分配失败 */

typedef struct {
    int page_w, page_h; /* 每册最大尺寸，1x 点 */
    long cap_bytes;     /* 每册显存上限（物理字节） */
    int bytes_per_px;   /* 每像素字节数，例如 RGB=3、RGBA=4 */
    int scale;          /* 倍率：1 或 2 */
} AvolLimits;

typedef struct {
    int volume; /* 册号 0..volume_count-1 */
    int x, y;   /* 册内 1x 点坐标 */
    int placed; /* 1 = 已回填 */
} AvolCell;

typedef struct {
    int page_w, page_h;
    int rects;       /* 本册矩形数 */
    int used_px;     /* 本册已用面积（1x 点面积） */
    long used_bytes; /* 本册已用物理字节 */
    int shelves;     /* 本册货架数 */
    double occupancy;/* used_px / (page_w*page_h) */
} AvolInfo;

/*
 * 汇总统计。面积类字段用 1x 点，字节类字段按 scale 放大后计。
 *   index_len   = n + volume_count            （索引记录条数）
 *   index_bytes = 16 + 12*n + 20*volume_count （索引序列化字节数）
 *   capacity_px = volume_count * page_w*page_h
 *   samples     = sum(w*h) * scale*scale      （逐像素校验的解析次数）
 *   waste_px    = capacity_px - used_px       （统计误差：册内留空面积）
 *   waste_bytes = waste_px*scale*scale*bytes_per_px
 */
typedef struct {
    int scale;
    int volumes;
    int index_len;
    int index_bytes;
    long samples;
    long used_px;
    long used_bytes;
    long capacity_px;
    long waste_px;
    long waste_bytes;
    double occupancy; /* used_px / capacity_px */
    int oob_index;    /* 打包成功时为 -1 */
} AvolStats;

typedef struct {
    AvolCell *cells; /* n 条，按矩形原始序号回填 */
    AvolInfo *vols;  /* volume_count 条 */
    int volume_count;
    int n;
    AvolLimits lim;  /* 生成本索引时的上限（读回/导出可复现） */
} AvolBook;

/*
 * 阶段一：只切册，不写坐标。
 * vol_of 长度必须 >= n；*volume_count 收到册数。
 * 矩形非法/大于单册/单矩形超过 cap_bytes → AVOL_ERR_TOO_BIG，err_item 为序号。
 * n<=0、空指针、非法上限 → AVOL_ERR_ARG。
 */
int avol_split(const AtlasRect *rects, int n, const AvolLimits *lim,
               int *vol_of, int *volume_count, int *err_item);

/*
 * 阶段二：按 vol_of[] 逐册回填索引与每册统计。
 * book 被完整初始化（旧内容需先 avol_free）；stats 可为 NULL。
 * 册内回填前会按页尺寸复核，越界返回 AVOL_ERR_OOB 且 err_item 给出矩形序号。
 */
int avol_layout(const AtlasRect *rects, int n, const int *vol_of,
                int volume_count, const AvolLimits *lim,
                AvolBook *book, AvolStats *stats, int *err_item);

/*
 * 打包入口（两段串联）。
 *   enable_new != 0：avol_split + avol_layout。
 *   enable_new == 0：老路径，直接调用 atlas_pack(rects, n, page_w)，忽略
 *                    page_h/cap_bytes/scale 对布局的影响；册内坐标与
 *                    atlas_pack 的输出逐字节一致（单册）。此时索引与册表
 *                    的导出文本与“新路径单册”逐字节相同。
 * stats 可为 NULL；err_item 可为 NULL，入口先置 -1。
 */
int avol_pack(const AtlasRect *rects, int n, const AvolLimits *lim,
              AvolBook *book, AvolStats *stats, int *err_item, int enable_new);

void avol_free(AvolBook *book);

/* 按原始序号取索引项；越界返回 AVOL_ERR_OOB 并把 rect_index 写入 *bad_index。 */
int avol_lookup(const AvolBook *book, int rect_index, AvolCell *out, int *bad_index);

/*
 * 取矩形 rect_index 在册内的第 (tx,ty) 个物理像素（相对矩形左上角）。
 * tx/ty 越出矩形、或换算后越出册页时返回 AVOL_ERR_OOB，*bad_index 报矩形序号；
 * 成功时写册号与相对册缓冲的字节偏移。
 */
int avol_texel(const AvolBook *book, const AtlasRect *rects, int rect_index,
               int tx, int ty, int *volume, long *byte_offset, int *bad_index);

/*
 * 全量校验：索引项、册号、册内坐标、每册 used_px/used_bytes 与 cap_bytes。
 * 发现第一处问题时返回 AVOL_ERR_OOB：矩形级问题写矩形序号到 *bad_index（>=0），
 * 册级问题（占用/上限不一致）写 -(册号+1)。
 */
int avol_validate(const AvolBook *book, const AtlasRect *rects, int *bad_index);

/*
 * 采样校验：按索引把每个矩形在 scale 倍率下的每个物理 texel 都解析一遍
 * （tx/ty 走 avol_texel），累计解析次数到 *samples。全部命中时 0；
 * 任一处越界立即返回 AVOL_ERR_OOB，*bad_index 报矩形序号。
 * 成功时 *samples == stats->samples == sum(w*h*scale*scale)。
 */
int avol_probe(const AvolBook *book, const AtlasRect *rects,
               long *samples, int *bad_index);

/* 导出册表（确定性文本，含每册占用与每个矩形的册内坐标）。 */
int avol_export(const AvolBook *book, const AtlasRect *rects, FILE *out);

#endif /* ATLAS_USE_VOL */
#endif /* ATLAS_VOL_H */
