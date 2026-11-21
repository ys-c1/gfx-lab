#ifndef OBJTAB_H
#define OBJTAB_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 对象表：开放寻址哈希表（线性探测，容量 2 的幂）。
 *
 * 扩容分两段：
 *   1. ot_plan_resize()  按新规模算出新容量与扩容倍数，超限时返回 OT_E_LIMIT
 *                        并在 OtError 里给出上限值；
 *   2. ot_resize_begin() / ot_migrate_step() / ot_resize_finish()
 *                        把旧表的条目分批搬进新表，每步工作量有上界。
 *
 * 迁移期间旧表保持只读（一行都不改），因此：
 *   - ot_get() 先查新表、再查旧表，迁移前后命中逐项一致；
 *   - ot_resize_rollback() 直接丢弃新表、恢复旧表，回滚误差严格为 0；
 *   - 迁移期间的写入（ot_put/ot_del）返回 OT_E_MIGRATING，不产生半迁移状态。
 *
 * incremental=0 时走旧的整表重建路径（ot_resize_full）；两条路径的
 * 查找结果与迭代输出完全相同，可直接对照。
 */

#define OT_MIN_SLOTS 16ull
#define OT_DEFAULT_LOAD 0.70
#define OT_DEFAULT_MAX_ENTRIES 10000000u
#define OT_DEFAULT_MAX_SLOTS (1ull << 26)

enum {
    OT_OK = 0,
    OT_E_ARG = -1,       /* 空指针 / 参数非法 */
    OT_E_NOMEM = -2,     /* 分配失败 */
    OT_E_FULL = -3,      /* 表已满，无法插入 */
    OT_E_LIMIT = -4,     /* 超过配置上限；OtError 给出上限值 */
    OT_E_MIGRATING = -5, /* 迁移期间不允许写 */
    OT_E_STATE = -6,     /* 不在迁移中 / 迁移尚未完成 / 计划已失效 */
    OT_E_NOTFOUND = -7,  /* 键不存在 */
    OT_E_RANGE = -8,     /* 新规模小于当前条目数等 */
    OT_E_STALE_PLAN = -9 /* 计划生成后表已变化，需重新规划 */
};

typedef struct {
    int code;                  /* OT_OK 或负错误码 */
    uint64_t limit_slots;      /* code==OT_E_LIMIT 时的槽位上限 */
    uint64_t requested_slots;  /* 计划需要的槽位数 */
    uint32_t limit_entries;    /* 条目数上限 */
    uint32_t requested_entries;/* 计划的条目数 */
} OtError;

typedef struct {
    uint32_t key;
    uint32_t value;
} OtPair;

typedef struct {
    uint64_t old_slots;      /* 当前槽位数 */
    uint64_t new_slots;      /* 按新规模算出的槽位数（2 的幂） */
    uint32_t new_scale;      /* 新规模：期望容纳的对象数 */
    double load_factor;      /* 规划使用的负载因子 */
    double growth;           /* 扩容倍数 = new_slots / old_slots */
    uint64_t migrate_entries;/* 需要迁移的条目数 */
    uint64_t table_gen;      /* 规划时的表版本；表内容变化后计划失效 */
} OtPlan;

typedef struct {
    int incremental;         /* 1=两段增量迁移；0=整表重建 */
    uint32_t max_entries;    /* 条目数上限；0 用默认值 */
    uint64_t max_slots;      /* 槽位数硬上限；0 用默认值 */
    double load_factor;      /* (0,1)；<=0 用默认值 */
} OtOptions;

typedef struct {
    uint64_t lookups;
    uint64_t hits;
    uint64_t misses;
    uint64_t probes;         /* 累计探测槽位数 */
    uint64_t max_probes;     /* 单次查找最长探测 */
} OtStats;

typedef struct {
    OtPlan plan;
    int incremental;
    uint64_t migrated;       /* 实际迁移条数 */
    uint64_t steps;          /* 分批次数 */
    uint64_t max_step_moved; /* 单步最多迁移条数 */
} OtResizeReport;

