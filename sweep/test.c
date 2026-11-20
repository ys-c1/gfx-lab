#include "sweep.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

/* 记录检查点活动表, 供逐对象手算对照 */
typedef struct { int cp, type, evid, count; int ids[16]; } Snapshot;
static Snapshot snaps[64];
static int nsnap;
static void snap_fn(int cp, double x, int type, int evid,
                    const int *active, int count, void *user) {
    (void)x; (void)user;
    Snapshot *s = &snaps[nsnap++];
    s->cp = cp; s->type = type; s->evid = evid; s->count = count;
    memcpy(s->ids, active, sizeof(int) * (size_t)count);
}
static void snaps_reset(void) { nsnap = 0; }
static int snap_eq(int idx, const int *ids, int count) {
    if (idx >= nsnap || snaps[idx].count != count) return 0;
    for (int i = 0; i < count; ++i) if (snaps[idx].ids[i] != ids[i]) return 0;
    return 1;
}

/* 深度抖动: 奇偶 id 反向翻转, 使相邻检查点间真实发生相对次序颠倒。
 * 扫描线模式只在 ENTER 采样一次并持有 -> 无跳变;
 * 非扫描线模式每检查点重采样并整表重排 -> 次序来回跳。 */
static double jitter(const SweepObj *o, int cp, void *user) {
    (void)user;
    double sgn = (o->id & 1) ? 1.0 : -1.0;
    return o->depth + sgn * ((cp & 1) ? 0.4 : -0.4);
}

