#include "objtab.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

/* 简易确定性伪随机 */
static uint64_t rng = 0x243f6a8885a308d3ull;
static uint32_t rnd32(void) {
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return (uint32_t)(rng >> 16);
}

/* 用参考模型（线性数组）核对整表内容 */
typedef struct { uint32_t key, value; int alive; } Ref;
static Ref *ref_new(uint32_t n) { return (Ref *)calloc(n, sizeof(Ref)); }

static int export_matches_model(ObjTab *t, const Ref *ref, uint32_t n, const char *tag) {
    OtPair *arr = NULL;
    size_t m = 0;
    OtError e;
    int rc = ot_export(t, &arr, &m, &e);
    if (rc != OT_OK) { printf("FAIL export %s rc=%d\n", tag, rc); return 0; }
    size_t expect = 0;
    for (uint32_t i = 0; i < n; ++i) if (ref[i].alive) ++expect;
    int ok = (m == expect);
    if (!ok) printf("FAIL export %s count %zu != %zu\n", tag, m, expect);
    size_t k = 0;
    for (uint32_t i = 0; i < n && ok; ++i) {
        if (!ref[i].alive) continue;
        if (arr[k].key != i || arr[k].value != ref[i].value) {
            printf("FAIL export %s item %zu key=%u/%u val=%u/%u\n", tag, k,
                   arr[k].key, i, arr[k].value, ref[i].value);
            ok = 0;
        }
        ++k;
    }
    free(arr);
    return ok;
}

