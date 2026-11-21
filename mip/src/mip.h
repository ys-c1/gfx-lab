#ifndef MIP_H
#define MIP_H
#include <stddef.h>

/*
 * 盒式降采样的 mip 层链（每像素 3 通道，每通道 1 字节）。
 *
 * 两段式构建，避免“一建就吃满内存”：
 *   1) mip_plan()  只做算术：按图像尺寸与可用内存预算 max_bytes，
 *                  算出理论层数、预算允许的层数上限以及每层字节数，不做任何分配。
 *   2) mip_build() 按 plan 给出的上限，从第 0 层起逐层 malloc、逐层降采样，
 *                  任何一层分配失败立即停止并返回，已分配层由 mip_free 回收。
 *
 * max_bytes == 0 表示不设预算（恢复旧版行为：构建完整层链直到 1x1）。
 * 预算不足时静默降级为更少的层数，chain.degraded 置 1，cap_levels 记录
 * 预算允许的上限；完整结果（无降级）与旧版本逐字节一致。
 */
typedef struct {
    int levels;          /* 实际生成的层数（>=1） */
    int cap_levels;      /* 预算允许的层数上限；无预算时等于 levels */
    int degraded;        /* 1 表示层数被预算或分配失败截断，0 表示完整链 */
    int w0, h0;
    unsigned char **pix;
    int *w, *h;
} MipChain;

typedef struct {
    int full_levels;     /* 不考虑预算时的理论层数 */
    int cap_levels;      /* 预算允许的层数上限（>=1，<=full_levels） */
    int degraded;        /* cap_levels < full_levels 时为 1 */
    size_t total_bytes;  /* 按 cap_levels 构建时，像素数据的总字节数（不含元数据） */
    size_t level_bytes[64]; /* 每层像素字节数；超过 64 层的尺寸在 mip_plan 中判为非法 */
} MipPlan;

/* 错误码：0 成功；-1 空指针/内存分配失败；-2 非法输入。
 * err_idx 非空时，写入第一个非法参数的索引（c=0, src=1, w=2, h=3,
 * max_bytes=4, plan=5），成功时写 -1。 */
int mip_plan(int w, int h, size_t max_bytes, MipPlan *plan, int *err_idx);
int mip_build(MipChain *c, const unsigned char *src, int w, int h, size_t max_bytes);
void mip_free(MipChain *c);
int mip_sample(const MipChain *c, int level, double u, double v, unsigned char out[3]);
#endif
