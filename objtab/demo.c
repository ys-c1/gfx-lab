#include "objtab.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 演示：4000 个对象，从 64 槽扩到 20000 规模。
 * 依次展示：规模规划、分批迁移、迁移中查询、回滚、增量与整表两条路径对照。 */
int main(void) {
    OtError err;
    OtOptions opt = ot_default_options();
    opt.max_entries = 100000;
    opt.max_slots = 1ull << 20;

    ObjTab *t = NULL;
    if (ot_create(64, opt, &t, &err) != OT_OK) { printf("create: %s\n", ot_strerror(err.code)); return 1; }

    enum { N = 4000 };
    for (uint32_t i = 0; i < N; ++i)
        if (ot_put(t, i * 3u, i, &err) != OT_OK) { printf("put: %s\n", ot_strerror(err.code)); return 1; }

    printf("entries %llu capacity %llu\n",
           (unsigned long long)ot_count(t), (unsigned long long)ot_capacity(t));

    /* 第一段：按新规模算扩容倍数 */
    OtPlan plan;
    if (ot_plan_resize(t, 20000, &plan, &err) != OT_OK) {
        printf("plan: %s\n", ot_strerror(err.code));
        return 1;
    }
    printf("plan: scale %u slots %llu -> %llu growth x%.2f load %.2f migrate %llu\n",
           plan.new_scale, (unsigned long long)plan.old_slots,
           (unsigned long long)plan.new_slots, plan.growth, plan.load_factor,
           (unsigned long long)plan.migrate_entries);

    /* 第二段：增量迁移，每步扫 200 个旧槽位 */
    if (ot_resize_begin(t, &plan, &err) != OT_OK) { printf("begin: %s\n", ot_strerror(err.code)); return 1; }
    int complete = 0, steps = 0, mism = 0;
    while (!complete) {
        uint32_t moved = 0;
        if (ot_migrate_step(t, 200, &moved, &complete, &err) != OT_OK) {
            printf("step: %s\n", ot_strerror(err.code));
            return 1;
        }
        ++steps;
        /* 迁移中查询：新旧表合并可见 */
        uint32_t v = 0;
        uint32_t probe = (uint32_t)(steps * 37) % N;
        if (ot_get(t, probe * 3u, &v, &err) != OT_OK || v != probe) ++mism;
    }
    printf("migrated %llu entries in %d steps, mid-migration query mismatches %d\n",
           (unsigned long long)ot_migrated(t), steps, mism);
    if (ot_resize_finish(t, &err) != OT_OK) { printf("finish: %s\n", ot_strerror(err.code)); return 1; }

    /* 回滚演示：再开一次迁移，迁一半后回滚，布局与迁移前完全一致 */
    uint64_t before = 0, after = 0;
    ot_layout_checksum(t, &before, &err);
    if (ot_plan_resize(t, 50000, &plan, &err) == OT_OK &&
        ot_resize_begin(t, &plan, &err) == OT_OK) {
        int done = 0;
        while (!done && ot_migrated(t) < 500) {
            uint32_t moved = 0;
            ot_migrate_step(t, 50, &moved, &done, &err);
        }
        uint64_t partial = ot_migrated(t); /* 回滚会重置计数器，先记下 */
        ot_resize_rollback(t, &err);
        ot_layout_checksum(t, &after, &err);
        printf("rollback: layout equal %s (回滚前已迁移 %llu 条)\n",
               before == after ? "yes" : "no", (unsigned long long)partial);
    }

    /* 超限报告 */
    OtOptions lim = opt;
    lim.max_entries = 10000;
    ObjTab *lt = NULL;
    if (ot_create(64, lim, &lt, &err) == OT_OK) {
        OtPlan bad;
        int rc = ot_plan_resize(lt, 20000, &bad, &err);
        printf("over-limit rc=%d (%s) limit_entries=%u limit_slots=%llu requested_slots=%llu\n",
               rc, ot_strerror(rc), err.limit_entries,
               (unsigned long long)err.limit_slots, (unsigned long long)err.requested_slots);
        ot_destroy(lt);
    }

    OtStats s;
    ot_stats(t, &s);
    printf("lookups %llu hits %llu misses %llu avg_probes %.3f max_probes %llu\n",
           (unsigned long long)s.lookups, (unsigned long long)s.hits,
           (unsigned long long)s.misses,
           s.lookups ? (double)s.probes / (double)s.lookups : 0.0,
           (unsigned long long)s.max_probes);

    ot_destroy(t);
    return 0;
}
