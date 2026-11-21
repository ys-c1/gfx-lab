#ifndef REINDEX_H
#define REINDEX_H

/*
 * reindex: 图片/纹理槽位重排后的引用修复。
 *
 * 背景：索引轮换一轮后，外部保存的旧索引会指向错误的图片。
 * 修复严格分两阶段：
 *   阶段一 rx_build  : 按 旧索引 -> 新索引 建映射表；
 *   阶段二 rx_rewrite: 按映射表批量改写引用数组。
 *
 * 负数参数一律硬报错，RxErr 指出是哪个参数、第几项。
 */

#define RX_OK         0
#define RX_EINVAL    -1   /* 空指针 / 负数参数 */
#define RX_ERANGE    -2   /* 索引越界 */
#define RX_ECONFLICT -3   /* 预留：冲突类错误 */
#define RX_EIO       -4   /* 导出文件写入失败 */

typedef struct {
    int  n;            /* 映射长度：覆盖的旧槽位数 */
    int *old_to_new;   /* 长度 n；-1 表示该旧槽位因冲突未建立映射 */
    int  conflicts;    /* 建表时发现的新槽位重复认领冲突数 */
} RxTable;

typedef struct {
    int         code;   /* RX_E* 错误码 */
    int         item;   /* 出错的是第几项；标量参数错误为 -1 */
    const char *param;  /* 出错参数名 */
} RxErr;

/*
 * 阶段一：由置换 perm 建旧->新映射表。
 * perm[i] = 旧索引 i 的图片现在所在的新槽位。
 *  - perm[i] < 0  : 硬报错 RX_EINVAL，err 标出 item=i, param="perm"
 *  - perm[i] >= n : 硬报错 RX_ERANGE，err 标出 item=i
 *  - 多个旧槽位认领同一新槽位：第一个生效，其余计入 conflicts 且不建映射
 * 成功返回 RX_OK，冲突不阻断建表，数量记录在 out->conflicts。
 */
int rx_build(const int *perm, int n, RxTable *out, RxErr *err);

/*
 * 阶段二：按表就地批量改写 refs[0..m-1]。
 *  - refs[i] < 0      : 硬报错 RX_EINVAL，err 标出 item=i, param="refs"
 *  - refs[i] >= n 或命中冲突槽位：计入 *conflicts，原值保留
 * 返回改写条数（>=0），失败返回负错误码。
 */
int rx_rewrite(const RxTable *table, int *refs, int m,
               int *conflicts, RxErr *err);

/*
 * 一站式流程，带开关：
 *  enabled=0 时完全不碰 refs（也不建表），输出与旧路径逐字节一致；
 *  enabled=1 时执行 建表 + 批量改写。
 * table 可为 NULL（内部自建自释放）；rewritten/conflicts 回填统计。
 */
int rx_apply(const int *perm, int n, int *refs, int m, int enabled,
             RxTable *table, int *rewritten, int *conflicts, RxErr *err);

/* 导出映射表为 CSV：头部注释 + "old,new" 数据行（冲突槽位不导出）。 */
int rx_export(const RxTable *table, const char *path);

void rx_free(RxTable *table);
const char *rx_strerror(int code);
#endif
