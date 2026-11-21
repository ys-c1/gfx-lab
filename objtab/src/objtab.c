#include "objtab.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    OtPair *pairs;
    uint8_t *state;   /* 0 = 空, 1 = 占用, 2 = 墓碑 */
    uint64_t cap;
    uint64_t used;    /* state==1 的槽位数 */
    uint64_t filled;  /* state!=0 的槽位数（used + 墓碑） */
} Tab;

struct ObjTab {
    Tab cur;
    Tab old;          /* 迁移源；增量迁移期间保持只读 */
    int migrating;
    int old_complete;
    uint64_t scan_pos;   /* 迁移扫描游标（旧槽位下标） */
    uint64_t migrated;   /* 已迁移条数 */
    uint64_t migrate_entries;
    OtPlan plan;
    OtOptions opt;
    OtStats stats;
    uint64_t gen;     /* 表内容版本：任何写入/迁移完成/回滚都会递增 */
};

/* ---------------- 基础工具 ---------------- */

static void fail(OtError *err, int code) {
    if (!err) return;
    memset(err, 0, sizeof(*err));
    err->code = code;
}

static uint32_t hash_u32(uint32_t x) {
    /* murmur3 finalizer */
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

static int is_pow2(uint64_t v) { return v != 0 && (v & (v - 1)) == 0; }

static uint64_t next_pow2(uint64_t v) {
    uint64_t c = OT_MIN_SLOTS;
    while (c < v) {
        if (c > (UINT64_MAX >> 1)) return 0; /* 溢出 */
        c <<= 1;
    }
    return c;
}

static void tab_reset(Tab *tb) {
    tb->pairs = NULL;
    tb->state = NULL;
    tb->cap = 0;
    tb->used = 0;
    tb->filled = 0;
}

static int tab_alloc(Tab *tb, uint64_t cap) {
    tab_reset(tb);
    if (cap == 0 || !is_pow2(cap)) return OT_E_ARG;
    tb->pairs = (OtPair *)calloc((size_t)cap, sizeof(OtPair));
    tb->state = (uint8_t *)calloc((size_t)cap, 1);
    if (!tb->pairs || !tb->state) {
        free(tb->pairs);
        free(tb->state);
        tab_reset(tb);
        return OT_E_NOMEM;
    }
    tb->cap = cap;
    return OT_OK;
}

static void tab_free(Tab *tb) {
    free(tb->pairs);
    free(tb->state);
    tab_reset(tb);
}

/* 查找 key。找到返回 1 并写入 *val；否则返回 0。*probes 累加检查的槽位数。 */
static int tab_lookup(const Tab *tb, uint32_t key, uint32_t *val, uint64_t *probes) {
    if (tb->cap == 0) return 0;
    uint64_t mask = tb->cap - 1;
    uint64_t i = hash_u32(key) & mask;
    for (uint64_t n = 0; n < tb->cap; ++n) {
        ++*probes;
        uint8_t st = tb->state[i];
        if (st == 0) return 0;
        if (st == 1 && tb->pairs[i].key == key) {
            if (val) *val = tb->pairs[i].value;
            return 1;
        }
        i = (i + 1) & mask;
    }
    return 0;
}

/* 找 key 应落到的槽位：存在则 *exists=1 且 *slot 指向它；
 * 否则 *exists=0 且 *slot 指向首个空槽或最早的墓碑。表满返回 OT_E_FULL。 */
static int tab_find_slot(const Tab *tb, uint32_t key, uint64_t *slot, int *exists) {
    uint64_t mask = tb->cap - 1;
    uint64_t i = hash_u32(key) & mask;
    uint64_t tomb = UINT64_MAX;
    for (uint64_t n = 0; n < tb->cap; ++n) {
        uint8_t st = tb->state[i];
        if (st == 0) {
            *slot = (tomb != UINT64_MAX) ? tomb : i;
            *exists = 0;
            return OT_OK;
        }
        if (st == 1 && tb->pairs[i].key == key) {
            *slot = i;
            *exists = 1;
            return OT_OK;
        }
        if (st == 2 && tomb == UINT64_MAX) tomb = i;
        i = (i + 1) & mask;
    }
    if (tomb != UINT64_MAX) {
        *slot = tomb;
        *exists = 0;
        return OT_OK;
    }
    return OT_E_FULL;
}

/* 向已有容量的表插入。新增返回 1，覆盖返回 0，负值为错误。 */
static int tab_insert_raw(Tab *tb, uint32_t key, uint32_t value) {
    uint64_t slot = 0;
    int exists = 0;
    int rc = tab_find_slot(tb, key, &slot, &exists);
    if (rc != OT_OK) return rc;
    if (exists) {
        tb->pairs[slot].value = value;
        return 0;
    }
    int was_empty = tb->state[slot] == 0;
    tb->pairs[slot].key = key;
    tb->pairs[slot].value = value;
    tb->state[slot] = 1;
    ++tb->used;
    if (was_empty) ++tb->filled;
    return 1;
}

/* 原地压紧墓碑。成功返回 OT_OK；分配失败时保持原表不变并返回错误。 */
static int tab_compact(Tab *tb) {
    Tab nt;
    int rc = tab_alloc(&nt, tb->cap);
    if (rc != OT_OK) return rc;
    for (uint64_t i = 0; i < tb->cap; ++i)
        if (tb->state[i] == 1)
            tab_insert_raw(&nt, tb->pairs[i].key, tb->pairs[i].value);
    tab_free(tb);
    *tb = nt;
    return OT_OK;
}

/* ---------------- 规模规划 ---------------- */

static int plan_for(const ObjTab *t, uint32_t new_scale, OtPlan *plan, OtError *err) {
    if (!t || !plan) { fail(err, OT_E_ARG); return OT_E_ARG; }

    double lf = t->opt.load_factor;
    if (!(lf > 0.0 && lf < 1.0)) lf = OT_DEFAULT_LOAD;

    uint32_t max_entries = t->opt.max_entries ? t->opt.max_entries : OT_DEFAULT_MAX_ENTRIES;
    uint64_t max_slots = t->opt.max_slots ? t->opt.max_slots : OT_DEFAULT_MAX_SLOTS;

    memset(plan, 0, sizeof(*plan));
    plan->new_scale = new_scale;
    plan->load_factor = lf;
    plan->old_slots = t->cur.cap;
    plan->migrate_entries = t->cur.used;
    plan->table_gen = t->gen;

    if (new_scale < t->cur.used) {
        fail(err, OT_E_RANGE);
        return OT_E_RANGE;
    }

    /* 需要的最小槽位数：new_scale <= slots * lf */
    long double need = (long double)new_scale / (long double)lf;
    uint64_t need_slots = (need >= (long double)UINT64_MAX) ? 0 : (uint64_t)need;
    if (need_slots < new_scale) need_slots = new_scale; /* 防浮点截断 */
    uint64_t slots = next_pow2(need_slots);
    if (slots == 0) {
        fail(err, OT_E_LIMIT);
        if (err) {
            err->limit_slots = max_slots;
            err->requested_slots = UINT64_MAX;
            err->limit_entries = max_entries;
            err->requested_entries = new_scale;
        }
        return OT_E_LIMIT;
    }
    plan->new_slots = slots;

    if (new_scale > max_entries || slots > max_slots) {
        fail(err, OT_E_LIMIT);
        if (err) {
            err->limit_slots = max_slots;
            err->requested_slots = slots;
            err->limit_entries = max_entries;
            err->requested_entries = new_scale;
        }
        return OT_E_LIMIT;
    }
    plan->growth = (double)slots / (double)(t->cur.cap ? t->cur.cap : 1);
    return OT_OK;
}

/* ---------------- 生命周期 ---------------- */

OtOptions ot_default_options(void) {
    OtOptions o;
    o.incremental = 1;
    o.max_entries = OT_DEFAULT_MAX_ENTRIES;
    o.max_slots = OT_DEFAULT_MAX_SLOTS;
    o.load_factor = OT_DEFAULT_LOAD;
    return o;
}

int ot_create(uint64_t initial_slots, OtOptions opt, ObjTab **out, OtError *err) {
    if (!out) { fail(err, OT_E_ARG); return OT_E_ARG; }
    *out = NULL;
    if (opt.max_entries == 0) opt.max_entries = OT_DEFAULT_MAX_ENTRIES;
    if (opt.max_slots == 0) opt.max_slots = OT_DEFAULT_MAX_SLOTS;
    if (!(opt.load_factor > 0.0 && opt.load_factor < 1.0)) opt.load_factor = OT_DEFAULT_LOAD;

    uint64_t cap = next_pow2(initial_slots ? initial_slots : OT_MIN_SLOTS);
    if (cap == 0 || cap > opt.max_slots) {
        fail(err, OT_E_LIMIT);
        if (err) {
            err->limit_slots = opt.max_slots;
            err->requested_slots = cap ? cap : UINT64_MAX;
            err->limit_entries = opt.max_entries;
        }
        return OT_E_LIMIT;
    }

    ObjTab *t = (ObjTab *)calloc(1, sizeof(ObjTab));
    if (!t) { fail(err, OT_E_NOMEM); return OT_E_NOMEM; }
    int rc = tab_alloc(&t->cur, cap);
    if (rc != OT_OK) { free(t); fail(err, rc); return rc; }
    tab_reset(&t->old);
    t->opt = opt;
    *out = t;
    fail(err, OT_OK);
    return OT_OK;
}

void ot_destroy(ObjTab *t) {
    if (!t) return;
    tab_free(&t->cur);
    tab_free(&t->old);
    free(t);
}

int ot_put(ObjTab *t, uint32_t key, uint32_t value, OtError *err) {
    if (!t) { fail(err, OT_E_ARG); return OT_E_ARG; }
    if (t->migrating) { fail(err, OT_E_MIGRATING); return OT_E_MIGRATING; }

    uint64_t slot = 0;
    int exists = 0;
    int rc = tab_find_slot(&t->cur, key, &slot, &exists);

    /* 条目数硬上限：新增条目一旦超限直接拒绝，并给出上限值 */
    if (!exists) {
        uint32_t max_entries = t->opt.max_entries ? t->opt.max_entries
                                                  : OT_DEFAULT_MAX_ENTRIES;
        if (t->cur.used + 1 > (uint64_t)max_entries) {
            fail(err, OT_E_LIMIT);
            if (err) {
                err->limit_entries = max_entries;
                err->requested_entries = (uint32_t)(t->cur.used + 1);
                err->limit_slots = t->opt.max_slots ? t->opt.max_slots
                                                    : OT_DEFAULT_MAX_SLOTS;
                err->requested_slots = t->cur.cap;
            }
            return OT_E_LIMIT;
        }
    }

    /* 探测链被墓碑塞满：先原地压紧（容量不变），再重试；压紧失败则扩容 */
    if (!exists && rc == OT_E_FULL) {
        rc = tab_compact(&t->cur);
        if (rc != OT_OK) {
            OtPlan plan;
            rc = plan_for(t, (uint32_t)(t->cur.used + 1), &plan, err);
            if (rc != OT_OK) return rc;
            rc = ot_resize_full(t, plan.new_scale, NULL, err);
            if (rc != OT_OK) return rc;
        }
        rc = tab_find_slot(&t->cur, key, &slot, &exists);
    }

    /* 负载因子到顶：按使用量扩容 */
    if (!exists && rc == OT_OK) {
        double lf = t->opt.load_factor;
        if ((double)(t->cur.used + 1) > (double)t->cur.cap * lf) {
            OtPlan plan;
            rc = plan_for(t, (uint32_t)(t->cur.used + 1), &plan, err);
            if (rc != OT_OK) return rc;
            rc = ot_resize_full(t, plan.new_scale, NULL, err);
            if (rc != OT_OK) return rc;
            rc = tab_find_slot(&t->cur, key, &slot, &exists);
        }
    }
    if (rc != OT_OK) { fail(err, rc); return rc; }

    int was_empty = t->cur.state[slot] == 0;
    if (!exists) {
        t->cur.pairs[slot].key = key;
        t->cur.pairs[slot].value = value;
        t->cur.state[slot] = 1;
        ++t->cur.used;
        if (was_empty) ++t->cur.filled;
        ++t->gen;
    } else if (t->cur.pairs[slot].value != value) {
        t->cur.pairs[slot].value = value;
        ++t->gen;
    }
    fail(err, OT_OK);
    return OT_OK;
}

int ot_get(ObjTab *t, uint32_t key, uint32_t *value, OtError *err) {
    if (!t) { fail(err, OT_E_ARG); return OT_E_ARG; }
    uint64_t probes = 0;
    ++t->stats.lookups;
    int found = tab_lookup(&t->cur, key, value, &probes);
    /* 迁移中：新表未覆盖到的条目仍留在旧表，旧表只读 */
    if (!found && t->migrating && t->old.cap)
        found = tab_lookup(&t->old, key, value, &probes);
    t->stats.probes += probes;
    if (probes > t->stats.max_probes) t->stats.max_probes = probes;
    if (found) {
        ++t->stats.hits;
        fail(err, OT_OK);
        return OT_OK;
    }
    ++t->stats.misses;
    fail(err, OT_E_NOTFOUND);
    return OT_E_NOTFOUND;
}

int ot_del(ObjTab *t, uint32_t key, OtError *err) {
    if (!t) { fail(err, OT_E_ARG); return OT_E_ARG; }
    if (t->migrating) { fail(err, OT_E_MIGRATING); return OT_E_MIGRATING; }

    uint64_t mask = t->cur.cap - 1;
    uint64_t i = hash_u32(key) & mask;
    for (uint64_t n = 0; n < t->cur.cap; ++n) {
        uint8_t st = t->cur.state[i];
        if (st == 0) { fail(err, OT_E_NOTFOUND); return OT_E_NOTFOUND; }
        if (st == 1 && t->cur.pairs[i].key == key) {
            t->cur.state[i] = 2;
            --t->cur.used;
            /* 墓碑过多时压紧，避免探测链无限增长 */
            if (t->cur.filled > t->cur.cap / 2 && t->cur.used * 4 < t->cur.filled)
                tab_compact(&t->cur);
            ++t->gen;
            fail(err, OT_OK);
            return OT_OK;
        }
        i = (i + 1) & mask;
    }
    fail(err, OT_E_NOTFOUND);
    return OT_E_NOTFOUND;
}

/* ---------------- 两段式扩缩容 ---------------- */

int ot_plan_resize(const ObjTab *t, uint32_t new_scale, OtPlan *plan, OtError *err) {
    if (!t || !plan) { fail(err, OT_E_ARG); return OT_E_ARG; }
    if (t->migrating) { fail(err, OT_E_STATE); return OT_E_STATE; }
    return plan_for(t, new_scale, plan, err);
}

int ot_resize_begin(ObjTab *t, const OtPlan *plan, OtError *err) {
    if (!t || !plan) { fail(err, OT_E_ARG); return OT_E_ARG; }
    if (t->migrating) { fail(err, OT_E_STATE); return OT_E_STATE; }

    OtPlan expect;
    int rc = plan_for(t, plan->new_scale, &expect, err);
    if (rc != OT_OK) return rc;
    if (plan->old_slots != expect.old_slots || plan->new_slots != expect.new_slots ||
        plan->table_gen != expect.table_gen) {
        fail(err, OT_E_STALE_PLAN);
        return OT_E_STALE_PLAN;
    }

    Tab nt;
    rc = tab_alloc(&nt, expect.new_slots);
    if (rc != OT_OK) { fail(err, rc); return rc; }

    /* 旧表整体移交，迁移期间只读；新表初始为空 */
    t->old = t->cur;
    t->cur = nt;
    t->migrating = 1;
    t->old_complete = 0;
    t->scan_pos = 0;
    t->migrated = 0;
    t->migrate_entries = t->old.used;
    t->plan = expect;
    fail(err, OT_OK);
    return OT_OK;
}
int ot_migrate_step(ObjTab *t, uint32_t budget, uint32_t *moved, int *complete, OtError *err) {
    if (!t || !moved || !complete) { fail(err, OT_E_ARG); return OT_E_ARG; }
    if (!t->migrating) { fail(err, OT_E_STATE); return OT_E_STATE; }
    if (budget == 0) { fail(err, OT_E_ARG); return OT_E_ARG; }

    /* 单步工作量有上界：最多扫描 budget 个旧槽位。
     * 只在本槽成功处理后才推进游标，失败时可原样重试。 */
    *moved = 0;
    uint64_t scanned = 0;
    while (scanned < budget && t->scan_pos < t->old.cap) {
        uint64_t pos = t->scan_pos;
        if (t->old.state[pos] == 1) {
            int r = tab_insert_raw(&t->cur, t->old.pairs[pos].key, t->old.pairs[pos].value);
            if (r < 0) { fail(err, r); return r; }
            ++*moved;
            ++t->migrated;
        }
        t->scan_pos = pos + 1;
        ++scanned;
    }
    if (t->scan_pos >= t->old.cap) {
        t->old_complete = 1;
        *complete = 1;
    } else {
        *complete = 0;
    }
    fail(err, OT_OK);
    return OT_OK;
}

int ot_migrate_all(ObjTab *t, uint32_t batch, OtResizeReport *report, OtError *err) {
    if (!t) { fail(err, OT_E_ARG); return OT_E_ARG; }
    if (!t->migrating) { fail(err, OT_E_STATE); return OT_E_STATE; }
    if (batch == 0) batch = 1024;

    uint64_t steps = 0, max_moved = 0;
    int complete = 0;
    while (!complete) {
        uint32_t moved = 0;
        int rc = ot_migrate_step(t, batch, &moved, &complete, err);
        if (rc != OT_OK) return rc;
        ++steps;
        if (moved > max_moved) max_moved = moved;
    }
    if (report) {
        memset(report, 0, sizeof(*report));
        report->plan = t->plan;
        report->incremental = 1;
        report->migrated = t->migrated;
        report->steps = steps;
        report->max_step_moved = max_moved;
    }
    fail(err, OT_OK);
    return OT_OK;
}

int ot_resize_finish(ObjTab *t, OtError *err) {
    if (!t) { fail(err, OT_E_ARG); return OT_E_ARG; }
    if (!t->migrating || !t->old_complete || t->migrated != t->migrate_entries) {
        fail(err, OT_E_STATE);
        return OT_E_STATE;
    }
    tab_free(&t->old);
    t->migrating = 0;
    t->old_complete = 0;
    t->scan_pos = 0;
    ++t->gen;
    fail(err, OT_OK);
    return OT_OK;
}

int ot_resize_rollback(ObjTab *t, OtError *err) {
    if (!t) { fail(err, OT_E_ARG); return OT_E_ARG; }
    if (!t->migrating) { fail(err, OT_E_STATE); return OT_E_STATE; }
    /* 旧表在迁移期间一行未改，丢弃新表即精确回滚 */
    tab_free(&t->cur);
    t->cur = t->old;
    tab_reset(&t->old);
    t->migrating = 0;
    t->old_complete = 0;
    t->scan_pos = 0;
    t->migrated = 0;
    t->migrate_entries = 0;
    ++t->gen;
    fail(err, OT_OK);
    return OT_OK;
}

int ot_resize_full(ObjTab *t, uint32_t new_scale, OtResizeReport *report, OtError *err) {
    if (!t) { fail(err, OT_E_ARG); return OT_E_ARG; }
    if (t->migrating) { fail(err, OT_E_MIGRATING); return OT_E_MIGRATING; }

    OtPlan plan;
    int rc = plan_for(t, new_scale, &plan, err);
    if (rc != OT_OK) return rc;

    Tab nt;
    rc = tab_alloc(&nt, plan.new_slots);
    if (rc != OT_OK) { fail(err, rc); return rc; }

    uint64_t moved = 0;
    for (uint64_t i = 0; i < t->cur.cap; ++i) {
        if (t->cur.state[i] == 1) {
            int r = tab_insert_raw(&nt, t->cur.pairs[i].key, t->cur.pairs[i].value);
            if (r < 0) { tab_free(&nt); fail(err, r); return r; }
            ++moved;
        }
    }
    tab_free(&t->cur);
    t->cur = nt;
    ++t->gen;
    if (report) {
        memset(report, 0, sizeof(*report));
        report->plan = plan;
        report->incremental = 0;
        report->migrated = moved;
        report->steps = 1;
        report->max_step_moved = moved;
    }
    fail(err, OT_OK);
    return OT_OK;
}

int ot_resize(ObjTab *t, uint32_t new_scale, int incremental, uint32_t batch,
              OtResizeReport *report, OtError *err) {
    if (!t) { fail(err, OT_E_ARG); return OT_E_ARG; }
    if (!incremental) return ot_resize_full(t, new_scale, report, err);

    OtPlan plan;
    int rc = ot_plan_resize(t, new_scale, &plan, err);
    if (rc != OT_OK) return rc;
    rc = ot_resize_begin(t, &plan, err);
    if (rc != OT_OK) return rc;
    return ot_migrate_all(t, batch, report, err);
}

/* ---------------- 导出与观测 ---------------- */

static int cmp_pair(const void *a, const void *b) {
    const OtPair *x = (const OtPair *)a, *y = (const OtPair *)b;
    return (x->key > y->key) - (x->key < y->key);
}

int ot_export(const ObjTab *t, OtPair **out, size_t *n, OtError *err) {
    if (!t || !out || !n) { fail(err, OT_E_ARG); return OT_E_ARG; }
    *out = NULL;
    *n = 0;
    /* 迁移中：新表里已迁入 + 旧表里尚未扫描到的，合起来正好是全部逻辑条目 */
    uint64_t total;
    if (t->migrating) {
        total = t->cur.used + (t->old.used - t->migrated);
    } else {
        total = t->cur.used;
    }
    if (total == 0) { fail(err, OT_OK); return OT_OK; }
    OtPair *arr = (OtPair *)malloc((size_t)total * sizeof(OtPair));
    if (!arr) { fail(err, OT_E_NOMEM); return OT_E_NOMEM; }
    size_t k = 0;
    for (uint64_t i = 0; i < t->cur.cap; ++i)
        if (t->cur.state[i] == 1) arr[k++] = t->cur.pairs[i];
    if (t->migrating)
        for (uint64_t i = t->scan_pos; i < t->old.cap; ++i)
            if (t->old.state[i] == 1) arr[k++] = t->old.pairs[i];
    qsort(arr, k, sizeof(OtPair), cmp_pair);
    /* 迁移中两端不应有重复键；有则说明写入未被拦住 */
    for (size_t i = 1; i < k; ++i) {
        if (arr[i].key == arr[i - 1].key) {
            free(arr);
            fail(err, OT_E_STATE);
            return OT_E_STATE;
        }
    }
    *out = arr;
    *n = k;
    fail(err, OT_OK);
    return OT_OK;
}

int ot_layout_checksum(const ObjTab *t, uint64_t *out, OtError *err) {
    if (!t || !out) { fail(err, OT_E_ARG); return OT_E_ARG; }
    uint64_t h = 1469598103934665603ull;
    uint64_t v = t->cur.cap;
    for (int b = 0; b < 8; ++b) { h ^= (v >> (b * 8)) & 0xff; h *= 1099511628211ull; }
    for (uint64_t i = 0; i < t->cur.cap; ++i) {
        h ^= t->cur.state[i]; h *= 1099511628211ull;
        if (t->cur.state[i] == 1) {
            uint32_t key = t->cur.pairs[i].key, val = t->cur.pairs[i].value;
            for (int b = 0; b < 4; ++b) { h ^= (key >> (b * 8)) & 0xff; h *= 1099511628211ull; }
            for (int b = 0; b < 4; ++b) { h ^= (val >> (b * 8)) & 0xff; h *= 1099511628211ull; }
        }
    }
    *out = h;
    fail(err, OT_OK);
    return OT_OK;
}

uint64_t ot_capacity(const ObjTab *t) { return t ? t->cur.cap : 0; }

int ot_layout_snapshot(const ObjTab *t, OtSlot **out, uint64_t *cap, OtError *err) {
    if (!t || !out || !cap) { fail(err, OT_E_ARG); return OT_E_ARG; }
    *out = NULL;
    *cap = 0;
    if (t->migrating) { fail(err, OT_E_STATE); return OT_E_STATE; } /* 迁移中布局未定 */
    if (t->cur.cap == 0) { fail(err, OT_OK); return OT_OK; }
    OtSlot *arr = (OtSlot *)calloc((size_t)t->cur.cap, sizeof(OtSlot));
    if (!arr) { fail(err, OT_E_NOMEM); return OT_E_NOMEM; }
    for (uint64_t i = 0; i < t->cur.cap; ++i) {
        arr[i].state = t->cur.state[i];
        if (t->cur.state[i] == 1) {
            arr[i].key = t->cur.pairs[i].key;
            arr[i].value = t->cur.pairs[i].value;
        }
    }
    *out = arr;
    *cap = t->cur.cap;
    fail(err, OT_OK);
    return OT_OK;
}

/* 逻辑条目数：迁移中旧表仍持有全部条目（新表内容是其中已迁走的子集） */
uint64_t ot_count(const ObjTab *t) {
    if (!t) return 0;
    return t->migrating ? t->old.used : t->cur.used;
}

int ot_migrating(const ObjTab *t) { return t ? t->migrating : 0; }
uint64_t ot_old_capacity(const ObjTab *t) { return (t && t->migrating) ? t->old.cap : 0; }
uint64_t ot_migrated(const ObjTab *t) { return t ? t->migrated : 0; }

void ot_stats(const ObjTab *t, OtStats *out) {
    if (!t || !out) return;
    *out = t->stats;
}

void ot_stats_reset(ObjTab *t) {
    if (!t) return;
    memset(&t->stats, 0, sizeof(t->stats));
}

const char *ot_strerror(int code) {
    switch (code) {
    case OT_OK: return "ok";
    case OT_E_ARG: return "invalid argument";
    case OT_E_NOMEM: return "out of memory";
    case OT_E_FULL: return "table full";
    case OT_E_LIMIT: return "limit exceeded";
    case OT_E_MIGRATING: return "table is migrating";
    case OT_E_STATE: return "invalid state";
    case OT_E_NOTFOUND: return "key not found";
    case OT_E_RANGE: return "new scale below entry count";
    case OT_E_STALE_PLAN: return "resize plan is stale";
    default: return "unknown error";
    }
}
