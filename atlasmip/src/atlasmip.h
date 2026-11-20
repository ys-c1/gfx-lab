#ifndef ATLASMIP_H
#define ATLASMIP_H

/*
 * 块感知的图集 mipmap 管线，解决相邻块颜色在降采样层互相渗入的问题。
 *
 * 管线分三段：
 *   1. 贴边扩展：块外像素按块边界做 clamp-to-edge 填充（am_build 的 extend 开关）；
 *   2. 每块单独生成层链：盒式降采样只在块内取像素，越界索引钳制到块边，
 *      等价于给每块套一圈边缘拉伸的 halo 再滤波，邻块数据永不参与；
 *   3. 取样按块裁剪：UV 被钳制到目标块当前层级的像素矩形内，越界 UV 只取块边，
 *      绝不会落到邻居块上。
 *
 * 渗色度量：另建一条朴素全图链（与 mip 模块相同的整图盒式金字塔）作对照。
 * 全图链第 L 层的每个像素，按其基底 footprint 中心找到所属块，与该块自己的
 * 第 L 层链值逐通道比较；不一致即记为一个渗色像素。
 */

typedef struct { int x, y, w, h; } AmRect;

/* 块边界索引：am_build 内部拷贝一份，也可独立构建并序列化到文件。 */
typedef struct {
    int w, h;
    int n;
    AmRect *rects;
} AmIndex;

/* 单块层链：levels 个层级，尺寸在 1 处钳制并复制末级。 */
typedef struct {
    int levels;
    int *w, *h;
    unsigned char **pix;
} AmBlockChain;

typedef struct {
    int pixels;       /* 参与比较的像素数（全图链该层总像素数） */
    int bleed_px;     /* 渗色像素数：任一字段通道与块链值不一致 */
    long err_sum;     /* 三通道绝对误差之和 */
    int err_max;      /* 单通道最大绝对误差 */
} AmReport;

typedef struct {
    AmIndex idx;                   /* 块边界索引（本结构持有，am_free 释放） */
    int levels;                    /* 每条块链的层数 */
    AmBlockChain *chains;          /* n 条独立块层链 */
    unsigned char *extended;       /* 阶段1产物：扩展后的基底（w*h*3） */
    int extend;                    /* 构建时是否做了贴边扩展 */
    /* 朴素全图链，渗色对照基准 */
    int naive_levels;
    int *naive_w, *naive_h;
    unsigned char **naive_pix;
    /* 各阶段耗时（纳秒，CLOCK_MONOTONIC） */
    long ns_extend, ns_chains, ns_naive;
} AtlasMip;

/* 返回码：0 成功；-1 空指针/非法尺寸/块数非法；-2 levels<=0；-3 块越界；-4 IO 错误 */

/* 构建块边界索引。越界时返回 -3 且 *bad_block 写入越界块序号（可为 NULL）。 */
int am_index_build(const AmRect *rects, int n, int w, int h,
                   AmIndex *out, int *bad_block);
int am_index_write(const char *path, const AmIndex *idx);
int am_index_read(const char *path, AmIndex *idx);
void am_index_free(AmIndex *idx);

/*
 * 构建整套块感知金字塔。
 *   extend != 0：阶段1对块外像素做贴边扩展；extend == 0：基底保持输入原样。
 *   levels 必须 > 0，否则返回 -2。
 *   任一块越界返回 -3 且 *bad_block 给出序号（可为 NULL）。
 */
int am_build(AtlasMip *am, const unsigned char *src,
             const AmRect *rects, int n, int w, int h,
             int levels, int extend, int *bad_block);

/*
 * 块内取样。u/v 超出 [0,1] 时裁剪到块边（绝不读到邻块）。
 * 块序号非法返回 -3，level 非法返回 -2。
 */
int am_sample(const AtlasMip *am, int block, double u, double v,
              int level, unsigned char out[3]);

/* 在指定层度量渗色，结果写入 rep。level 非法返回 -2。 */
int am_measure(const AtlasMip *am, int level, AmReport *rep);

void am_free(AtlasMip *am);
#endif
