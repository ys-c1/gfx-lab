#ifndef CULL_H
#define CULL_H
typedef struct { double x, y, w, h; int layer; } CullBox;
typedef struct { double x, y, w, h; } CullView;
typedef struct { int *ids; int count; } CullResult;
int cull_run(const CullBox *boxes, int n, CullView view, CullResult *out);
int cull_count_within(const CullBox *boxes, int n, CullView view);

/* ------------------------------------------------------------------ */
/* 绘制候选合并：同一对象号在候选里可能出现多次，每条候选都会变成一次
 * 绘制调用。这里分两段处理：
 *   阶段 1  cull_merge_ids       按对象号合并（去重，结果按对象号升序）
 *   阶段 2  cull_order_original  按候选中的原顺序（首次出现）输出
 * 合并开启时绘制调用数 = 唯一对象数；关闭时输出与候选逐项相同（画面照旧）。
 *
 * 序号约定：err_index 为 0 基下标，打印给用户时再 +1；空列表无序号，
 * err_index = -1。
 * 错误：
 *   CULL_E_EMPTY     列表为空（候选列表或对象表），err_index = -1
 *   CULL_E_DUP_ID    对象号重复（对象表/映射表），err_index = 重复项在表中的下标
 *   CULL_E_MAP_MISS  映射缺项，err_index = 该候选的下标
 * ------------------------------------------------------------------ */
#define CULL_OK          0
#define CULL_E_ARG      -1   /* 空指针 / n<0 / 内存不足 */
#define CULL_E_EMPTY    -3   /* 列表为空 */
#define CULL_E_DUP_ID   -4   /* 对象号重复 */
#define CULL_E_MAP_MISS -5   /* 映射缺项 */

/* 场景对象表：id 为对象号，必须在表内唯一。 */
typedef struct { int id; CullBox box; } CullObject;

/* 对象号 -> 对象表下标 的映射。ids 升序，at[i] 为 ids[i] 在对象表中的下标；
 * hkey/hval/hcap 为内部开放寻址哈希，查找 O(1)，清空请用 cull_map_free。 */
typedef struct {
    int *ids; int *at; int n;
    int *hkey; int *hval; unsigned hcap;
} CullMap;

/* 构建映射；对象号重复时返回 CULL_E_DUP_ID 并给出重复项的表内下标。 */
int cull_map_build(CullMap *m, const CullObject *objs, int n, int *err_index);
void cull_map_free(CullMap *m);
/* 查对象表下标；缺项返回 -1。 */
int cull_map_find(const CullMap *m, int id);

/* 阶段 1：按对象号合并。uniq 至少容纳 n 项，输出升序唯一对象号。 */
int cull_merge_ids(const int *cand, int n, int *uniq, int *uniq_count, int *err_index);

/* 阶段 2：按候选中的原顺序输出。uniq 必须是阶段 1 的输出（严格升序），
 * out_ids/out_first 至少容纳 uniq_count 项；out_first 可传 NULL。
 * out_first[i] 为 out_ids[i] 在候选中首次出现的下标。 */
int cull_order_original(const int *cand, int n, const int *uniq, int uniq_count,
                        int *out_ids, int *out_first, int *err_index);

/* 绘制列表：count 即绘制调用数。 */
typedef struct { int *ids; int *first; int count; } CullDrawList;

/* 完整流程。merge != 0 走两阶段合并；merge == 0 原样输出候选（旧行为）。
 * map 用于校验候选的映射缺项；无论 merge 开关都会校验。 */
int cull_draw_list(const int *cand, int ncand, const CullMap *map, int merge,
                   CullDrawList *out, int *err_index);
void cull_draw_list_free(CullDrawList *d);

#endif
