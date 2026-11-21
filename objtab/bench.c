#define _POSIX_C_SOURCE 200809L
/* 基准：两档规模（1 万 / 10 万对象），每档对照两条路径：
 *   full —— 旧的整表重建；
 *   inc  —— 两段式：先按新规模算扩容倍数，再分批增量迁移。
 *
 * 每档报告（题目要求项逐条对应）：
 *   - 扩容倍数、迁移条数、耗时（增量路径额外报分批步数与单步最长迁移数）
 *   - 迁移前后命中：逐键比较「是否存在 + 值」的差异数（应为 0）
 *   - 输出误差：迁移前后导出内容的逐项差异数（应为 0）
 *   - 回滚：迁到一半后回滚，槽位布局与导出内容是否逐字节回到迁移前
 *   - 两条路径最终布局校验和是否一致
 *
 * 同时把槽位快照写到 build/snapshot_<tag>.txt，供 tools/refcheck.py 独立复核。
 */
#include "objtab.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

typedef struct {
    uint32_t n;       /* 条目数 */
    uint32_t scale;   /* 新规模 */
    uint32_t batch;   /* 每次迁移扫描的旧槽位数 */
} Tier;

static OtOptions bench_options(void) {
    OtOptions opt = ot_default_options();
    opt.max_entries = 20000000;
    opt.max_slots = 1ull << 28;
    return opt;
}

/* 建表：key 用 2^32 黄金比例步进保证不重复；value 由 key 决定，
 * 使任意两次建表得到完全相同的内容（两条路径的对照才有意义）。 */
static ObjTab *make_table(uint32_t n, OtPair **pairs, OtOptions opt, OtError *err) {
    ObjTab *t = NULL;
    if (ot_create(OT_MIN_SLOTS, opt, &t, err) != OT_OK) return NULL;
    OtPair *arr = (OtPair *)malloc((size_t)n * sizeof(OtPair));
    if (!arr) { ot_destroy(t); return NULL; }
    for (uint32_t k = 0; k < n; ++k) {
        uint32_t key = (uint32_t)((uint64_t)k * 2654435761u);
        arr[k].key = key;
        arr[k].value = key * 2246822519u + 0x9e3779b9u; /* 确定性的伪随机值 */
        if (ot_put(t, arr[k].key, arr[k].value, err) != OT_OK) {
            free(arr);
            ot_destroy(t);
            return NULL;
        }
    }
    *pairs = arr;
    return t;
}

typedef struct {
    uint64_t hit_diff;             /* 迁移前后逐键命中差异 */
    uint64_t export_diff;          /* 迁移前后导出内容逐项差异 */
    uint64_t rollback_slot_diff;   /* 回滚前后槽位布局差异 */
    uint64_t rollback_export_diff; /* 回滚前后导出内容差异 */
    int      rollback_layout_equal;
    uint64_t rollback_migrated;
    double   rollback_ms;
    double   resize_ms;
    uint64_t final_layout;         /* 迁移完成后的布局校验和 */
} Verify;

