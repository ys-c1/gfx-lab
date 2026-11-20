#ifndef APACK_H
#define APACK_H
#include "asset.h"

/* 两阶段布局:
 *   阶段一 ap_pack_plan  按块大小与对齐约束求排列,输出每块偏移与总跨度;
 *   阶段二 ap_pack_apply 把偏移回填进索引(ApEntry.offset)。
 * optimize=0 时为现状顺序布局(逐字节一致);optimize=1 时为紧凑布局。
 */

typedef struct {
    unsigned int size;  /* 块字节数,必须 > 0 */
    unsigned int align; /* 对齐值,必须为非零 2 的幂 */
} ApBlock;

typedef struct {
    int index;         /* 校验失败的块下标,无错误时为 -1 */
    const char *field; /* 出错字段:"align" 或 "size" */
} ApPackError;

#define AP_PACK_OK        0
#define AP_PACK_ERR_ARG  -1 /* 空指针或 n < 0 */
#define AP_PACK_ERR_ALIGN -2 /* 对齐值非法(为 0 或不是 2 的幂) */
#define AP_PACK_ERR_SIZE -3 /* 块大小为 0 */
#define AP_PACK_ERR_OFLOW -4 /* 布局超出 32 位偏移范围 */
#define AP_PACK_ERR_NOMEM -5 /* 内存分配失败 */

#define AP_PACK_GREEDY 0 /* 顺序贪心(现状) */
#define AP_PACK_TIGHT  1 /* 紧凑:对齐降序 + 空隙回收 */

int ap_pack_plan(const ApBlock *blocks, int n, int optimize,
                 unsigned int *offsets, unsigned int *total,
                 ApPackError *err);
void ap_pack_apply(ApEntry *entries, const unsigned int *offsets, int n);

#endif
