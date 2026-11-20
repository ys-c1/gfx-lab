#ifndef CULL_H
#define CULL_H

typedef struct { double x, y, w, h; int layer; } CullBox;
typedef struct { double x, y, w, h; } CullView;
typedef struct { int *ids; int count; } CullResult;

/* 暴力遍历 API（对照基准，保留原接口） */
int cull_run(const CullBox *boxes, int n, CullView view, CullResult *out);
int cull_count_within(const CullBox *boxes, int n, CullView view);

/* 错误码 */
enum {
    CULL_OK = 0,
    CULL_ERR_ARG = -1,          /* 空指针 / n 非法等 */
    CULL_ERR_VIEW = -2,         /* 视口 w/h 非法 */
    CULL_ERR_DEGENERATE = -3,   /* 包围盒退化 */
    CULL_ERR_ZERO_LEVELS = -4,  /* 树层数为 0 */
    CULL_ERR_MEMORY = -5        /* 分配失败 */
};

/* 错误详情：index 指出第几个对象，field 指出哪个字段 */
typedef struct {
    int code;
    int index;          /* 对象下标，不适用时为 -1 */
    const char *field;  /* 出错字段名，如 "w"、"h"、"levels"；不适用为 NULL */
    char message[160];
} CullError;

/* BVH 节点：叶子节点 left/right == -1，first/count 指向 tree->order 中的对象区间 */
typedef struct {
    double minx, miny, maxx, maxy;
    int left, right;
    int first, count;
} CullNode;

typedef struct {
    CullNode *nodes;    /* 节点数组，根节点固定为 nodes[0] */
    int *order;         /* 重排后的对象下标，叶子区间索引此数组 */
    int node_count;     /* 总节点数：满二叉树时为 2*leaf_count-1 */
    int leaf_count;
    int object_count;   /* 构建时的对象总数 n */
    int levels;         /* 层数（根为第 1 层） */
} CullBVH;

/* 单次查询统计：访问节点数、候选（叶子内实测）对象数、最终可见数 */
typedef struct {
    int visited_nodes;
    int candidates;
    int visible;
} CullQueryStats;

/* 第一层：由全部对象构建层次包围盒（确定性：中位切分、id 决胜，无随机） */
int cull_bvh_build(const CullBox *boxes, int n, CullBVH *tree, CullError *err);
void cull_bvh_free(CullBVH *tree);

/* 第二层：按视口递归剪枝；输出 id 按升序排列，保证可复现 */
int cull_bvh_query(const CullBVH *tree, const CullBox *boxes, CullView view,
                   CullResult *out, CullQueryStats *stats, CullError *err);

/* 两个（升序）结果集的对称差规模，即可见集合误差元素数；0 表示完全一致 */
int cull_result_diff(const CullResult *a, const CullResult *b);

#endif