static Verify verify_path(uint32_t n, const Tier *tier, int incremental,
                          const char *tag, OtError *err) {
    Verify v;
    memset(&v, 0, sizeof(v));
    OtOptions opt = bench_options();

    OtPair *pairs = NULL;
    ObjTab *t = make_table(n, &pairs, opt, err);
    if (!t) { printf("%s: build failed %s\n", tag, ot_strerror(err->code)); return v; }

    /* 迁移前：逐键命中快照（命中键 + 一批不存在的键，覆盖 miss 分支） */
    uint32_t *pre_val = (uint32_t *)malloc((size_t)n * sizeof(uint32_t));
    uint8_t *pre_hit = (uint8_t *)calloc(n ? n : 1, 1);

    /* 命中键：全部 n 个；缺失键：一个与主键序列不重叠的区间 */
    enum { MISS_N = 4096 };
    uint32_t miss_keys[MISS_N];
    uint8_t miss_hit_before[MISS_N];
    uint32_t miss_val_before[MISS_N];
    for (int i = 0; i < MISS_N; ++i) miss_keys[i] = 0x80000000u + (uint32_t)i * 7919u;
    for (int i = 0; i < MISS_N; ++i) {
        uint32_t val = 0;
        miss_hit_before[i] = (uint8_t)(ot_get(t, miss_keys[i], &val, err) == OT_OK);
        miss_val_before[i] = val;
    }
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t val = 0;
        pre_hit[i] = (uint8_t)(ot_get(t, pairs[i].key, &val, err) == OT_OK);
        pre_val[i] = val;
    }
    OtPair *pre_export = NULL;
    size_t pre_n = 0;
    ot_export(t, &pre_export, &pre_n, err);
    uint64_t pre_cap = ot_capacity(t);

    /* 迁移 */
    double t0 = now_ms();
    OtResizeReport rep;
    int rc = ot_resize(t, tier->scale, incremental, tier->batch, &rep, err);
    double resize_ms = now_ms() - t0;
    if (rc != OT_OK) {
        printf("%s: resize failed %s\n", tag, ot_strerror(rc));
        free(pre_val); free(pre_hit); free(pre_export); free(pairs); ot_destroy(t);
        return v;
    }
    if (incremental && ot_resize_finish(t, err) != OT_OK) {
        printf("%s: finish failed %s\n", tag, ot_strerror(err->code));
        free(pre_val); free(pre_hit); free(pre_export); free(pairs); ot_destroy(t);
        return v;
    }

    /* 迁移后：逐键命中比对 */
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t val = 0;
        int hit = ot_get(t, pairs[i].key, &val, err) == OT_OK;
        if (pre_hit[i] && (!hit || val != pre_val[i])) ++v.hit_diff;
        else if (!pre_hit[i] && hit) ++v.hit_diff;
    }
    for (int i = 0; i < MISS_N; ++i) {
        uint32_t val = 0;
        int hit = ot_get(t, miss_keys[i], &val, err) == OT_OK;
        if ((uint8_t)hit != miss_hit_before[i]) ++v.hit_diff;
        else if (hit && val != miss_val_before[i]) ++v.hit_diff;
    }
    OtPair *post_export = NULL;
    size_t post_n = 0;
    ot_export(t, &post_export, &post_n, err);
    if (post_n != pre_n) {
        v.export_diff = (uint64_t)(post_n > pre_n ? post_n - pre_n : pre_n - post_n);
    } else {
        for (size_t i = 0; i < pre_n; ++i)
            if (pre_export[i].key != post_export[i].key ||
                pre_export[i].value != post_export[i].value) ++v.export_diff;
    }
    ot_layout_checksum(t, &v.final_layout, err);
    v.resize_ms = resize_ms;

    /* 槽位快照，交给参考工具复核 */
    OtSlot *slots = NULL;
    uint64_t cap = 0;
    if (ot_layout_snapshot(t, &slots, &cap, err) == OT_OK) {
        char path[256];
        snprintf(path, sizeof(path), "build/snapshot_%s.txt", tag);
        FILE *f = fopen(path, "w");
        if (f) {
            fprintf(f, "# tag %s slots %llu entries %zu\n", tag,
                    (unsigned long long)cap, post_n);
            for (uint64_t i = 0; i < cap; ++i) {
                if (slots[i].state == 1)
                    fprintf(f, "S %llu %u %u\n", (unsigned long long)i,
                            slots[i].key, slots[i].value);
                else
                    fprintf(f, "E %llu %u\n", (unsigned long long)i,
                            (unsigned)slots[i].state);
            }
            fclose(f);
        }
        free(slots);
    }

    printf("%-9s inc=%d slots %llu->%llu growth x%.2f migrated %llu "
           "steps %llu max_step %llu time_ms %.3f\n",
           tag, incremental, (unsigned long long)pre_cap,
           (unsigned long long)rep.plan.new_slots, rep.plan.growth,
           (unsigned long long)rep.migrated, (unsigned long long)rep.steps,
           (unsigned long long)rep.max_step_moved, resize_ms);

    /* ---- 回滚段：同内容新表，迁到一半回滚 ---- */
    {
        OtPair *pairs2 = NULL;
        ObjTab *t2 = make_table(n, &pairs2, opt, err);
        if (t2) {
            uint64_t l0 = 0;
            ot_layout_checksum(t2, &l0, err);
            OtSlot *s0 = NULL; uint64_t c0 = 0;
            ot_layout_snapshot(t2, &s0, &c0, err);
            OtPair *e0 = NULL; size_t e0n = 0;
            ot_export(t2, &e0, &e0n, err);

            double r0 = now_ms();
            OtPlan plan;
            if (ot_plan_resize(t2, tier->scale, &plan, err) == OT_OK &&
                ot_resize_begin(t2, &plan, err) == OT_OK) {
                int done = 0;
                uint64_t halfway = (uint64_t)n / 2;
                while (!done && ot_migrated(t2) < halfway) {
                    uint32_t moved = 0;
                    ot_migrate_step(t2, tier->batch ? tier->batch : 1024,
                                    &moved, &done, err);
                }
                v.rollback_migrated = ot_migrated(t2);
                if (ot_resize_rollback(t2, err) != OT_OK)
                    printf("%s: rollback failed %s\n", tag, ot_strerror(err->code));
            }
            v.rollback_ms = now_ms() - r0;

            uint64_t l1 = 0;
            ot_layout_checksum(t2, &l1, err);
            v.rollback_layout_equal = (l0 == l1);
            OtSlot *s1 = NULL; uint64_t c1 = 0;
            ot_layout_snapshot(t2, &s1, &c1, err);
            if (c0 != c1) {
                v.rollback_slot_diff = (uint64_t)(c0 > c1 ? c0 - c1 : c1 - c0);
            } else {
                for (uint64_t i = 0; i < c0; ++i)
                    if (s0[i].state != s1[i].state ||
                        (s0[i].state == 1 && (s0[i].key != s1[i].key ||
                                              s0[i].value != s1[i].value)))
                        ++v.rollback_slot_diff;
            }
            OtPair *e1 = NULL; size_t e1n = 0;
            ot_export(t2, &e1, &e1n, err);
            if (e1n != e0n) {
                v.rollback_export_diff = (uint64_t)(e1n > e0n ? e1n - e0n : e0n - e1n);
            } else {
                for (size_t i = 0; i < e0n; ++i)
                    if (e0[i].key != e1[i].key || e0[i].value != e1[i].value)
                        ++v.rollback_export_diff;
            }
            free(s0); free(s1); free(e0); free(e1); free(pairs2);
            ot_destroy(t2);
        }
    }

    printf("%-9s hit_diff %llu export_diff %llu | rollback layout_equal %s "
           "slot_diff %llu export_diff %llu migrated %llu time_ms %.3f\n",
           "", (unsigned long long)v.hit_diff, (unsigned long long)v.export_diff,
           v.rollback_layout_equal ? "yes" : "no",
           (unsigned long long)v.rollback_slot_diff,
           (unsigned long long)v.rollback_export_diff,
           (unsigned long long)v.rollback_migrated, v.rollback_ms);

    free(pre_val); free(pre_hit); free(pre_export); free(post_export); free(pairs);
    ot_destroy(t);
    return v;
}

