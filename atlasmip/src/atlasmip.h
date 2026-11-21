#ifndef ATLASMIP_H
#define ATLASMIP_H

/*
 * atlasmip: 图集中"每个块自带独立层链"的两段式方案。
 *
 * 图集里的块不能带自己的缩略层：对整张图集做盒式金字塔时，层 1+
 * 的 2x2 降采样核会跨过块边界，把相邻块的像素混进来（串色）。
 *
 * 两段式：
 *   1) am_build 按块从整图中拷贝块像素，逐块构建独立 MipChain；
 *   2) am_sample 按块号 + 块内归一化坐标 (u,v) 在对应层链上取样。
 *
 * use_block_chains == 0 时完全走老路径（整图层链 + 全局坐标），
 * 降采样/取整实现直接复用 mip 模块，输出与老逻辑字节一致。
 */

/* 越界/非法访问错误码，均为负数，调用方可经 am_last_oob() 取回编号。 */
#define AM_OK             0
#define AM_ERR_NULL      -1   /* 空指针等参数错误            */
#define AM_ERR_PARAM     -2   /* n/atlas 宽高非法            */
#define AM_ERR_BLOCK     -3   /* 块号越界（编号经 oob 报出） */
#define AM_ERR_LEVEL     -4   /* 层号越界（编号经 oob 报出） */
#define AM_ERR_UV        -5   /* 块内坐标 u/v 越界（编号经 oob 报出） */
#define AM_ERR_BLOCK_RECT -6  /* 构建期块矩形越界（编号经 oob 报出） */
#define AM_ERR_NOMEM     -7
#define AM_ERR_STATE     -8   /* 对象未构建/已释放           */

/* 两档倍率档（1x = 原始层，2x 减半 = 每维 1/2 的缩略层）。 */
typedef enum { AM_SCALE_1X = 0, AM_SCALE_2X = 1, AM_SCALE_N = 2 } AmScale;

typedef struct {
    int x, y;          /* 块在图集中的左上角（像素） */
    int w, h;          /* 块尺寸（像素）             */
} AmBlock;

typedef struct {
    int block;         /* 越界访问的块号；非块类错误时为层号或 -1 */
    int level;         /* 越界访问的层号；不适用时为 -1           */
    int code;          /* 对应的 AM_ERR_* 码                      */
} AmOob;

typedef struct {
    long samples;                 /* 该档采样次数                 */
    unsigned long abs_err_sum;    /* 新路径相对老路径的逐通道绝对误差和 */
    int max_err;                  /* 单通道最大绝对误差           */
} AmScaleStat;

typedef struct AtlasMip AtlasMip;

/*
 * 构建。src 为整图 RGB（w*h*3 字节），blocks/n 为块布局。
 * use_block_chains 为 0 时只建整图老链，为 1 时另建每块独立链。
 * 构建期发现块矩形越界返回 AM_ERR_BLOCK_RECT，并把部分对象写入 *out，
 * 可用 am_last_oob 取坏块编号后 am_free。
 */
int am_build(AtlasMip **out, const unsigned char *src, int w, int h,
             const AmBlock *blocks, int n, int use_block_chains);
void am_free(AtlasMip *am);

/*
 * 取样。
 * 新路径：按 block 的独立层链、块内 u/v 取样；
 * 老路径：block 映射到全局坐标后在整图层链上取样（与 mip_sample 字节一致）。
 * 任何越界返回负数错误码并记录编号（am_last_oob），不做钳制。
 */
int am_sample(AtlasMip *am, int block, int level, double u, double v,
              unsigned char out[3]);

/*
 * 按倍率档取样：1x -> level 0，2x -> level 1（链不足 2 层时钳到末层）。
 * 新路径下同时累计该档采样次数与相对老路径的误差统计。
 */
int am_sample_scaled(AtlasMip *am, int block, AmScale scale, double u, double v,
                     unsigned char out[3]);

/* 查询：某块某层的宽高与该块层数、该块层链单独内存（字节）。 */
int am_level_info(const AtlasMip *am, int block, int level, int *lw, int *lh);
int am_level_count(const AtlasMip *am, int block);
long am_chain_bytes(const AtlasMip *am, int block);

/* 统计：所有块层链内存合计（不含整图老链），整图老链内存，块数。 */
long am_total_chain_bytes(const AtlasMip *am);
long am_full_chain_bytes(const AtlasMip *am);
int am_block_count(const AtlasMip *am);
int am_uses_block_chains(const AtlasMip *am);
const AmScaleStat *am_scale_stats(const AtlasMip *am, AmScale scale);

/* 最近一次越界访问的编号信息；无则返回 NULL。 */
const AmOob *am_last_oob(const AtlasMip *am);
#endif
