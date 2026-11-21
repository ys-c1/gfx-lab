#ifndef CULL_H
#define CULL_H
typedef struct { double x, y, w, h; int layer; } CullBox;
typedef struct { double x, y, w, h; } CullView;
typedef struct { int *ids; int count; } CullResult;
int cull_run(const CullBox *boxes, int n, CullView view, CullResult *out);
int cull_count_within(const CullBox *boxes, int n, CullView view);

/* ------------------------------------------------------------------ */
/* 空间哈希两段式剔除                                                  */
/*                                                                    */
/* 第一段 cull_hash_build：把每个包围盒插入它覆盖到的所有网格桶，      */
/*                         桶内按对象下标升序保存，构建结果可复现；    */
/* 第二段 cull_hash_query：只遍历视口覆盖到的桶，桶内对象去重后逐个   */
/*                         做包围盒重叠测试，输出按下标升序。          */
/*                                                                    */
/* 只要一个盒与视口重叠，它覆盖的某个桶就必然被视口覆盖，因此两段式   */
/* 的可见集合与逐个比较的 cull_run 完全相同（无漏剔、无误剔）。       */
/* cull_run_hashed 是两段合一的便捷入口。                             */
/* opts.legacy != 0 时完全绕过哈希，直接调用 cull_run，输出逐项照旧。 */
/* ------------------------------------------------------------------ */

/* 默认上限：网格桶总数、对象-桶插入总数。 */
#define CULL_HASH_MAX_BUCKETS 4096
#define CULL_HASH_MAX_ENTRIES 4000000L

enum {
    CULL_OK = 0,
    CULL_ERR_ARG = -1,        /* 空指针 / n < 0 / 参数非法 / 分配失败 */
    CULL_ERR_VIEW = -2,       /* 视口 w/h 非正或非有限 */
    CULL_ERR_HASH_LIMIT = -3, /* 桶数或插入数超过上限；err->limit 给出上限值 */
    CULL_ERR_HASH_BOX = -4,   /* 包围盒坐标非有限或 w/h 为负；err->index 指出下标 */
    CULL_ERR_HASH_NOMEM = -5, /* 内存分配失败 */
    CULL_ERR_HASH_IO = -6     /* 桶表导出写文件失败 */
};

typedef struct {
    int code;          /* CULL_OK 或以上负值 */
    int index;         /* 出错对象下标，无则 -1 */
    long limit;        /* CULL_ERR_HASH_LIMIT 时的上限值，其余为 0 */
    const char *field; /* 出错字段名（"buckets" / "entries" / "x" ...），无则 NULL */
    char message[192]; /* 带具体数值的说明，含上限值 */
} CullHashError;

typedef struct {
    double cell_w, cell_h; /* >0 指定网格单元尺寸；0 或负数按对象数自动选择 */
    int max_buckets;       /* 0 -> CULL_HASH_MAX_BUCKETS */
    long max_entries;      /* 0 -> CULL_HASH_MAX_ENTRIES */
    int legacy;            /* 非 0：关闭哈希新路径，输出与 cull_run 逐项相同 */
} CullHashOpts;

/* 一次构建（以及每次查询复制一份）的统计。
 * collisions：同一桶内除第一个之外的插入次数，即 sum_bucket(max(0, count-1))；
 * collision_rate：collisions / entries * 100，百分比，输出保留两位小数。 */
typedef struct {
    int buckets;         /* 网格桶总数 gw*gh */
    int occupied;        /* 非空桶数 */
    long entries;        /* 对象-桶插入总数 */
    long collisions;     /* 冲突插入数 */
    double collision_rate; /* 冲突率（百分比） */
} CullHashStats;

typedef struct {
    int *ids;   /* 可见对象下标，升序 */
    int count;
    int legacy; /* 1 表示由 legacy 路径产生（未建哈希） */
    long candidates;     /* 本次查询实际做重叠测试的对象数（去重后） */
    int visited_buckets; /* 本次查询访问的桶数 */
    CullHashStats stats;
} CullHashResult;

typedef struct CullHash CullHash;

/* 第一段：校验入参、选网格、分桶。失败返回负值并填充 err（可为 NULL）。
 * 成功时 *out 需由 cull_hash_free 释放。boxes 必须比索引活得久。 */
int cull_hash_build(const CullBox *boxes, int n, const CullHashOpts *opts,
                    CullHash **out, CullHashError *err);

/* 第二段：查询一个视口，只测试视口覆盖桶内的对象。
 * 内部复用去重暂存区（桶访问标记 + 候选数组），因此同一索引上的查询
 * 不是线程安全的；可见集合按对象下标升序输出，与 cull_run 逐项相同。 */
int cull_hash_query(CullHash *h, CullView view, CullHashResult *out,
                    CullHashError *err);

/* 两段合一；opts.legacy != 0 时退化为 cull_run。 */
int cull_run_hashed(const CullBox *boxes, int n, CullView view,
                    const CullHashOpts *opts, CullHashResult *out,
                    CullHashError *err);

/* 读取索引自身的统计（不查询）。 */
int cull_hash_get_stats(const CullHash *h, CullHashStats *out);

/* 导出桶表（仅非空桶，按 (bucket_y, bucket_x) 升序；桶内按对象下标升序）。
 * 格式为可复现的纯文本，文件包含 grid/origin/cell/统计行与 bucket 行。 */
int cull_hash_export_buckets(const CullHash *h, const char *path);

void cull_hash_free(CullHash *h);
void cull_hash_result_free(CullHashResult *r);
#endif