int main(void) {
    /* 两档规模：1 万 / 10 万对象；新规模取当前条目数的 4 倍。
     * 164/1149 槽位一档的倍数差异也便于核对扩容倍数公式。 */
    Tier tiers[2] = {
        {10000, 40000, 512},
        {100000, 400000, 512},
    };

    OtError err;
    printf("=== 规模档基准：整表重建 vs 两段增量迁移 ===\n");
    for (int i = 0; i < 2; ++i) {
        const Tier *tr = &tiers[i];
        printf("\n-- 第 %d 档: 对象 %u, 新规模 %u, 每步扫描 %u 槽 --\n",
               i + 1, tr->n, tr->scale, tr->batch);
        char tag_full[64], tag_inc[64];
        snprintf(tag_full, sizeof(tag_full), "full_%u", tr->n);
        snprintf(tag_inc, sizeof(tag_inc), "inc_%u", tr->n);
        Verify vf = verify_path(tr->n, tr, 0, tag_full, &err);
        Verify vi = verify_path(tr->n, tr, 1, tag_inc, &err);
        printf("%-9s full vs inc 最终布局校验和一致: %s\n", "",
               vf.final_layout == vi.final_layout ? "yes" : "no");
        printf("%-9s 耗时 total: full %.3f ms, inc %.3f ms"
               "（分段把一次大停顿拆成多步，整表那次 max_step=%u 条一次做完）\n",
               "", vf.resize_ms, vi.resize_ms, tr->n);
    }

    printf("\n输出快照写入 build/snapshot_<tag>.txt，由 tools/refcheck.py 独立复核。\n");
    return 0;
}
