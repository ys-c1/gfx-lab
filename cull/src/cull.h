#ifndef CULL_H
#define CULL_H
typedef struct { double x, y, w, h; int layer; } CullBox;
typedef struct { double x, y, w, h; } CullView;
typedef struct { int *ids; int count; } CullResult;
int cull_run(const CullBox *boxes, int n, CullView view, CullResult *out);
int cull_count_within(const CullBox *boxes, int n, CullView view);

/* 权重表：对象 id -> 权重。同层对象按权重决定绘制先后（小权重先画）。 */
typedef struct { int id; double weight; } CullWeight;
typedef struct { const CullWeight *entries; int count; } CullWeightTable;

/* 校验权重表。返回 0 正常；
 * -3 权重为负，*bad_id 指明出错对象；
 * -4 同一对象重复登记且权重冲突，*bad_id 指明出错对象。 */
int cull_weights_validate(const CullWeightTable *t, int *bad_id);

/* 查权重；未登记的对象权重为 0。 */
double cull_weight_of(const CullWeightTable *t, int id);

/* 两段式排序输出：先按 (layer, weight) 稳定排序，再按序写入 out。
 * sort 为 0 时跳过排序，输出顺序与输入 ids 完全一致。
 * ids 为 boxes 的下标。cmp_count 非空时回写排序比较次数；
 * 出错时 bad_id 非空则指明出错对象。 */
int cull_order(const CullBox *boxes, const int *ids, int n,
               const CullWeightTable *t, int sort, int *out,
               long *cmp_count, int *bad_id);

/* 导出权重表到文件，每行 "id weight"。 */
int cull_weights_export(const CullWeightTable *t, const char *path);
#endif
