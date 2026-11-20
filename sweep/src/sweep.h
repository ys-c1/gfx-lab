#ifndef SWEEP_H
#define SWEEP_H
#include <stdio.h>

/*
 * 两层遮挡排序:
 *   第一层 扫描线: 把每个对象的区间 [x0,x1] 拆成 ENTER/EXIT 两个事件,
 *                  扫描线推进时只收集覆盖当前位置的活动对象;
 *   第二层 事件序: ENTER 时按 (深度, id) 稳定插入活动表, EXIT 时摘除,
 *                  留在活动表中的对象相对次序永不翻转 -> 跳变为 0。
 *
 * 约定: id 为对象唯一编号, 取 0..n-1; 深度越小越靠前;
 *       同坐标事件 EXIT 先于 ENTER(端点相切不算同时活动)。
 */

#define SWEEP_OK             0
#define SWEEP_ERR_NULL      -1   /* 空指针 */
#define SWEEP_ERR_EMPTY     -2   /* 扫描线为空: 没有任何有效对象/事件 */
#define SWEEP_ERR_INVERTED  -3   /* 区间倒置: x0 > x1 (或 NaN) */
#define SWEEP_ERR_MEM       -4   /* 内存分配失败 */

#define SWEEP_EVENT_ENTER 0
#define SWEEP_EVENT_EXIT  1
#define SWEEP_EVENT_NONE (-1)   /* 非扫描线模式的检查点 */

typedef struct {
    int id;
    double x0, x1;   /* 扫描轴闭区间, 要求 x0 <= x1 */
    double depth;    /* 基准遮挡深度, 越小越靠前 */
} SweepObj;

typedef struct {
    int type;        /* SWEEP_EVENT_ENTER / SWEEP_EVENT_EXIT */
    double x;
    int id;
} SweepEvent;

/* 每个检查点的深度采样; 为 NULL 时恒取 obj->depth。
 * 演示程序用它模拟每帧深度抖动(z-fighting), 扫描线模式只在 ENTER
 * 时刻采样一次并持有, 非扫描线模式每个检查点重新采样、整表重排。 */
typedef double (*SweepDepthFn)(const SweepObj *obj, int checkpoint, void *user);

/* 检查点回调(可为 NULL): 给出当前活动表(已按遮挡次序排列), 用于测试逐对象核对 */
typedef void (*SweepSinkFn)(int checkpoint, double x, int event_type, int event_id,
                            const int *active, int count, void *user);

typedef struct {
    int use_sweep;               /* 1=扫描线两层; 0=不走扫描线(集合恒为全集) */
    SweepDepthFn depth_fn;       /* 可空 */
    void *user;
    FILE *csv;                   /* 可空: 活动表导出流, 运行器负责写表头 */
    SweepSinkFn sink;            /* 可空 */
    void *sink_user;
} SweepOpt;

typedef struct {
    int n;
    int checkpoints;             /* 检查点数 */
    int events;                  /* 事件数: 扫描线=2n, 非扫描线=0 */
    int max_active;              /* 活动表峰值 */
    long long compares;          /* 候选数: 深度比较实际调用次数 */
    long long pair_candidates;   /* 重叠候选对数: 各 ENTER 时活动对象数之和 */
    long long jumps;             /* 跳变量: 相邻检查点共同对象相对次序翻转对数 */
    long long errors;            /* 误差: 持有序相对本检查点实时深度序的逆序对 */
    long long ref_compares;      /* 实时参考排序所用比较数(误差度量的额外开销) */
    long long set_changes;       /* 集合变化次数; 非扫描线模式必须为 0 */
    long long algo_ns;           /* 算法净耗时(不含误差参考排序与导出) */
    long long elapsed_ns;        /* 总耗时(含误差度量/导出) */
} SweepStats;

/* 校验输入; 出错时 *bad_index 给出对象序号(EMPTY 时为 -1) */
int sweep_validate(const SweepObj *objs, int n, int *bad_index);

/* 构建并排序 2n 个事件(EXIT 先于同坐标 ENTER) */
int sweep_build_events(const SweepObj *objs, int n, SweepEvent *events);

/* 运行两层排序; opt 可为 NULL(等价于扫描线+恒深+无输出) */
int sweep_run(const SweepObj *objs, int n, const SweepOpt *opt, SweepStats *out);

/* 导出活动表一行 CSV: cp,x,event,obj_id,active_count,"id;id;..." */
int sweep_export_active(FILE *csv, int checkpoint, double x,
                        int event_type, int event_id,
                        const int *active, int count);

#endif
