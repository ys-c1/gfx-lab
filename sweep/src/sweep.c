#define _POSIX_C_SOURCE 199309L
#include "sweep.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#define NAIVE_CHECKPOINTS 100

/* ---------- 基础工具 ---------- */

static double hold_depth(const SweepObj *o, int cp, SweepDepthFn fn, void *user) {
    return fn ? fn(o, cp, user) : o->depth;
}

/* 事件排序键: x 升序, 同坐标 EXIT 先于 ENTER, 再按 id */
static int event_cmp(const void *a, const void *b) {
    const SweepEvent *ea = (const SweepEvent *)a;
    const SweepEvent *eb = (const SweepEvent *)b;
    if (ea->x < eb->x) return -1;
    if (ea->x > eb->x) return 1;
    if (ea->type != eb->type) return eb->type - ea->type; /* type 降序: EXIT(1) 在 ENTER(0) 前 */
    return ea->id - eb->id;
}

int sweep_build_events(const SweepObj *objs, int n, SweepEvent *events) {
    if (!objs || !events || n <= 0) return SWEEP_ERR_NULL;
    for (int i = 0; i < n; ++i) {
        events[2 * i]     = (SweepEvent){SWEEP_EVENT_ENTER, objs[i].x0, objs[i].id};
        events[2 * i + 1] = (SweepEvent){SWEEP_EVENT_EXIT,  objs[i].x1, objs[i].id};
    }
    qsort(events, (size_t)(2 * n), sizeof(SweepEvent), event_cmp);
    return SWEEP_OK;
}

int sweep_validate(const SweepObj *objs, int n, int *bad_index) {
    if (bad_index) *bad_index = -1;
    if (!objs || n < 0) return SWEEP_ERR_NULL;
    if (n == 0) return SWEEP_ERR_EMPTY;               /* 扫描线为空 */
    char *seen = calloc((size_t)n, 1);
    if (!seen) return SWEEP_ERR_MEM;
    for (int i = 0; i < n; ++i) {
        if (isnan(objs[i].x0) || isnan(objs[i].x1) ||
            isinf(objs[i].x0) || isinf(objs[i].x1) ||
            objs[i].x0 > objs[i].x1) {               /* 区间倒置, 指出序号 */
            if (bad_index) *bad_index = i;
            free(seen);
            return SWEEP_ERR_INVERTED;
        }
        if (objs[i].id < 0 || objs[i].id >= n || seen[objs[i].id]) {
            if (bad_index) *bad_index = i;
            free(seen);
            return SWEEP_ERR_NULL;
        }
        seen[objs[i].id] = 1;
    }
    free(seen);
    return SWEEP_OK;
}

/* ---------- 稳定排序 + 比较计数(候选数) ---------- */

static double *g_depth;
static long long *g_count;

static int counted_cmp(const void *a, const void *b) {
    int ia = *(const int *)a, ib = *(const int *)b;
    ++*g_count;
    if (g_depth[ia] < g_depth[ib]) return -1;
    if (g_depth[ia] > g_depth[ib]) return 1;
    return ia - ib;                                  /* id 破平 => 稳定 */
}

/* 归并排序, 稳定, 每次比较都计入 *count; 按 (depth[id], id) 升序 */
static void stable_sort_ids(int *a, int n, const double *depth, long long *count) {
    int *tmp = malloc(sizeof(int) * (size_t)n);
    if (!tmp) {                                      /* 内存退路: 计数 qsort */
        g_depth = (double *)depth;
        g_count = count;
        qsort(a, (size_t)n, sizeof(int), counted_cmp);
        return;
    }
    for (int width = 1; width < n; width <<= 1) {
        for (int lo = 0; lo < n; lo += 2 * width) {
            int mid = lo + width < n ? lo + width : n;
            int hi  = lo + 2 * width < n ? lo + 2 * width : n;
            int i = lo, j = mid, k = lo;
            while (i < mid && j < hi) {
                ++*count;
                if (depth[a[j]] < depth[a[i]] ||
                    (depth[a[j]] == depth[a[i]] && a[j] < a[i]))
                    tmp[k++] = a[j++];
                else
                    tmp[k++] = a[i++];
            }
            while (i < mid) tmp[k++] = a[i++];
            while (j < hi)  tmp[k++] = a[j++];
            memcpy(a + lo, tmp + lo, sizeof(int) * (size_t)(hi - lo));
        }
    }
    free(tmp);
}

/* ---------- 逆序对计数(跳变量/误差), Fenwick 树 ---------- */

static void bit_add(int *bit, int n, int i) {
    for (++i; i <= n; i += i & -i) ++bit[i];
}
static int bit_sum(const int *bit, int i) {
    int s = 0;
    for (++i; i > 0; i -= i & -i) s += bit[i];
    return s;
}

/* 跳变量: 当前表相对上一帧表, 仅统计两帧共同存活对象对的次序翻转。
 * scratch[id] 用作上一帧位置表(不在上一帧则记 -1)。 */