typedef struct ObjTab ObjTab;

OtOptions ot_default_options(void);

/* 创建表。initial_slots 会向上取整到 2 的幂且不小于 OT_MIN_SLOTS。 */
int ot_create(uint64_t initial_slots, OtOptions opt, ObjTab **out, OtError *err);
void ot_destroy(ObjTab *t);

/* 基本读写。新键插入时若超过负载因子会走旧的整表重建（阻塞）。
 * 迁移期间返回 OT_E_MIGRATING。 */
int ot_put(ObjTab *t, uint32_t key, uint32_t value, OtError *err);
int ot_get(ObjTab *t, uint32_t key, uint32_t *value, OtError *err);
int ot_del(ObjTab *t, uint32_t key, OtError *err);

/* 第一段：按新规模算容量与扩容倍数。new_scale 必须 >= 当前条目数。
 * 超限返回 OT_E_LIMIT，err 中带 limit_slots / limit_entries。 */
int ot_plan_resize(const ObjTab *t, uint32_t new_scale, OtPlan *plan, OtError *err);

/* 第二段：开始迁移。校验 plan 与当前表一致，分配新表，旧表保持不动。 */
int ot_resize_begin(ObjTab *t, const OtPlan *plan, OtError *err);

/* 再次迁移：budget 为本步最多扫描的旧槽位数（>=1），moved 返回本步
 * 实际迁移条数，complete 返回旧表是否已全部扫描。 */
int ot_migrate_step(ObjTab *t, uint32_t budget, uint32_t *moved, int *complete,
                    OtError *err);

/* 反复调用 ot_migrate_step 直到旧表扫描完；不释放旧表，需再调 finish
 * 或 rollback。batch 为每步扫描槽位数，0 用 1024。 */
int ot_migrate_all(ObjTab *t, uint32_t batch, OtResizeReport *report, OtError *err);

/* 迁移数据完成后释放旧表。未完成或不在迁移中返回 OT_E_STATE。 */
int ot_resize_finish(ObjTab *t, OtError *err);

/* 放弃新表、恢复旧表。旧表迁移期间未改动，回滚后内容逐项与迁移前相同。 */
int ot_resize_rollback(ObjTab *t, OtError *err);

/* 旧路径：一次性整表重建到 new_scale。report 可为 NULL。 */
int ot_resize_full(ObjTab *t, uint32_t new_scale, OtResizeReport *report, OtError *err);

/* 便捷入口：incremental!=0 走两段增量迁移，否则走整表重建。 */
int ot_resize(ObjTab *t, uint32_t new_scale, int incremental, uint32_t batch,
              OtResizeReport *report, OtError *err);

/* 导出逻辑内容（按 key 升序）。迁移中也能导出，每条逻辑条目恰好出现一次。
 * 调用者用 free() 释放 *out。 */
int ot_export(const ObjTab *t, OtPair **out, size_t *n, OtError *err);

/* 当前表槽位布局校验和（迁移完成后可用于比较增量/整表两条路径）。 */
int ot_layout_checksum(const ObjTab *t, uint64_t *out, OtError *err);

/* 槽位快照：按槽位下标导出 state/key/value，供参考工具独立复核。
 * *out 由 malloc 分配，槽位数写入 *cap，调用者 free()。 */
typedef struct {
    uint8_t state;   /* 0 空 / 1 占用 / 2 墓碑 */
    uint32_t key;
    uint32_t value;
} OtSlot;
int ot_layout_snapshot(const ObjTab *t, OtSlot **out, uint64_t *cap, OtError *err);

uint64_t ot_capacity(const ObjTab *t);
uint64_t ot_count(const ObjTab *t);
int ot_migrating(const ObjTab *t);
uint64_t ot_old_capacity(const ObjTab *t);
uint64_t ot_migrated(const ObjTab *t);

void ot_stats(const ObjTab *t, OtStats *out);
void ot_stats_reset(ObjTab *t);

const char *ot_strerror(int code);

#ifdef __cplusplus
}
#endif

#endif