int main(void) {
    /* ---- 手算用例 5 对象; 同坐标 10: id4 EXIT 先于 id3 ENTER ---- */
    SweepObj o[5] = {
        {0,  0.0,  4.0, 2.0},
        {1,  1.0,  6.0, 1.0},
        {2,  2.0,  8.0, 3.0},
        {3, 10.0, 12.0, 0.5},
        {4,  3.0, 10.0, 1.5},
    };

    int bad = -99;
    CK(sweep_validate(o, 5, &bad) == SWEEP_OK);

    SweepEvent ev[10];
    CK(sweep_build_events(o, 5, ev) == SWEEP_OK);
    /* 手算事件次序: 按 x 升序, x=10 处 EXIT(4) 先于 ENTER(3) */
    CK(ev[0].type == SWEEP_EVENT_ENTER && ev[0].id == 0);   /* x=0  */
    CK(ev[1].type == SWEEP_EVENT_ENTER && ev[1].id == 1);   /* x=1  */
    CK(ev[2].type == SWEEP_EVENT_ENTER && ev[2].id == 2);   /* x=2  */
    CK(ev[3].type == SWEEP_EVENT_ENTER && ev[3].id == 4);   /* x=3  */
    CK(ev[4].type == SWEEP_EVENT_EXIT  && ev[4].id == 0);   /* x=4  */
    CK(ev[5].type == SWEEP_EVENT_EXIT  && ev[5].id == 1);   /* x=6  */
    CK(ev[6].type == SWEEP_EVENT_EXIT  && ev[6].id == 2);   /* x=8  */
    CK(ev[7].type == SWEEP_EVENT_EXIT  && ev[7].id == 4);   /* x=10 EXIT 先 */
    CK(ev[8].type == SWEEP_EVENT_ENTER && ev[8].id == 3);   /* x=10 ENTER 后 */
    CK(ev[9].type == SWEEP_EVENT_EXIT  && ev[9].id == 3);   /* x=12 */

    SweepOpt opt = {1, NULL, NULL, NULL, snap_fn, NULL};
    SweepStats st;
    snaps_reset();
    CK(sweep_run(o, 5, &opt, &st) == SWEEP_OK);

    /* 逐对象手算活动表(深度升序, id 破平) */
    int e0[] = {0};
    int e1[] = {1,0};                 /* d1=1 < d0=2 */
    int e2[] = {1,0,2};
    int e3[] = {1,4,0,2};             /* 1, 1.5, 2, 3 */
    int e4[] = {1,4,2};               /* x=4: 0 离开 */
    int e5[] = {4,2};                 /* x=6: 1 离开 */
    int e6[] = {4};                   /* x=8: 2 离开, 只剩 4 */
    int e7[] = {0};                   /* x=10: 4 离开, 空表 */
    int e8[] = {3};                   /* x=10: 3 进入 */
    int e9n = 0;                      /* x=12: 3 离开 */
    CK(snap_eq(0, e0, 1)); CK(snap_eq(1, e1, 2)); CK(snap_eq(2, e2, 3));
    CK(snap_eq(3, e3, 4)); CK(snap_eq(4, e4, 3)); CK(snap_eq(5, e5, 2));
    CK(snap_eq(6, e6, 1)); CK(snap_eq(7, e7, 0)); CK(snap_eq(8, e8, 1));
    CK(snaps[9].count == e9n);
    CK(nsnap == 10);

    CK(st.events == 10);
    CK(st.checkpoints == 10);
    CK(st.max_active == 4);
    CK(st.jumps == 0);                /* 持有序: 无跳变 */
    CK(st.errors == 0);               /* 与基准深度序完全一致 */
    /* 手算候选(各 ENTER 时活动数): 0(id0)+1(id1)+2(id2)+3(id4)+0+0+0+0+0(id3)+0 = 6 */
    CK(st.pair_candidates == 6);
    /* 集合变化: 5 次 ENTER 各引入 1 个新成员 = 5; EXIT 不引入新成员 */
    CK(st.set_changes == 5);

    /* ---- 暴力交叉验证: 每个事件坐标处与参考实现逐对象对照 ---- */
    double dep[5];
    for (int i = 0; i < 5; ++i) dep[i] = o[i].depth;
    /* 权威暴力对照: 独立重放同一份有序事件, 逐检查点核对活动集合与遮挡序。
     * 不依赖坐标开闭区间, 直接按 ENTER 加入 / EXIT 摘除, 与库同构。 */
    int ref_active[8], ref_n = 0;
    for (int k = 0; k < 10; ++k) {
        int eid = ev[k].id;
        if (ev[k].type == SWEEP_EVENT_ENTER) {
            int p = ref_n;
            while (p > 0 && (dep[ref_active[p - 1]] > dep[eid] ||
                             (dep[ref_active[p - 1]] == dep[eid] &&
                              ref_active[p - 1] > eid))) {
                ref_active[p] = ref_active[p - 1];
                --p;
            }
            ref_active[p] = eid;
            ++ref_n;
        } else {
            int p = 0;
            while (p < ref_n && ref_active[p] != eid) ++p;
            for (int j = p; j + 1 < ref_n; ++j) ref_active[j] = ref_active[j + 1];
            --ref_n;
        }
        CK(ref_n == snaps[k].count);
        for (int i = 0; i < ref_n; ++i) CK(ref_active[i] == snaps[k].ids[i]);
    }

    /* ---- 抖动: 扫描线模式持有深度 => 仍 0 跳变; 非扫描线整表重排 => 有跳变 ---- */
    SweepOpt jit = {1, jitter, NULL, NULL, NULL, NULL};
    SweepStats stj;
    CK(sweep_run(o, 5, &jit, &stj) == SWEEP_OK);
    CK(stj.jumps == 0);

    SweepOpt naive = {0, jitter, NULL, NULL, NULL, NULL};
    SweepStats stn;
    CK(sweep_run(o, 5, &naive, &stn) == SWEEP_OK);
    CK(stn.events == 0);             /* 不走扫描线: 无事件 */
    CK(stn.set_changes == 0);        /* 集合不变 */
    CK(stn.max_active == 5);         /* 恒为全集 */
    CK(stn.jumps > 0);               /* 抖动导致整表次序来回跳 */
    CK(stn.checkpoints == 100);

    /* ---- 错误: 扫描线为空 ---- */
    CK(sweep_validate(o, 0, &bad) == SWEEP_ERR_EMPTY);
    CK(bad == -1);
    CK(sweep_run(o, 0, NULL, &st) == SWEEP_ERR_EMPTY);
    CK(sweep_validate(NULL, 3, &bad) == SWEEP_ERR_NULL);

    /* ---- 错误: 区间倒置, 必须指出序号 ---- */
    SweepObj bad_objs[3] = {{0,0,5,1},{1,9,2,1},{2,0,5,1}};
    bad = -99;
    CK(sweep_validate(bad_objs, 3, &bad) == SWEEP_ERR_INVERTED);
    CK(bad == 1);
    CK(sweep_run(bad_objs, 3, NULL, &st) == SWEEP_ERR_INVERTED);

    SweepObj nan_objs[2] = {{0,0,5,1},{1,NAN,5,1}};
    bad = -99;
    CK(sweep_validate(nan_objs, 2, &bad) == SWEEP_ERR_INVERTED);
    CK(bad == 1);

    /* ---- 同深度 id 破平稳定性 ---- */
    SweepObj eq[3] = {{2,0,4,1.0},{0,0,4,1.0},{1,0,4,1.0}};
    snaps_reset();
    SweepOpt eqopt = {1, NULL, NULL, NULL, snap_fn, NULL};
    CK(sweep_run(eq, 3, &eqopt, &st) == SWEEP_OK);
    int eqorder[] = {0,1,2};
    CK(snap_eq(2, eqorder, 3));     /* 最后一次 ENTER 后次序 0,1,2 */
    CK(st.jumps == 0);

    /* ---- 活动表导出 CSV ---- */
    FILE *f = tmpfile();
    CK(f != NULL);
    int row[] = {3,1,2};
    CK(sweep_export_active(f, 6, 10.0, SWEEP_EVENT_ENTER, 3, row, 3) == SWEEP_OK);
    rewind(f);
    char buf[128];
    CK(fgets(buf, sizeof buf, f) != NULL);
    CK(strstr(buf, "6,10.000000,ENTER,3,3,\"3;1;2\"") != NULL);
    fclose(f);
    CK(sweep_export_active(NULL, 0, 0, 0, 0, row, 3) == SWEEP_ERR_NULL);

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