static long long order_jumps(const int *active, int m,
                             const int *prev, int prev_m,
                             int *bit, int *scratch, int n) {
    for (int i = 0; i < n; ++i) scratch[i] = -1;
    for (int k = 0; k < prev_m; ++k) scratch[prev[k]] = k;
    memset(bit, 0, sizeof(int) * (size_t)(n + 1));
    long long seen = 0, inv = 0;
    for (int k = 0; k < m; ++k) {
        int r = scratch[active[k]];
        if (r < 0) continue;                       /* 新进入对象, 无上一帧次序 */
        inv += seen - bit_sum(bit, r);
        bit_add(bit, n, r);
        ++seen;
    }
    return inv;
}

/* 实时误差: 当前(持有)活动表相对"本检查点按实时深度重排"参考序的逆序对。
 * ref_dep/ref_order 为 n 元工作缓冲, rankmap 复用 scratch(id->实时排名)。
 * 参考排序产生的比较计入 *extra_cmps(单独统计, 不计入两层算法本身的候选数)。 */
static long long realtime_errors(const SweepObj *objs, int cp,
                                 const int *active, int m,
                                 SweepDepthFn fn, void *user,
                                 double *ref_dep, int *ref_order,
                                 int *bit, int *rankmap, int n,
                                 long long *extra_cmps) {
    if (m <= 1) return 0;
    for (int k = 0; k < m; ++k) {
        int id = active[k];
        ref_order[k] = id;
        ref_dep[id] = fn ? fn(&objs[id], cp, user) : objs[id].depth;
    }
    long long cmps = 0;
    stable_sort_ids(ref_order, m, ref_dep, &cmps);
    if (extra_cmps) *extra_cmps += cmps;
    for (int k = 0; k < m; ++k) rankmap[ref_order[k]] = k;  /* 实时参考排名 */
    memset(bit, 0, sizeof(int) * (size_t)(n + 1));
    long long seen = 0, inv = 0;
    for (int k = 0; k < m; ++k) {                       /* 沿持有表顺序统计逆序 */
        int r = rankmap[active[k]];
        inv += seen - bit_sum(bit, r);
        bit_add(bit, n, r);
        ++seen;
    }
    return inv;
}

/* ---------- 活动表 CSV 导出 ---------- */

int sweep_export_active(FILE *csv, int checkpoint, double x,
                        int event_type, int event_id,
                        const int *active, int count) {
    if (!csv || !active) return SWEEP_ERR_NULL;
    if (fprintf(csv, "%d,%.6f,%s,%d,%d,\"", checkpoint, x,
                event_type == SWEEP_EVENT_ENTER ? "ENTER" :
                event_type == SWEEP_EVENT_EXIT  ? "EXIT"  : "NONE",
                event_id, count) < 0)
        return SWEEP_ERR_MEM;
    for (int i = 0; i < count; ++i)
        if (fprintf(csv, "%s%d", i ? ";" : "", active[i]) < 0)
            return SWEEP_ERR_MEM;
    if (fprintf(csv, "\"\n") < 0) return SWEEP_ERR_MEM;
    return SWEEP_OK;
}

/* ---------- 主运行器 ---------- */

