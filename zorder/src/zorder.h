#ifndef ZORDER_H
#define ZORDER_H

/* 图块重排策略 */
#define ZO_MORTON 0   /* 按莫顿码（Z 序）重排：让二维相邻图块在一维内存里尽量相邻 */
#define ZO_AREA   1   /* 按面积从大到小排序（对照用，面积相同按原序号） */
#define ZO_NONE   2   /* 不打乱顺序，布局保持原样 */

typedef struct {
    int w, h;        /* 图块网格尺寸（均须为 2 的幂） */
    int n;           /* 图块总数 w*h */
    int *order;      /* 阶段一：newpos -> 原图块序号（重排后的存放顺序） */
    int *remap;      /* 阶段二：原图块序号 -> 新位置（回填索引） */
    int jumps;       /* 全图按新顺序遍历时的跳转次数（相邻两块在原内存中不相邻） */
    int err_seq;     /* 出错序号：参数类错误为 0，否则为首个出错图块序号 */
} ZoLayout;

/* 莫顿编码/解码（每维最多 16 bit） */
unsigned int zo_morton_encode(unsigned int x, unsigned int y);
void zo_morton_decode(unsigned int code, unsigned int *x, unsigned int *y);

/*
 * 用外部给定的莫顿码位重排（codes[i] 为图块 i 的码位，按码位升序存放）。
 * 用于校验码位合法性：码位越界返回 -4、重复返回 -3，err_seq 指出首个出错序号。
 */
int zo_reorder_codes(int w, int h, const unsigned int *codes, ZoLayout *lay);

/*
 * 阶段一：按策略重排图块顺序，分配并填好 lay->order，并计算全图跳转次数。
 * 仅写入 order 与 jumps；remap 留空，交给 zo_backfill 回填。
 * 返回 0 成功；负数错误码：
 *   -1 参数非法（空指针/尺寸非正），err_seq = 0
 *   -2 尺寸不是 2 的幂（err_seq = 0）
 *   -3 出现重复图块序号，err_seq = 首个重复序号
 *   -4 出现越界图块序号，err_seq = 首个越界序号
 *   -5 内存分配失败
 */
int zo_reorder(int w, int h, const long *areas, int strategy, ZoLayout *lay);

/*
 * 阶段二：依据 order 回填 remap 索引（原图块序号 -> 新位置）。
 * 返回 0 成功；-1 参数非法。
 */
int zo_backfill(ZoLayout *lay);

/* 便捷封装：一次完成重排 + 回填。错误码同 zo_reorder。 */
int zo_layout(int w, int h, const long *areas, int strategy, ZoLayout *lay);

void zo_free(ZoLayout *lay);

/*
 * 缩图（缩略图）局部性统计：在网格上以 stride 为步长滑动 bw×bh 的采样窗口，
 * 模拟缩图时每个窗口读取 bw*bh 个图块，统计（对所有窗口取平均）：
 *   jumps_win   窗口内按新顺序访问的跳转次数
 *   span_ratio  平均地址跨度比 = 窗口图块在新内存中的跨度(max-min+1) / 理想紧凑跨度(=图块数)，
 *               1.0 表示窗口图块在内存里完全紧挨，越大越分散
 *   occupancy   平均缓存占用率 = 窗口触及的缓存行数 / 全阵列缓存总行数
 *               （cache_line 个连续图块占一行；越小表示一个窗口占用的缓存比例越低、命中越好）
 * 返回 0 成功；负数为参数非法。
 */
int zo_window_stats(const ZoLayout *lay, int bw, int bh, int stride, int cache_line,
                    double *jumps_win, double *span_ratio, double *occupancy);
#endif
