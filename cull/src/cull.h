#ifndef CULL_H
#define CULL_H

/* seq: 插入序号, 分层模式下全局唯一, 用于桶内稳定排序 */
typedef struct { double x, y, w, h; int layer; int seq; } CullBox;
typedef struct { double x, y, w, h; } CullView;
typedef struct { int *ids; int count; } CullResult;

int cull_run(const CullBox *boxes, int n, CullView view, CullResult *out);
int cull_count_within(const CullBox *boxes, int n, CullView view);

/* ---- 分层稳定输出 ---- */

#define CULL_MAX_BUCKETS 64 /* 不同层号(桶)数上限 */

enum {
    CULL_OK = 0,
    CULL_ERR_NULL = -1,            /* 参数为空 / n < 0 / 分配失败 */
    CULL_ERR_VIEW = -2,            /* 视口非法 */
    CULL_ERR_LAYER_NEGATIVE = -3,  /* layer 字段为负 */
    CULL_ERR_SEQ_DUP = -4,         /* seq 字段重复 */
    CULL_ERR_BUCKETS_OVERFLOW = -5 /* 不同 layer 数超过 CULL_MAX_BUCKETS */
};

typedef struct {
    int code;          /* CULL_OK 或以上负值 */
    int index;         /* 出错元素在 boxes 中的下标, 无则 -1 */
    const char *field; /* 出错字段名 ("layer" / "seq"), 无则 NULL */
} CullStatus;

typedef struct {
    int layer;   /* 桶层号, 输出按层号升序 */
    int visible; /* 桶内可见数 */
    int clipped; /* 桶内被裁数 */
} CullBucket;

typedef struct {
    int *ids;   /* 可见下标: layered!=0 时按 (layer, seq) 升序稳定排列;
                   layered==0 时按插入序, 与 cull_run 完全一致 */
    int count;
    CullBucket *buckets; /* 每桶可见/被裁统计, 按层号升序 */
    int bucket_count;
    unsigned long long comparisons; /* 本次运行内部排序的比较次数 */
} CullLayered;

/* 两阶段: 先按层号分桶(校验 layer/seq/桶数), 再桶内按 seq 稳定输出并统计每桶。
   layered==0 时不重排, 可见集合与顺序和 cull_run 相同。
   注意: 比较计数使用内部全局计数器, 本接口非线程安全。 */
int cull_run_layered(const CullBox *boxes, int n, CullView view,
                     int layered, CullLayered *out, CullStatus *st);
void cull_layered_free(CullLayered *out);

#endif
