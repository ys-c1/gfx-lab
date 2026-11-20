#ifndef CULL_INC_H
#define CULL_INC_H

#include "cull.h"

/* 错误码：负值，并通过 err 字段字符串指出出错的具体字段 */
#define CULL_OK           0
#define CULL_E_ARG       -1   /* 空指针 / n 为负 */
#define CULL_E_VIEW_EMPTY -2  /* 视口 w 或 h 非正（字段名见 err） */
#define CULL_E_BOX       -3   /* 盒子 w 或 h 为负（字段名见 err） */
#define CULL_E_NOMEM     -4   /* 内存分配失败 */
#define CULL_E_STATE     -5   /* 状态未初始化 */

/* 视口差异：old 中可能退出的区域(exit)、new 中可能进入的区域(enter)。
 * 两区域按矩形的并集保守近似，保证不漏判；无差异时两区域面积为 0。 */
typedef struct {
    CullView enter;       /* 新视口里、旧视口外的带状区域（并集矩形） */
    CullView exit_;       /* 旧视口里、新视口外的带状区域（并集矩形） */
    int moved;            /* 视口是否发生任何变化 */
} CullDiff;

/* 一次增量更新的统计：更新对象数要对外报告 */
typedef struct {
    long candidates;      /* 差异带内被检查的候选对象数（去重后） */
    long entered;         /* 实际进入视口的对象数 */
    long exited;          /* 实际退出视口的对象数 */
    long updated;         /* 更新对象总数 = entered + exited */
} CullIncStats;

/* 增量剔除状态：保存当前可见集合 + 均匀网格空间索引 */
typedef struct CullInc CullInc;

/* 建索引并做首次全量裁剪，结果与 cull_run 一致。cand/upd 为 0（非增量）。 */
int cull_inc_open(const CullBox *boxes, int n, CullView view,
                  CullInc **out, const char **err);

/* 第一段：比较前后视口，产出进入/退出两个差异矩形 */
int cull_diff(CullView old_view, CullView new_view, CullDiff *out,
              const char **err);

/* 第二段：只对进出边界的候选对象做增量更新；
 * 返回 0 时 out->count/out->ids 为新可见集合（按 id 升序）。 */
int cull_inc_update(CullInc *st, CullView new_view, CullIncStats *stats,
                    const char **err);

/* 无增量（视口无变化）时直接返回当前集合；stats 全 0、集合保持不变 */
int cull_inc_snapshot(const CullInc *st, CullResult *out);

void cull_inc_close(CullInc *st);

#endif
