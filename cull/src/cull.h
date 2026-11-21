#ifndef CULL_H
#define CULL_H

typedef struct { double x, y, w, h; int layer; } CullBox;
typedef struct { double x, y, w, h; } CullView;
typedef struct { int *ids; int count; } CullResult;

int cull_run(const CullBox *boxes, int n, CullView view, CullResult *out);
int cull_count_within(const CullBox *boxes, int n, CullView view);

/* ------------------------------------------------------------------ */
/* 分层两段剔除                                                        */
/*                                                                     */
/* 问题：不同层号的对象原先混在一次遍历里剔除，统计与输出无法区分      */
/*       远近层，层与层之间互相干扰。                                   */
/* 方案：第一段只按层号分组（不剔除），第二段对每个组单独、独立地      */
/*       做视口剔除；任何一组的结果都只取决于该组自己的对象和视口，    */
/*       与其他组无关（组间串扰为 0）。                                 */
/*                                                                     */
/* 分组边界（明确约定）：                                              */
/*   - 组按层号升序排列；组内保持输入下标升序。                        */
/*   - 输出 ids 按同样的顺序拼接，第 j 组的可见 id 恰好落在半开区间    */
/*     [groups[j].first, groups[j].first + groups[j].count) 内。       */
/*   - 各组区间首尾相接、从 0 开始、无空洞、无重叠；                    */
/*     groups[group_count-1].first + count == ids 总数。               */
/*   - 某组一个都不可见时该组仍会报告，count = 0，边界是一个空区间。   */
/*                                                                     */
/* 规模上限：不同层号的个数（即分组数）不得超过 CULL_MAX_LAYERS。      */
/*   - 超过时返回 CULL_ERR_LAYER_LIMIT，并在 err 中给出：              */
/*       limit  = 上限值 CULL_MAX_LAYERS                               */
/*       found  = 实际不同层号个数                                     */
/*       layer  = 第 limit+1 个（按层号升序）层号，即越界层号          */
/*       index  = 输入顺序中第一个属于越界层的对象下标                 */
/*       field  = "layer"                                              */
/*   - 恰好等于上限是合法的。                                          */
/*                                                                     */
/* 关闭新分支（layered == 0）：                                        */
/*   - 输出与旧接口 cull_run 逐项一致（可见集合与顺序都不变）；        */
/*   - 不校验层号、不检查上限（保持旧行为）；                          */
/*   - 此时不填充分组信息：groups == NULL，group_count == 0。          */
/*                                                                     */
/* 错误码为负，成功为 CULL_OK(0)；err 可为 NULL。                      */
/* ------------------------------------------------------------------ */

#define CULL_MAX_LAYERS 64

enum {
    CULL_OK = 0,
    CULL_ERR_ARG = -1,        /* 空指针 / n < 0 / 内存不足 / 分组结构非法 */
    CULL_ERR_VIEW = -2,       /* 视口 w 或 h <= 0 */
    CULL_ERR_LAYER = -3,      /* layer < 0；err->index 指出对象，field = "layer" */
    CULL_ERR_LAYER_LIMIT = -4 /* 不同层号数超过 CULL_MAX_LAYERS；见上方说明 */
};

typedef struct {
    int code;          /* CULL_OK 或负错误码 */
    int index;         /* 出错对象在输入中的下标，不适用为 -1 */
    int layer;         /* 相关层号，不适用为 -1 */
    int found;         /* 超限时：实际不同层号个数；其他情况为 0 */
    int limit;         /* 分组数上限值 CULL_MAX_LAYERS */
    const char *field; /* 出错字段名，不适用为 NULL */
} CullError;

/* 第一段输出：按层号分组的结果。 */
typedef struct {
    int layer; /* 层号 */
    int first; /* 本组在 members[] 中的起点 */
    int count; /* 本组对象数（即该组候选数） */
} CullLayerGroup;

typedef struct {
    int n;                    /* 原对象总数 */
    int *members;             /* 对象下标，按 (层号, 输入下标) 升序 */
    CullLayerGroup *groups;   /* 按层号升序 */
    int group_count;
} CullGrouping;

/* 第二段输出：逐组剔除后的可见结果与每组统计。 */
typedef struct {
    int layer;      /* 层号，升序 */
    int first;      /* 本组可见 id 在 ids[] 中的起点；边界 [first, first+count) */
    int count;      /* 本组可见数 */
    int candidates; /* 本组候选数（该层对象总数，含被裁掉的） */
} CullGroup;

typedef struct {
    int *ids;          /* 可见对象下标：组按层号升序，组内按输入下标升序 */
    int count;         /* 可见总数 */
    CullGroup *groups; /* 分组统计，按层号升序 */
    int group_count;   /* 分组数 */
    int layer_limit;   /* 分组数上限值回显 */
} CullLayered;

/* 第一段：只按层号分组，不做剔除、不接触视口。
   成功时 g 由调用者用 cull_grouping_free 释放。 */
int cull_group_by_layer(const CullBox *boxes, int n, CullGrouping *g,
                        CullError *err);
void cull_grouping_free(CullGrouping *g);

/* 第二段：对每个组独立做视口剔除并汇总。
   只读取本组成员的坐标与视口，组与组之间没有任何数据依赖。
   g 非法（边界越界、不连续、层号非升序等）时返回 CULL_ERR_ARG。 */
int cull_run_grouped(const CullBox *boxes, const CullGrouping *g, CullView view,
                     CullLayered *out, CullError *err);

/* 两段合一。layered != 0 走新分支；layered == 0 走旧逻辑，输出与 cull_run 相同。
   成功时 out 由调用者用 cull_layered_free 释放。 */
int cull_run_layered(const CullBox *boxes, int n, CullView view, int layered,
                     CullLayered *out, CullError *err);
void cull_layered_free(CullLayered *out);

#endif