int sweep_run(const SweepObj *objs, int n, const SweepOpt *opt, SweepStats *out) {
    int bad;
    int rc = sweep_validate(objs, n, &bad);
    if (rc != SWEEP_OK) return rc;

    SweepOpt def = {1, NULL, NULL, NULL, NULL, NULL};
    const SweepOpt *o = opt ? opt : &def;
    if (!out) return SWEEP_ERR_NULL;

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    memset(out, 0, sizeof(*out));
    out->n = n;

    int *base_order = malloc(sizeof(int) * (size_t)n);  /* 实时误差工作缓冲 */
    double *base_dep = malloc(sizeof(double) * (size_t)n);
    int *active = malloc(sizeof(int) * (size_t)n);
    int *prev = malloc(sizeof(int) * (size_t)n);
    double *held = malloc(sizeof(double) * (size_t)n);
    SweepEvent *events = malloc(sizeof(SweepEvent) * (size_t)(2 * n));
    int *bit = calloc((size_t)(n + 1), sizeof(int));
    char *mark = calloc((size_t)n, 1);
    int *scratch = malloc(sizeof(int) * (size_t)n);
    if (!base_order || !base_dep || !active || !prev ||
        !held || !events || !bit || !mark || !scratch) {
        free(base_order); free(base_dep); free(active); free(prev);
        free(held); free(events); free(bit); free(mark); free(scratch);
        return SWEEP_ERR_MEM;
    }

    int prev_count = 0;
    int cur = 0;                       /* 当前活动对象数(随 ENTER/EXIT 增减) */

    if (o->csv)
        fprintf(o->csv, "checkpoint,x,event,event_id,active_count,active_ids\n");

    if (o->use_sweep) {
        /* 第一层: 扫描线收集活动对象; 第二层: 入/出事件维护遮挡次序 */
        rc = sweep_build_events(objs, n, events);
        if (rc != SWEEP_OK) goto done;
        out->events = 2 * n;

        for (int e = 0; e < 2 * n; ++e) {
            struct timespec ta, tb;
            clock_gettime(CLOCK_MONOTONIC, &ta);
            int id = events[e].id, cp = e;
            if (events[e].type == SWEEP_EVENT_ENTER) {
                double d = hold_depth(&objs[id], cp, o->depth_fn, o->user);
                held[id] = d;
                /* 稳定插入: 从后往前平移, 每次深度比较计入候选数 */
                int pos = cur;
                while (pos > 0) {
                    ++out->compares;
                    int pid = active[pos - 1];
                    if (held[pid] < d || (held[pid] == d && pid < id)) break;
                    active[pos] = active[pos - 1];
                    --pos;
                }
                active[pos] = id;
                out->pair_candidates += cur;
                ++cur;
                if (cur > out->max_active) out->max_active = cur;
            } else {
                int pos = 0;
                while (pos < cur && active[pos] != id) ++pos;
                memmove(active + pos, active + pos + 1,
                        sizeof(int) * (size_t)(cur - pos - 1));
                --cur;
            }

            int m = cur;
            /* 跳变量: 与上一帧共同存活对象对的次序翻转(扫描线持有序 => 恒 0) */
            out->jumps  += order_jumps(active, m, prev, prev_count,
                                       bit, scratch, n);

            /* 集合变化: 本帧活动对象中不在上一帧集合里的个数 */
            for (int k = 0; k < prev_count; ++k) mark[prev[k]] = 1;
            long long changed = 0;
            for (int k = 0; k < m; ++k) if (!mark[active[k]]) ++changed;
            memset(mark, 0, (size_t)n);
            out->set_changes += changed;
            clock_gettime(CLOCK_MONOTONIC, &tb);
            out->algo_ns += (tb.tv_sec - ta.tv_sec) * 1000000000LL
                          + (tb.tv_nsec - ta.tv_nsec);

            /* 误差度量(含实时参考排序)与导出不计入算法净耗时 */
            out->errors += realtime_errors(objs, cp, active, m,
                                           o->depth_fn, o->user,
                                           base_dep, base_order, bit,
                                           scratch, n, &out->ref_compares);

            if (o->csv)
                sweep_export_active(o->csv, cp, events[e].x,
                                    events[e].type, id, active, m);
            if (o->sink)
                o->sink(cp, events[e].x, events[e].type, id, active, m, o->sink_user);
            memcpy(prev, active, sizeof(int) * (size_t)m);
            prev_count = m;
            ++out->checkpoints;
        }
    } else {
        /* 不走扫描线: 不构建事件, 集合恒为全集, 每个检查点整表重排 */
        double lo = objs[0].x0, hi = objs[0].x1;
        for (int i = 1; i < n; ++i) {
            if (objs[i].x0 < lo) lo = objs[i].x0;
            if (objs[i].x1 > hi) hi = objs[i].x1;
        }
        double *d = malloc(sizeof(double) * (size_t)n);
        if (!d) { rc = SWEEP_ERR_MEM; goto done; }
        for (int i = 0; i < n; ++i) active[i] = i;   /* 初始全集, 之后原地重排 */
        for (int cp = 0; cp < NAIVE_CHECKPOINTS; ++cp) {
            struct timespec ta, tb;
            clock_gettime(CLOCK_MONOTONIC, &ta);
            double x = lo + (hi - lo) * (cp + 0.5) / NAIVE_CHECKPOINTS;
            for (int i = 0; i < n; ++i)
                d[i] = hold_depth(&objs[i], cp, o->depth_fn, o->user);
            long long cmps = 0;
            stable_sort_ids(active, n, d, &cmps);
            out->compares += cmps;
            out->max_active = n;                       /* 集合恒为全集 */
            out->jumps  += order_jumps(active, n, prev, prev_count,
                                       bit, scratch, n);
            /* 本就按实时深度排序, 持有序即实时序 => 误差恒 0, 无需再排一次 */
            out->errors += 0;
            memcpy(prev, active, sizeof(int) * (size_t)n);
            prev_count = n;
            clock_gettime(CLOCK_MONOTONIC, &tb);
            out->algo_ns += (tb.tv_sec - ta.tv_sec) * 1000000000LL
                          + (tb.tv_nsec - ta.tv_nsec);
            if (o->csv)
                sweep_export_active(o->csv, cp, x, SWEEP_EVENT_NONE, -1, active, n);
            if (o->sink)
                o->sink(cp, x, SWEEP_EVENT_NONE, -1, active, n, o->sink_user);
            ++out->checkpoints;
        }
        free(d);
    }

done:
    clock_gettime(CLOCK_MONOTONIC, &t1);
    out->elapsed_ns = (long long)(t1.tv_sec - t0.tv_sec) * 1000000000LL
                    + (t1.tv_nsec - t0.tv_nsec);
    free(base_order); free(base_dep); free(active); free(prev);
    free(held); free(events); free(bit); free(mark); free(scratch);
    return rc;
}