int main(void) {
    OtError e;
    OtOptions opt = ot_default_options();

    /* ---- 基础读写 ---- */
    {
        ObjTab *t = NULL;
        CK(ot_create(0, opt, &t, &e) == OT_OK);
        CK(ot_capacity(t) >= OT_MIN_SLOTS && (ot_capacity(t) & (ot_capacity(t) - 1)) == 0);
        CK(ot_put(t, 10, 100, &e) == OT_OK);
        uint32_t v = 0;
        CK(ot_get(t, 10, &v, &e) == OT_OK && v == 100);
        CK(ot_put(t, 10, 101, &e) == OT_OK);
        CK(ot_get(t, 10, &v, &e) == OT_OK && v == 101);
        CK(ot_count(t) == 1);
        CK(ot_get(t, 11, &v, &e) == OT_E_NOTFOUND);
        CK(ot_del(t, 10, &e) == OT_OK);
        CK(ot_get(t, 10, &v, &e) == OT_E_NOTFOUND);
        CK(ot_del(t, 10, &e) == OT_E_NOTFOUND);
        CK(ot_count(t) == 0);
        ot_destroy(t);
    }

    /* ---- 负载因子触发整表重建仍正确 ---- */
    {
        enum { N = 5000 };
        ObjTab *t = NULL;
        CK(ot_create(OT_MIN_SLOTS, opt, &t, &e) == OT_OK);
        for (uint32_t i = 0; i < N; ++i) CK(ot_put(t, i * 7u, i, &e) == OT_OK);
        CK(ot_count(t) == N);
        int bad = 0;
        for (uint32_t i = 0; i < N; ++i) {
            uint32_t v = 0;
            if (ot_get(t, i * 7u, &v, &e) != OT_OK || v != i) ++bad;
        }
        CK(bad == 0);
        ot_destroy(t);
    }

    /* ---- 规模规划：扩容倍数 ---- */
    {
        ObjTab *t = NULL;
        CK(ot_create(64, opt, &t, &e) == OT_OK);
        for (uint32_t i = 0; i < 40; ++i) CK(ot_put(t, i, i, &e) == OT_OK);
        OtPlan p;
        CK(ot_plan_resize(t, 1000, &p, &e) == OT_OK);
        CK(p.old_slots == 64);
        CK(p.new_slots >= 1000 && (p.new_slots & (p.new_slots - 1)) == 0);
        CK(p.new_slots == 2048); /* ceil(1000/0.7)=1429 -> 2048 */
        CK(p.growth > 31.9 && p.growth < 32.1);
        CK(p.migrate_entries == 40);
        CK(ot_plan_resize(t, 39, &p, &e) == OT_E_RANGE); /* 低于条目数 */
        ot_destroy(t);
    }

    /* ---- 超限报错并给出上限值 ---- */
    {
        OtOptions o2 = ot_default_options();
        o2.max_entries = 1000;
        o2.max_slots = 2048;
        ObjTab *t = NULL;
        CK(ot_create(64, o2, &t, &e) == OT_OK);
        OtPlan p;
        CK(ot_plan_resize(t, 5000, &p, &e) == OT_E_LIMIT);
        CK(e.code == OT_E_LIMIT);
        CK(e.limit_entries == 1000);
        CK(e.limit_slots == 2048);
        CK(e.requested_entries == 5000);
        CK(e.requested_slots == 8192);
        /* 规划失败后 plan 未被填充，不能用它开迁移 */
        OtPlan zero;
        memset(&zero, 0, sizeof(zero));
        CK(ot_resize_begin(t, &zero, &e) == OT_E_STALE_PLAN);
        /* 恰好压线：1000 条目 -> ceil(1000/.7)=1429 -> 2048 槽，允许 */
        CK(ot_plan_resize(t, 1000, &p, &e) == OT_OK && p.new_slots == 2048);
        /* 创建时超限也要报错 */
        ObjTab *t2 = NULL;
        CK(ot_create(4096, o2, &t2, &e) == OT_E_LIMIT && e.limit_slots == 2048);
        CK(t2 == NULL);
        ot_destroy(t);
    }

    /* ---- 两段增量迁移：迁移前后命中逐项一致 ---- */
    {
        enum { N = 3000 };
        ObjTab *t = NULL;
        OtOptions o3 = ot_default_options();
        o3.max_entries = 100000;
        o3.max_slots = 1ull << 20;
        CK(ot_create(64, o3, &t, &e) == OT_OK);

        Ref *ref = ref_new(N);
        for (uint32_t i = 0; i < N; ++i) {
            ref[i].key = i; ref[i].value = rnd32(); ref[i].alive = 1;
            CK(ot_put(t, i, ref[i].value, &e) == OT_OK);
        }
        CK(ot_count(t) == N);

        uint64_t pre_sum = 0, pre_layout = 0;
        for (uint32_t i = 0; i < N; ++i) {
            uint32_t v = 0;
            CK(ot_get(t, i, &v, &e) == OT_OK && v == ref[i].value);
            pre_sum += v;
        }
        CK(ot_layout_checksum(t, &pre_layout, &e) == OT_OK);

        OtPlan plan;
        CK(ot_plan_resize(t, 20000, &plan, &e) == OT_OK);
        CK(plan.growth > 1.0);
        CK(ot_resize_begin(t, &plan, &e) == OT_OK);
        CK(ot_migrating(t) == 1);
        CK(ot_count(t) == N); /* 迁移中逻辑条目数不变 */

        /* 迁移过程中逐步检查命中一致性 */
        uint64_t post_sum = 0;
        int mid_bad = 0, exported_bad = 0;
        int steps = 0;
        int complete = 0;
        while (!complete) {
            uint32_t moved = 0;
            CK(ot_migrate_step(t, 37, &moved, &complete, &e) == OT_OK);
            ++steps;
            /* 每步后随机抽 50 个键与参考模型比对 */
            for (int q = 0; q < 50; ++q) {
                uint32_t k = rnd32() % N, v = 0;
                if (ot_get(t, k, &v, &e) != OT_OK || v != ref[k].value) ++mid_bad;
            }
            if (steps % 7 == 0) {
                OtPair *arr = NULL; size_t m = 0;
                if (ot_export(t, &arr, &m, &e) != OT_OK || m != N) ++exported_bad;
                else {
                    for (size_t k = 0; k < m; ++k)
                        if (arr[k].key != k || arr[k].value != ref[k].value) { ++exported_bad; break; }
                }
                free(arr);
            }
            /* 迁移中禁止写入 */
            if (ot_put(t, 999999, 1, &e) != OT_E_MIGRATING) ++mid_bad;
            if (ot_del(t, 5, &e) != OT_E_MIGRATING) ++mid_bad;
        }
        CK(mid_bad == 0);
        CK(exported_bad == 0);
        CK(steps > 1);
        CK(ot_migrated(t) == N);

        /* 未 finish 前不能再 finish 都要正确；先测 finish 成功 */
        CK(ot_resize_finish(t, &e) == OT_OK);
        CK(ot_migrating(t) == 0);
        CK(ot_capacity(t) == plan.new_slots);

        for (uint32_t i = 0; i < N; ++i) {
            uint32_t v = 0;
            CK(ot_get(t, i, &v, &e) == OT_OK && v == ref[i].value);
            post_sum += v;
        }
        CK(pre_sum == post_sum);
        CK(export_matches_model(t, ref, N, "after-migrate"));
        CK(ot_resize_finish(t, &e) == OT_E_STATE);
        CK(ot_resize_rollback(t, &e) == OT_E_STATE);
        free(ref);
        ot_destroy(t);
    }

    /* ---- 旧表回滚：逐字节回到迁移前 ---- */
    {
        enum { N = 2000 };
        ObjTab *t = NULL;
        OtOptions o4 = ot_default_options();
        o4.max_entries = 100000; o4.max_slots = 1ull << 20;
        CK(ot_create(32, o4, &t, &e) == OT_OK);
        Ref *ref = ref_new(N);
        for (uint32_t i = 0; i < N; ++i) {
            ref[i].key = i * 3u; ref[i].value = rnd32(); ref[i].alive = 1;
            CK(ot_put(t, ref[i].key, ref[i].value, &e) == OT_OK);
        }
        uint64_t before_cap = ot_capacity(t), before_layout = 0, before_sum = 0;
        CK(ot_layout_checksum(t, &before_layout, &e) == OT_OK);
        OtPair *pre = NULL; size_t pre_n = 0;
        CK(ot_export(t, &pre, &pre_n, &e) == OT_OK);
        for (size_t i = 0; i < pre_n; ++i) before_sum += pre[i].value;

        OtPlan plan;
        CK(ot_plan_resize(t, 20000, &plan, &e) == OT_OK);
        CK(ot_resize_begin(t, &plan, &e) == OT_OK);
        /* 只迁一半就回滚 */
        int complete = 0;
        while (!complete && ot_migrated(t) < N / 2) {
            uint32_t moved = 0;
            CK(ot_migrate_step(t, 13, &moved, &complete, &e) == OT_OK);
        }
        CK(ot_migrated(t) > 0 && ot_migrated(t) < N);
        CK(ot_resize_rollback(t, &e) == OT_OK);
        CK(ot_migrating(t) == 0);
        CK(ot_capacity(t) == before_cap);

        uint64_t after_layout = 0;
        CK(ot_layout_checksum(t, &after_layout, &e) == OT_OK);
        CK(after_layout == before_layout);

        OtPair *post = NULL; size_t post_n = 0;
        CK(ot_export(t, &post, &post_n, &e) == OT_OK);
        CK(post_n == pre_n);
        CK(memcmp(pre, post, pre_n * sizeof(OtPair)) == 0);
        uint64_t after_sum = 0;
        for (size_t i = 0; i < post_n; ++i) after_sum += post[i].value;
        CK(after_sum == before_sum);

        /* 回滚后还能正常读写 */
        CK(ot_put(t, 123456789u, 42, &e) == OT_OK);
        uint32_t v = 0;
        CK(ot_get(t, 123456789u, &v, &e) == OT_OK && v == 42);
        free(pre); free(post); free(ref);
        ot_destroy(t);
    }

    /* ---- 计划过期 / 未开始就 finish / budget 为零 ---- */
    {
        ObjTab *t = NULL;
        OtOptions o5 = ot_default_options();
        o5.max_entries = 100000; o5.max_slots = 1ull << 20;
        CK(ot_create(64, o5, &t, &e) == OT_OK);
        for (uint32_t i = 0; i < 20; ++i) CK(ot_put(t, i, i, &e) == OT_OK);
        OtPlan p1;
        CK(ot_plan_resize(t, 1000, &p1, &e) == OT_OK);
        /* 计划后表发生新增 -> 计划过期 */
        CK(ot_put(t, 500, 500, &e) == OT_OK);
        CK(ot_resize_begin(t, &p1, &e) == OT_E_STALE_PLAN);
        /* 计划后写入同值不算变化 */
        OtPlan p1b;
        CK(ot_plan_resize(t, 1000, &p1b, &e) == OT_OK);
        CK(ot_put(t, 500, 500, &e) == OT_OK);
        CK(ot_resize_begin(t, &p1b, &e) == OT_OK);
        CK(ot_migrate_all(t, 8, NULL, &e) == OT_OK);
        CK(ot_resize_finish(t, &e) == OT_OK);

        OtPlan p2;
        CK(ot_plan_resize(t, 1000, &p2, &e) == OT_OK);
        CK(ot_resize_begin(t, &p2, &e) == OT_OK);
        uint32_t moved; int complete;
        CK(ot_migrate_step(t, 0, &moved, &complete, &e) == OT_E_ARG);
        CK(ot_resize_finish(t, &e) == OT_E_STATE); /* 尚未迁完 */
        /* 迁移中同样的 plan 再开一次要被拒绝 */
        CK(ot_resize_begin(t, &p2, &e) == OT_E_STATE);
        CK(ot_migrate_all(t, 0, NULL, &e) == OT_OK);
        CK(ot_migrate_step(t, 1, &moved, &complete, &e) == OT_OK && complete == 1);
        CK(ot_resize_finish(t, &e) == OT_OK);
        CK(ot_migrate_step(t, 1, &moved, &complete, &e) == OT_E_STATE);
        /* 迁移结束后旧 plan 过期 */
        CK(ot_resize_begin(t, &p2, &e) == OT_E_STALE_PLAN);
        CK(ot_plan_resize(t, 2000, &p1, &e) == OT_OK);
        ot_destroy(t);
    }

    /* ---- 缩容：条目数超过新规模要拒绝 ---- */
    {
        ObjTab *t = NULL;
        OtOptions o6 = ot_default_options();
        o6.max_entries = 100000; o6.max_slots = 1ull << 20;
        CK(ot_create(1024, o6, &t, &e) == OT_OK);
        for (uint32_t i = 0; i < 600; ++i) CK(ot_put(t, i, i, &e) == OT_OK);
        OtPlan p;
        CK(ot_plan_resize(t, 599, &p, &e) == OT_E_RANGE);
        CK(ot_plan_resize(t, 600, &p, &e) == OT_OK);
        CK(p.new_slots == 1024); /* ceil(600/.7)=858 -> 1024 */
        CK(p.growth == 1.0);
        CK(ot_resize(t, 600, 1, 128, NULL, &e) == OT_OK);
        CK(ot_resize_finish(t, &e) == OT_OK);
        int bad = 0;
        for (uint32_t i = 0; i < 600; ++i) {
            uint32_t v = 0;
            if (ot_get(t, i, &v, &e) != OT_OK || v != i) ++bad;
        }
        CK(bad == 0);
        ot_destroy(t);
    }

    /* ---- 增量迁移与整表重建结果完全一致（含输出顺序） ---- */
    {
        enum { N = 4000 };
        ObjTab *a = NULL, *b = NULL;
        OtOptions o7 = ot_default_options();
        o7.max_entries = 100000; o7.max_slots = 1ull << 20;
        CK(ot_create(128, o7, &a, &e) == OT_OK);
        uint32_t inserted = 0;
        for (uint32_t i = 0; i < N; ++i) {
            uint32_t key = (uint32_t)((uint64_t)i * 2654435761u >> 8);
            CK(ot_put(a, key, i, &e) == OT_OK);
            ++inserted;
        }
        /* 用完全相同的插入顺序建一份表 b，使两条路径从同一布局出发 */
        OtPair *arr = NULL; size_t m = 0;
        CK(ot_export(a, &arr, &m, &e) == OT_OK);
        CK(ot_create(128, o7, &b, &e) == OT_OK);
        for (uint32_t i = 0; i < N; ++i)
            CK(ot_put(b, (uint32_t)((uint64_t)i * 2654435761u >> 8), i, &e) == OT_OK);
        uint64_t pre_a = 0, pre_b = 0;
        CK(ot_layout_checksum(a, &pre_a, &e) == OT_OK);
        CK(ot_layout_checksum(b, &pre_b, &e) == OT_OK);
        CK(pre_a == pre_b); /* 对照前提：两条路径起点一致 */
        free(arr);

        OtResizeReport ra, rb;
        CK(ot_resize(a, 50000, 1, 256, &ra, &e) == OT_OK);
        CK(ot_resize_finish(a, &e) == OT_OK);
        CK(ot_resize(b, 50000, 0, 0, &rb, &e) == OT_OK);

        CK(ra.plan.new_slots == rb.plan.new_slots);
        CK(ra.migrated == rb.migrated);
        CK(ra.migrated == m);
        CK(m == N);
        CK(ra.incremental == 1 && rb.incremental == 0);
        CK(ra.steps > 1 && rb.steps == 1);
        CK(ra.max_step_moved <= 256);
        CK(inserted == N); /* N 个 key 均唯一，否则对照前提不成立 */

        uint64_t cka = 0, ckb = 0;
        CK(ot_layout_checksum(a, &cka, &e) == OT_OK);
        CK(ot_layout_checksum(b, &ckb, &e) == OT_OK);
        CK(cka == ckb); /* 同一组键值 -> 同一布局，逐槽相同 */

        OtPair *ea = NULL, *eb = NULL; size_t na = 0, nb = 0;
        CK(ot_export(a, &ea, &na, &e) == OT_OK);
        CK(ot_export(b, &eb, &nb, &e) == OT_OK);
        CK(na == nb && memcmp(ea, eb, na * sizeof(OtPair)) == 0);
        free(ea); free(eb);
        ot_destroy(a); ot_destroy(b);
    }

    /* ---- 随机增删 + 随机扩容，对照参考模型 ---- */
    {
        enum { KEYSPACE = 20000 };
        ObjTab *t = NULL;
        OtOptions o8 = ot_default_options();
        o8.max_entries = 200000; o8.max_slots = 1ull << 21;
        CK(ot_create(64, o8, &t, &e) == OT_OK);
        Ref *ref = ref_new(KEYSPACE);
        int bad = 0;
        for (int round = 0; round < 4000; ++round) {
            uint32_t k = rnd32() % KEYSPACE;
            if (rnd32() % 3 == 0) {
                int rc = ot_del(t, k, &e);
                if (ref[k].alive) { if (rc != OT_OK) ++bad; ref[k].alive = 0; }
                else if (rc != OT_E_NOTFOUND) ++bad;
            } else {
                uint32_t val = rnd32();
                if (ot_put(t, k, val, &e) != OT_OK) ++bad;
                ref[k].alive = 1; ref[k].value = val;
            }
            if (round % 500 == 499) {
                /* 随机换规模 */
                uint32_t scale = (uint32_t)(ot_count(t) + rnd32() % 5000);
                if (ot_resize(t, scale, 1, 64, NULL, &e) != OT_OK) ++bad;
                if (ot_resize_finish(t, &e) != OT_OK) ++bad;
            }
        }
        CK(bad == 0);
        {
            size_t live = 0;
            for (uint32_t i = 0; i < KEYSPACE; ++i) if (ref[i].alive) ++live;
            CK(ot_count(t) == (uint64_t)live);
        }
        CK(export_matches_model(t, ref, KEYSPACE, "random"));
        free(ref);
        ot_destroy(t);
    }

    /* ---- 墓碑堆积与运行时超限 ---- */
    {
        OtOptions o9 = ot_default_options();
        o9.max_entries = 200;     /* 逼出运行时 OT_E_LIMIT */
        o9.max_slots = 1024;
        ObjTab *t = NULL;
        CK(ot_create(64, o9, &t, &e) == OT_OK);
        int rc = OT_OK, limit_hit = 0;
        for (uint32_t i = 0; i < 500; ++i) {
            rc = ot_put(t, i, i, &e);
            if (rc == OT_E_LIMIT) { limit_hit = 1; break; }
            CK(rc == OT_OK);
        }
        CK(limit_hit == 1);
        CK(e.limit_entries == 200);
        CK(e.requested_entries == 201 || e.requested_entries > 200);
        /* 超限后表仍可用：已插入的键都能查到 */
        uint64_t live = ot_count(t);
        CK(live > 0 && live <= 200);
        int bad = 0;
        for (uint32_t i = 0; i < (uint32_t)live; ++i) {
            uint32_t v = 0;
            if (ot_get(t, i, &v, &e) != OT_OK || v != i) ++bad;
        }
        CK(bad == 0);

        /* 大量删除制造墓碑，再插入新键仍能找到 */
        ObjTab *t2 = NULL;
        CK(ot_create(64, ot_default_options(), &t2, &e) == OT_OK);
        for (uint32_t i = 0; i < 40; ++i) CK(ot_put(t2, i, i, &e) == OT_OK);
        for (uint32_t i = 0; i < 40; i += 2) CK(ot_del(t2, i, &e) == OT_OK);
        for (uint32_t i = 100; i < 140; ++i) CK(ot_put(t2, i, i, &e) == OT_OK);
        bad = 0;
        for (uint32_t i = 0; i < 40; ++i) {
            uint32_t v = 0;
            int r = ot_get(t2, i, &v, &e);
            if (i % 2 == 0) { if (r != OT_E_NOTFOUND) ++bad; }
            else if (r != OT_OK || v != i) ++bad;
        }
        for (uint32_t i = 100; i < 140; ++i) {
            uint32_t v = 0;
            if (ot_get(t2, i, &v, &e) != OT_OK || v != i) ++bad;
        }
        CK(bad == 0);
        ot_destroy(t2);
        ot_destroy(t);
    }

    /* ---- 单步预算上界：任何一步迁移条数不超过步数预算 ---- */
    {
        ObjTab *t = NULL;
        OtOptions o10 = ot_default_options();
        o10.max_entries = 100000; o10.max_slots = 1ull << 20;
        CK(ot_create(64, o10, &t, &e) == OT_OK);
        for (uint32_t i = 0; i < 5000; ++i) CK(ot_put(t, i, i, &e) == OT_OK);
        OtPlan p;
        CK(ot_plan_resize(t, 20000, &p, &e) == OT_OK);
        CK(ot_resize_begin(t, &p, &e) == OT_OK);
        int complete = 0;
        int over = 0;
        while (!complete) {
            uint32_t moved = 0;
            CK(ot_migrate_step(t, 7, &moved, &complete, &e) == OT_OK);
            if (moved > 7) ++over;
        }
        CK(over == 0);
        CK(ot_resize_finish(t, &e) == OT_OK);
        ot_destroy(t);
    }

    /* ---- 统计与字符串 ---- */
    {
        ObjTab *t = NULL;
        CK(ot_create(0, opt, &t, &e) == OT_OK);
        CK(ot_put(t, 1, 1, &e) == OT_OK);
        uint32_t v;
        CK(ot_get(t, 1, &v, &e) == OT_OK);
        CK(ot_get(t, 2, &v, &e) == OT_E_NOTFOUND);
        OtStats s;
        ot_stats(t, &s);
        CK(s.lookups == 2 && s.hits == 1 && s.misses == 1);
        CK(s.probes >= 2 && s.max_probes >= 1);
        ot_stats_reset(t);
        ot_stats(t, &s);
        CK(s.lookups == 0);
        CK(strcmp(ot_strerror(OT_E_LIMIT), "limit exceeded") == 0);
        CK(strcmp(ot_strerror(OT_OK), "ok") == 0);
        ot_destroy(t);
    }

    /* ---- 空表导出 / 空表扩容 ---- */
    {
        ObjTab *t = NULL;
        CK(ot_create(0, opt, &t, &e) == OT_OK);
        OtPair *arr = (OtPair *)0x1;
        size_t n = 123;
        CK(ot_export(t, &arr, &n, &e) == OT_OK && arr == NULL && n == 0);
        OtResizeReport r;
        CK(ot_resize(t, 100, 1, 8, &r, &e) == OT_OK);
        CK(ot_resize_finish(t, &e) == OT_OK);
        CK(r.migrated == 0);
        CK(ot_capacity(t) == 256); /* ceil(100/.7)=143 -> 256 */
        ot_destroy(t);
    }

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
